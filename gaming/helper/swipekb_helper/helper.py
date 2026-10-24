# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""The helper's main loop (plan Sections 3 and 6).

Single thread, no framework:  find Steam's debug port -> watch its pages -> check the contract -> install the
layer stub -> keep it there (Steam restarts, UI reloads, our own restarts) -> report status.

Failure rule everywhere: do nothing and leave Steam's own keyboard alone. We only ever ADD things to a page
(one window property, one attribute, one timer) and can always remove them.
"""
import collections
import json
import os
import select
import threading
import time
from pathlib import Path

from . import LAYER_VERSION, cdp, config, contract, exposure, ws
from .bridge import Bridge, SendFailed
from .retry import RetryLedger
from .status import Status

_STUB_JS = Path(__file__).with_name('layer_stub.js')


SWITCH_OFF_REASON = ("Steam's developer switch was turned off. Swipe in Gaming Mode is off; Steam's normal keyboard is in use. "
                     "To turn it back on, run the Swipe Keyboard installer again (or choose Turn it back on in the notification).")


class PageSession:
    """Our open websocket to one of Steam's pages that carries a stub."""

    def __init__(self, target_id, c):
        self.id = target_id
        self.cdp = c
        self.last_beat = time.monotonic()
        self.since = time.monotonic()   # when this copy of the layer went in; a layer that stays and answers is forgiven past failures
        self.lost = []                  # times the layer was found missing and put back (M9: a page that keeps removing it is a failure)
        self.report = {}                # what the layer said about itself at the last ping
        # WHY (review, 2026-10-23): the binding exists in every document and frame of this page, so a call is only taken
        # from a context the browser says is Steam's own top document. Filled from Runtime.executionContextCreated.
        self.trusted_contexts = set()


class Helper:
    def __init__(self, cfg, exposure_check=None, exposure_interval=30.0, exposure_choice=None):
        self.cfg = cfg
        # WHY a parameter that defaults to "no check": __main__ turns it on for the installed helper; tests leave it off so
        # whatever happens to be listening on a developer's machine cannot change their results.
        self.exposure_check = exposure_check
        self.exposure_interval = exposure_interval
        self.exposure_choice = exposure_choice   # returns 'open' when the person chose to leave the door open
        self.exposed = False
        self.open_by_choice = False
        self.status = Status(cfg.state_dir)
        self.ledger = RetryLedger(cfg.state_dir / 'retry.json', cfg.max_failures, boot_id=_boot_id())
        self.thread = None
        self._stop = threading.Event()
        self._abort = False            # crash(): leave without any cleanup (tests, and what kill -9 looks like)
        self.device = None
        self.browser = None            # Cdp on Steam's browser socket
        self.browser_string = None
        self.targets = {}              # id -> {'type', 'title'}
        self.pages = {}                # id -> PageSession
        self.first_seen = {}           # id -> monotonic time we first saw the SteamClient page
        self.not_ready_since = {}      # id -> when that page last started reporting "SteamClient not defined yet" (review fix 3)
        self.next_attempt = 0.0
        self.last_inject = None
        self.closed_since = None
        self.next_beat = 0.0
        self.last_written = None
        self.paused_reason = ''
        self._stub_source = None
        # Typing (M5). The bridge has its own worker thread, because the first swipe loads the engine's models (seconds) and
        # the main loop must keep the page's heartbeat going meanwhile. The worker never touches the page sockets: it hands
        # finished answers to the main loop through this outbox, and the main loop is woken through a pipe.
        self.bridge = None
        self._outbox = collections.deque()
        self._wake_r, self._wake_w = os.pipe()
        os.set_blocking(self._wake_r, False)
        os.set_blocking(self._wake_w, False)
        self._sjs = None               # the worker's own socket to SharedJSContext
        self._sjs_id = None
        # M9 (failure handling). dead_pages: pages whose layer stopped itself, with why; they are not tried again until the page
        # is replaced. kb_missing: Steam is up but has no page with the keyboard page's title. recheck: ask Steam's hidden page
        # again whether the text call is still there (periodically, and at once when a typing call fails).
        self.dead_pages = {}
        # Pages titled like the keyboard that proved to be someone else's (target id -> listed url). WHY not a failure: any web
        # page can open a window and give it that title; counting it would let a web page switch Gaming Mode swipe off.
        self.foreign = {}
        self.kb_missing = False
        self.next_recheck = 0.0
        self._recheck_now = False

    # ---- lifecycle ------------------------------------------------------------------------------------
    def start(self):
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def stop(self):
        """Clean shutdown: take our stub out of Steam, then say 'off'."""
        self._stop.set()
        if self.thread:
            self.thread.join(timeout=10)

    def crash(self):
        """Vanish like kill -9: no removal, no status. Only the page-side watchdog can clean up after this."""
        self._abort = True
        self._stop.set()
        if self.thread:
            self.thread.join(timeout=10)
        self._drop_connections()

    def run(self):
        self.bridge = Bridge(self.cfg, self._send_text, self._deliver)
        self._clear_stop_request()          # a request left by an earlier stop must not stop this run
        try:
            self._load_contract()
            self._loop()
        finally:
            self.bridge.stop()
            self._close_sjs()
            if not self._abort:
                self._shutdown_cleanly()
                # WHY the file is removed only now (review fix 8): the service's stop script waits for it to go, so "gone" means
                # "the layer is out of Steam and the status says off", and only then does the service kill the sandbox.
                self._clear_stop_request()
            for fd in (self._wake_r, self._wake_w):
                try:
                    os.close(fd)
                except OSError:
                    pass

    def _clear_stop_request(self):
        try:
            (self.cfg.state_dir / 'stop-request').unlink()
        except OSError:
            pass

    # ---- contract / stub source -----------------------------------------------------------------------
    def _load_contract(self):
        try:
            self.device = contract.load(self.cfg.contract_path)
            self._contract_error = None
        except contract.ContractError as e:
            self.device = None
            self._contract_error = str(e)

    def _stub(self):
        if self._stub_source is None:
            cfg = {'version': LAYER_VERSION, 'watchdog_ms': int(self.cfg.layer_watchdog * 1000), 'ui_origin': self.device.ui_origin,
                   'device': self.device.page_config, 'gesture': dict(self.cfg.gesture), 'session': dict(self.cfg.session), 'failsafe': dict(self.cfg.failsafe)}
            # WHY read at use, pasted in as text: the layer files ship as data beside the helper and are sent to
            # Steam's page through the debug port; nothing is fetched and nothing is written into Steam.
            # session.js (policy, strip, trail, commit) is pasted in front of gesture.js (the arbiter); the frame starts both.
            parts = []
            for name in ('failsafe.js', 'session.js', 'gesture.js'):
                try:
                    parts.append((self.cfg.layer_dir / name).read_text())
                except OSError:
                    parts.append('')      # a missing file: that part simply does not start and Steam's keyboard is left as it is
            gestures = '\n'.join(parts)
            self._stub_source = _STUB_JS.read_text().replace('/*__GESTURES__*/', gestures).replace('__CONFIG__', json.dumps(cfg))
        return self._stub_source

    # ---- main loop ------------------------------------------------------------------------------------
    def _loop(self):
        next_exposure = 0.0
        while not self._stop.is_set():
            if (self.cfg.state_dir / 'stop-request').exists():
                self._stop.set()
                break
            now = time.monotonic()
            if self.exposure_check and now >= next_exposure:
                self._exposure_tick()
                next_exposure = now + self.exposure_interval
            if self.exposed:
                # WHY nothing else runs: while Steam's debug port can be reached from other devices the layer stays out.
                self._stop.wait(self.cfg.poll_interval)
                continue
            try:
                if self.device is None:
                    self._publish('error', self._contract_error)
                    self._load_contract()          # pick up a repaired contract file
                    self._stop.wait(self.cfg.poll_interval)
                elif self.browser is None:
                    self._try_connect(now)
                    if self.browser is None:
                        self._stop.wait(self.cfg.poll_interval)   # port closed: look again shortly
                else:
                    self._pump(now)                # waits inside select(), at most poll_interval
            except (ws.WsError, cdp.CdpError, OSError, ValueError):
                # Any trouble with Steam's side: forget the connection and look again. Never raise out.
                self._disconnect()
                self._stop.wait(self.cfg.poll_interval)

    # ---- outside door ---------------------------------------------------------------------------------
    def _exposure_tick(self):
        doors = self.exposure_check()
        now_open_by_choice = bool(doors) and self.exposure_choice is not None and self.exposure_choice() == 'open'
        if now_open_by_choice != self.open_by_choice:
            self.open_by_choice = now_open_by_choice
            self.last_written = None          # the status reason changes, so say it again
            if self.browser and not self.exposed:
                self._publish_running()
        if now_open_by_choice:
            doors = []                        # the person chose this; the layer runs and the status only notes it
        if not doors:
            if self.exposed:
                self.exposed = False
                self.last_written = None      # say something new as soon as the normal loop resumes
            return
        if not self.exposed:
            self.exposed = True
            # Take our layer out of Steam if it was in; any trouble talking to Steam must not stop the status below.
            try:
                for pid in list(self.pages):
                    self._detach(pid, remove=True)
            except (ws.WsError, cdp.CdpError, OSError, ValueError):
                pass
            self._disconnect()
        self._publish('blocked-outside-access',
                      'Steam\'s debug port is open to other devices on the network (port %s). Gaming Mode swipe is off. '
                      'Run the Swipe Keyboard installer again to close it.' % ', '.join(str(p) for p in doors))

    def _steam_pages(self, title, popup_ok=False):
        """Ids of the pages with this title that Steam itself serves.

        WHY the address as well as the title (review fix 1): a title is set by whoever wrote the page. Steam's in-client web
        browser opens ordinary web pages next to Steam's own, and one of those could call itself "SharedJSContext" and be
        mistaken for the hidden page that holds SteamClient (receiving every swiped word) or for the keyboard page."""
        d = self.device
        # WHY popup_ok (stable Steam, 2026-10-23): Steam's keyboard window is listed as about:blank?... because Steam's UI opened
        # it; inside, it is a Steam page. Only the keyboard page may be accepted this way, and _attach then asks the page itself
        # for its origin (which no web page can fake) before anything is installed. The hidden SteamClient page stays strict.
        return [i for i, t in list(self.targets.items())
                if t['type'] == 'page' and t['title'] == title and d and self.foreign.get(i) != t.get('url')
                and (d.is_steam_url(t.get('url', '')) or (popup_ok and d.is_steam_popup_placeholder(t.get('url', ''))))]

    def _hidden_page(self):
        """The one hidden page that holds SteamClient, or None. Two candidates are never guessed between."""
        ids = self._steam_pages(self.device.client_title) if self.device else []
        return ids[0] if len(ids) == 1 else None

    # ---- connecting -----------------------------------------------------------------------------------
    def _try_connect(self, now):
        try:
            ver = cdp.http_json(self.cfg.port, '/json/version', timeout=self.cfg.call_timeout)
            self.browser = cdp.Cdp.open(self.cfg.port, '/devtools/browser/' + _browser_id(ver, self.cfg.port),
                                        timeout=self.cfg.call_timeout)
            self.browser.call('Target.setDiscoverTargets', {'discover': True})
        except (OSError, ws.WsError, cdp.CdpError, ValueError, KeyError):
            self._disconnect()
            self._port_closed(now)
            return
        self.closed_since = None
        self.browser_string = ver.get('Browser')
        self.ledger.use_build(self._fingerprint())
        self.targets = {}
        self.first_seen = {}
        self.not_ready_since = {}
        self.next_beat = now
        self._load_contract()      # a fixed contract file takes effect on the next connect without a restart
        self._reconcile(now)
        self._publish_running()

    def _port_closed(self, now):
        if self.closed_since is None:
            self.closed_since = now
        # WHY the reason comes from a file and not from looking: this program runs in a sandbox that cannot see
        # Steam's folder or Steam's process (and must not be given them: Steam's folder holds login files). The
        # host-side watcher (gaming/setup/switch-watch.sh) looks and writes one word into our state folder.
        # It never recreates the switch file: the person is told and decides (owner decision 2026-10-22).
        word = self._switch_word()
        if word == 'off':
            self._publish('switch-off', SWITCH_OFF_REASON)
        elif word == 'no-steam':
            self._publish('steam-not-found', 'No Steam folder found for this user.')
        elif word == 'restart':
            self._publish('needs-steam-restart', 'Steam is running without its debug switch; restart Steam once.')
        else:
            self._publish('waiting-for-steam', 'Steam is not running or is restarting.')

    def _switch_word(self):
        try:
            return (self.cfg.state_dir / 'switch-state').read_text().strip()
        except OSError:
            return ''

    def _fingerprint(self):
        probe = self.cfg.steamui_probe
        extra = ''
        paths = [d / 'steamui' / 'sp.js' for d in self.cfg.marker_dirs] if probe == 'auto' else ([Path(probe)] if probe else [])
        for p in paths:
            try:
                extra = str(p.stat().st_mtime_ns)
                break
            except OSError:
                continue
        return '%s|%s' % (self.browser_string, extra)

    # ---- connected: events, safety poll, heartbeat ----------------------------------------------------
    def _pump(self, now):
        self._pump_io()
        now = time.monotonic()
        if now >= self.next_beat:
            self.next_beat = now + self.cfg.beat_interval
            self._safety_poll()
            self._beat_all(now)
        if self.pages and (self._recheck_now or now >= self.next_recheck):
            self._recheck_now = False
            self.next_recheck = now + self.cfg.recheck_interval
            self._recheck_contract(now)
        self._reconcile(now)
        self._publish_running()

    def _pump_io(self):
        """Wait for traffic (at most poll_interval) and hand it to whoever it belongs to."""
        socks = [self.browser.ws] + [p.cdp.ws for p in self.pages.values()]
        # WHY only the sockets that really hold something (review fix 2): a frame left in ONE page socket's buffer used to
        # mark ALL sockets ready, so the quiet browser socket was read for 0.5 s, timed out, and the timeout was treated as a
        # broken connection: every connection dropped and was opened again.
        buffered = [s for s in socks if s.has_buffered()]
        busy = bool(buffered) or any(p.cdp.events for p in self.pages.values())
        r, _, _ = select.select(socks + [self._wake_r], [], [], 0 if busy else self.cfg.poll_interval)
        for s in r:
            if s == self._wake_r:
                self._flush_outbox()
            elif s is self.browser.ws:
                self._browser_events()
            else:
                self._page_traffic(s)
        for s in buffered:
            if s not in r:
                if s is self.browser.ws:
                    self._browser_events(timeout=0.05)     # data we already have: do not wait for more
                else:
                    self._page_traffic(s, timeout=0.05)
        for pid, p in list(self.pages.items()):
            self._page_events(pid, p)       # events that arrived while we were waiting for an answer to a call

    def _browser_events(self, timeout=0.5):
        # Drain what is already buffered/readable without blocking longer than one message.
        while True:
            try:
                ev = self.browser.next_event(timeout=timeout)
            except ws.Timeout:
                return          # nothing whole to read yet: a quiet socket is not a broken one (review fix 2)
            m, p = ev.get('method'), ev.get('params', {})
            if m in ('Target.targetCreated', 'Target.targetInfoChanged'):
                i = p['targetInfo']
                self.targets[i['targetId']] = {'type': i.get('type'), 'title': i.get('title', ''), 'url': i.get('url', '')}
            elif m == 'Target.targetDestroyed':
                self.targets.pop(p['targetId'], None)
            if not self.browser.ws.has_buffered():
                return

    def _page_traffic(self, sock, timeout=0.5):
        for pid, p in list(self.pages.items()):
            if p.cdp.ws is sock:
                try:
                    self._page_event(pid, p.cdp.next_event(timeout=timeout))
                    while p.cdp.ws.has_buffered():
                        self._page_event(pid, p.cdp.next_event(timeout=timeout))
                except ws.Timeout:
                    pass
                except (ws.WsError, OSError, ValueError):
                    self._detach(pid, remove=False)

    def _page_events(self, pid, p):
        while p.cdp.events:
            self._page_event(pid, p.cdp.events.pop(0))

    def _page_event(self, pid, ev):
        # What we act on: the layer calling window.__swipekb(json), and the browser naming the documents in the page so we
        # know which calls come from Steam's own top document. Everything else Steam's page says is ignored.
        m, params = ev.get('method'), ev.get('params') or {}
        p = self.pages.get(pid)
        if m == 'Runtime.executionContextCreated' and p:
            ctx = params.get('context') or {}
            aux = ctx.get('auxData') or {}
            if ctx.get('origin') == self.device.ui_origin and aux.get('isDefault') and aux.get('type', 'default') == 'default':
                p.trusted_contexts.add(ctx.get('id'))
        elif m == 'Runtime.executionContextDestroyed' and p:
            p.trusted_contexts.discard(params.get('executionContextId'))
        elif m == 'Runtime.executionContextsCleared' and p:
            p.trusted_contexts.clear()
        elif m == 'Runtime.bindingCalled' and params.get('name') == '__swipekb':
            if p and params.get('executionContextId') in p.trusted_contexts:
                self.bridge.submit(pid, params.get('payload'))

    # ---- typing: worker thread -> page ----------------------------------------------------------------
    def _deliver(self, pid, js):
        """Called from the bridge's worker: queue a statement for the page and wake the main loop."""
        self._outbox.append((pid, js))
        try:
            os.write(self._wake_w, b'x')
        except OSError:
            pass

    def _flush_outbox(self):
        try:
            os.read(self._wake_r, 4096)
        except OSError:
            pass
        while self._outbox:
            pid, js = self._outbox.popleft()
            p = self.pages.get(pid)
            if p is None:
                continue
            try:
                p.cdp.evaluate(js)
            except (ws.WsError, cdp.CdpError, OSError, ValueError):
                self._detach(pid, remove=False)

    def _send_text(self, text):
        """Type `text` through Steam's own call (plan Section 7.4). Runs on the bridge's worker thread, on its own socket."""
        sjs = self._hidden_page()
        if sjs is None:
            raise SendFailed()
        try:
            if self._sjs is None or self._sjs_id != sjs:
                self._close_sjs()
                self._sjs = cdp.Cdp.open(self.cfg.port, cdp.page_path(sjs), timeout=self.cfg.call_timeout)
                self._sjs_id = sjs
            self._sjs.evaluate('SteamClient.Input.ControllerKeyboardSendText(%s)' % json.dumps(text))
        except (ws.WsError, cdp.CdpError, OSError, ValueError) as e:
            self._close_sjs()
            # WHY (M9): Steam answered but refused (the call is gone or renamed). Every later swipe would be swallowed and
            # typed nowhere; ask the main loop to look again now, so the layer comes off instead of waiting for the next check.
            if isinstance(e, cdp.CdpError):
                self._recheck_now = True
            raise SendFailed()

    def _close_sjs(self):
        if self._sjs is not None:
            try:
                self._sjs.close()
            except Exception:
                pass
        self._sjs = None
        self._sjs_id = None

    def _safety_poll(self):
        """WHY: the event stream is the fast path; this listing every second catches anything it dropped, and
        notices a closed port even if the browser socket still looks open."""
        lst = cdp.http_json(self.cfg.port, '/json/list', timeout=self.cfg.call_timeout)
        seen = {t['id']: {'type': t.get('type'), 'title': t.get('title', ''), 'url': t.get('url', '')} for t in lst if 'id' in t}
        self.targets = seen
        # WHY here (M9): the Steam build is otherwise only looked at when the port opens. A new Steam UI can arrive without the
        # port ever closing; a helper that had given up on the old build must get its fresh tries then.
        fp = self._fingerprint()
        if fp != self.ledger.fingerprint:
            self.ledger.use_build(fp)
            self.next_attempt = 0.0
            self.dead_pages = {}

    _PING = ('(function(){var L=window.__swipekbLayer;return L&&L.version===%s?(L.beat(),L.report?L.report():"{}"):null})()'
             % json.dumps(LAYER_VERSION))

    def _beat_all(self, now):
        for pid, p in list(self.pages.items()):
            try:
                raw = p.cdp.evaluate(self._PING)
                if raw is None:
                    # The page lost our stub (reload that skipped the registered script): put it back. WHY counted (M9): once
                    # is a reload; a page that removes it again and again is not something to keep fighting.
                    p.lost = [t for t in p.lost if now - t < self.cfg.healthy_after]
                    if len(p.lost) >= self.cfg.max_failures:
                        self._layer_trouble(pid, 'the layer keeps disappearing from the keyboard page', now)
                        continue
                    p.lost.append(now)
                    p.cdp.evaluate(self._stub())
                    self.last_inject = time.time()
                    p.since = now
                    continue
                try:
                    p.report = json.loads(raw)
                except ValueError:
                    p.report = {}
                self._page_events(pid, p)
            except cdp.CdpError as e:
                # The page answered but would not run our script (blocked or broken): a Steam-side refusal, not a dropped link.
                self._layer_trouble(pid, 'Steam refused to run the layer (%s)' % _short(e), now)
                continue
            except (ws.WsError, OSError, ValueError):
                self._detach(pid, remove=False)
                continue
            self._judge_report(pid, p, now)

    def _judge_report(self, pid, p, now):
        r = p.report if isinstance(p.report, dict) else {}
        for part in ('gestures', 'session'):
            if r.get(part) == 'failed':
                why = r.get(part + '_reason') or 'unknown error'
                self.dead_pages[pid] = 'the layer stopped itself after an error: %s' % why
                self._detach(pid, remove=True)
                if self.ledger.record_failure(self.dead_pages[pid], now, self.cfg.failure_spacing):
                    self.next_attempt = now + self.ledger.backoff(self.cfg.failure_spacing, self.cfg.max_backoff)
                return
        if now - p.since >= self.cfg.healthy_after:
            self.ledger.record_success()      # it stayed and answered: earlier failures on this build no longer count

    def _layer_trouble(self, pid, detail, now):
        """The layer cannot be kept in this page: take it out, count it, and let the (capped, slowing) retry rules decide."""
        self._detach(pid, remove=True)
        if self.ledger.record_failure(detail, now, self.cfg.failure_spacing):
            self.next_attempt = now + self.ledger.backoff(self.cfg.failure_spacing, self.cfg.max_backoff)

    def _recheck_contract(self, now):
        """With the layer installed: is Steam's text call still there? If not, take the layer out of every page."""
        sjs = self._hidden_page()
        if sjs is None:
            return
        verdict, detail = self._check_contract(sjs)
        self._note_readiness(sjs, verdict, time.monotonic())
        if verdict != 'fail':
            return        # 'notready' means Steam is reloading; the target events will bring the pages back
        for pid in list(self.pages):
            self._detach(pid, remove=True)
        if self.ledger.record_failure(detail, now, self.cfg.failure_spacing):
            self.next_attempt = now + self.ledger.backoff(self.cfg.failure_spacing, self.cfg.max_backoff)

    # ---- deciding what to attach ----------------------------------------------------------------------
    def _reconcile(self, now):
        if self.ledger.exhausted:
            self.paused_reason = self.ledger.detail
            # WHY (M9): giving up must also mean "layer out". A page we were already in keeps its copy otherwise.
            for pid in list(self.pages):
                self._detach(pid, remove=True)
            return
        kb = self.device.keyboard_title
        wanted = self._steam_pages(kb, popup_ok=True)
        for pid in list(self.pages):
            if pid not in wanted:
                self._detach(pid, remove=True)
        for pid in list(self.dead_pages):
            if pid not in wanted:
                del self.dead_pages[pid]
        sjs = self._steam_pages(self.device.client_title)
        # WHY forget pages that are gone (review fix 3): the grace clocks are per page and a new page must start its own.
        for book in (self.first_seen, self.not_ready_since):
            for i in [i for i in book if i not in self.targets]:
                del book[i]
        for i in sjs:
            self.first_seen.setdefault(i, now)
        # Steam is up (its hidden page is there) but nothing carries the keyboard page's title. Normal in Desktop Mode and during
        # start-up; if it lasts in Gaming Mode, Valve renamed the page. Said in the status, not counted as a failure.
        self.kb_missing = bool(sjs) and not wanted and now - min(self.first_seen[i] for i in sjs) >= self.cfg.not_ready_grace
        pending = [i for i in wanted if i not in self.pages and i not in self.dead_pages]
        if not pending or now < self.next_attempt:
            return
        if not sjs:
            return          # Steam's hidden page is not there yet; we are woken by its Target event
        if len(sjs) > 1:
            # WHY (review fix 1): two pages from Steam's address both calling themselves the hidden page is not something to
            # guess about; typing goes to neither, and after the usual few tries the helper pauses with this reason.
            if self.ledger.record_failure('more than one Steam page is named "%s"' % self.device.client_title, now, self.cfg.failure_spacing):
                self.next_attempt = now + self.ledger.backoff(self.cfg.failure_spacing, self.cfg.max_backoff)
            return
        verdict, detail = self._check_contract(sjs[0])
        self._note_readiness(sjs[0], verdict, now)
        if verdict == 'notready':
            # WHY the clock is "not ready since", not "page first seen" (review fix 3): Steam reloads this page in place (same
            # id), and the page's age says nothing about how long THIS load has been waiting for SteamClient.
            if now - self.not_ready_since[sjs[0]] < self.cfg.not_ready_grace:
                return
            verdict, detail = 'fail', 'Steam page never finished loading SteamClient'
        if verdict == 'fail':
            if self.ledger.record_failure(detail, now, self.cfg.failure_spacing):
                self.next_attempt = now + self.ledger.backoff(self.cfg.failure_spacing, self.cfg.max_backoff)
            return
        for i in pending:
            try:
                self._attach(i)
            except _NotYet:
                self.next_attempt = now + 1.0
                continue
            except _Foreign:
                self.foreign[i] = (self.targets.get(i) or {}).get('url')
                continue
            except cdp.CdpError as e:
                # Steam answered and said no (blocked script, refused registration): that is a Steam-side refusal.
                self.ledger.record_failure('Steam refused to run the layer (%s)' % _short(e), now, self.cfg.failure_spacing)
                self.next_attempt = now + self.ledger.backoff(self.cfg.failure_spacing, self.cfg.max_backoff)
                return
            except (ws.WsError, OSError, ValueError):
                self.ledger.record_failure('could not install into the keyboard page', now, self.cfg.failure_spacing)
                self.next_attempt = now + self.ledger.backoff(self.cfg.failure_spacing, self.cfg.max_backoff)
                return
        # WHY no success is recorded here (M9): an install that works is not yet a layer that stays. A page that accepts the layer
        # and drops it a second later would otherwise wipe the count every time and the helper would loop for ever.

    def _note_readiness(self, pid, verdict, now):
        """Start the 'still loading' clock when the page first reports not ready; stop it as soon as it answers."""
        if verdict == 'notready':
            self.not_ready_since.setdefault(pid, now)
        else:
            self.not_ready_since.pop(pid, None)

    def _check_contract(self, sjs_id):
        """-> ('ok'|'notready'|'fail', detail). Opens a short-lived socket to Steam's hidden page."""
        try:
            c = cdp.Cdp.open(self.cfg.port, cdp.page_path(sjs_id), timeout=self.cfg.call_timeout)
        except (ws.WsError, cdp.CdpError, OSError):
            return 'notready', 'hidden page not reachable'
        try:
            for m in self.device.methods:
                got = c.evaluate(contract.method_probe(m))
                if got == 'notready':
                    return 'notready', ''
                if got != 'ok':
                    return 'fail', 'Steam no longer offers %s' % (got.split(':', 1)[-1] if isinstance(got, str) else m['name'])
            return 'ok', ''
        except (ws.WsError, cdp.CdpError, OSError, ValueError):
            return 'notready', 'hidden page did not answer'
        finally:
            c.close()

    # ---- the stub in a page ---------------------------------------------------------------------------
    def _attach(self, target_id):
        c = cdp.Cdp.open(self.cfg.port, cdp.page_path(target_id), timeout=self.cfg.call_timeout)
        try:
            # WHY first: the listed address may be Steam's about:blank placeholder, so the page's own origin decides. The browser
            # sets it; a web page cannot claim Steam's. Nothing (no binding, no script) is given to a page that fails this.
            origin = c.evaluate('location.origin')
            if origin in (None, '', 'null'):
                raise _NotYet()             # still loading: asked again shortly, not counted as Steam failing
            if origin != self.device.ui_origin:
                raise _Foreign(origin)
            # WHY both: the registration brings the stub back at the start of every later document in this
            # page (UI reload) with no round trip; the evaluate puts it into the page that is open right now.
            # The registration lives only as long as this socket, so a dead helper leaves none behind.
            # The binding is how the layer talks to us: window.__swipekb(json) arrives here as Runtime.bindingCalled (it needs
            # Runtime enabled to be delivered; M4's rig proved the pair works on Steam's page). It is made BEFORE the layer
            # starts, so the layer finds it. It lives only as long as this socket, like the registration below.
            c.call('Runtime.addBinding', {'name': '__swipekb'})
            c.call('Runtime.enable')
            c.call('Page.addScriptToEvaluateOnNewDocument', {'source': self._stub()})
            c.evaluate(self._stub())
        except Exception:
            c.close()
            raise
        if not self.pages:
            # WHY (M9): the first look after an install is one full interval away, not "right now" (it was 0 at start), so the
            # periodic check and the check forced by a failed typing call are different things.
            self.next_recheck = time.monotonic() + self.cfg.recheck_interval
        self.pages[target_id] = PageSession(target_id, c)
        self.last_inject = time.time()

    def _detach(self, target_id, remove):
        p = self.pages.pop(target_id, None)
        if not p:
            return
        if self.bridge:
            self.bridge.forget(target_id)      # the engine forgets that keyboard session at once
        if remove:
            try:
                p.cdp.evaluate('(function(){var L=window.__swipekbLayer;if(L&&L.remove)L.remove();})()')
            except Exception:
                pass
        p.cdp.close()

    # ---- connection teardown --------------------------------------------------------------------------
    def _drop_connections(self):
        for pid in list(self.pages):
            p = self.pages.pop(pid)
            if self.bridge:
                # WHY (review fix 2): the engine must forget that keyboard session when its page connection goes away, the same
                # as _detach does; otherwise the layout and candidate ids stay in the engine until the idle timeout.
                self.bridge.forget(pid)
            try:
                p.cdp.ws.sock.close()
            except OSError:
                pass
        if self.browser:
            try:
                self.browser.ws.sock.close()
            except OSError:
                pass
            self.browser = None

    def _disconnect(self):
        self._drop_connections()
        self.targets = {}
        if self.closed_since is None:
            self.closed_since = time.monotonic()

    def _shutdown_cleanly(self):
        for pid in list(self.pages):
            self._detach(pid, remove=True)
        if self.browser:
            self.browser.close()
            self.browser = None
        self._write('off', 'Helper stopped.')

    # ---- status ---------------------------------------------------------------------------------------
    def _publish_running(self):
        if self.ledger.exhausted:
            # WHY this wording (review fix 3): the tries can run out for reasons that are not a Steam change (a slow load, a
            # refused script), and the person needs to know how to start again.
            self._publish('paused-steam-changed', 'Paused after %d failed tries on this Steam version. Last problem: %s. '
                          'Stock keyboard in use. Run the Swipe Keyboard installer again, or restart the device, to try again.'
                          % (self.ledger.failures, self.ledger.detail or 'a needed part is missing'))
        elif self.pages:
            # The layer's own view of the keyboard (M9): a keyboard that no longer matches switches the layer off in the page;
            # counted oddities (too few letter keys, no keys found) are only reported.
            for p in self.pages.values():
                kb = (p.report or {}).get('keyboard') or {}
                if kb.get('issue') and (p.report or {}).get('gestures') == 'off':
                    self._publish('paused-steam-changed', 'Steam changed: ' + kb['issue'] + '. Stock keyboard in use.', kb)
                    return
            for p in self.pages.values():
                kb = (p.report or {}).get('keyboard') or {}
                if kb.get('issue'):
                    self._publish('active', 'Note: ' + kb['issue'] + '.', kb)
                    return
            self._publish('active', 'Outside access left open by your choice.' if self.open_by_choice else '')
        elif self.dead_pages:
            self._publish('error', next(iter(self.dead_pages.values())).capitalize() + '. Stock keyboard in use.')
        elif self.ledger.failures:
            self._publish('waiting-for-steam', 'Last try failed: ' + self.ledger.detail + '. Trying again shortly.')
        elif self.kb_missing:
            self._publish('waiting-for-steam', 'Steam is up but has no page named "%s". Normal in Desktop Mode; in Gaming Mode '
                          'it means Steam renamed it. Stock keyboard in use.' % self.device.keyboard_title)
        else:
            self._publish('waiting-for-steam', 'Steam is up; waiting for its keyboard page.')

    def _publish(self, state, reason, keyboard=None):
        self._write(state, reason, keyboard)

    def _write(self, state, reason, keyboard=None):
        key = (state, reason, len(self.pages), self.last_inject, self.browser_string, json.dumps(keyboard, sort_keys=True))
        if key == self.last_written:
            return
        self.last_written = key
        self.status.write(state, reason, steam_browser=self.browser_string, layer_version=LAYER_VERSION,
                          last_inject=self.last_inject, attached_pages=len(self.pages), keyboard=keyboard)


def _boot_id():
    """Names this boot of the device (None if it cannot be read). A pause is remembered across helper restarts but not across reboots."""
    try:
        return Path('/proc/sys/kernel/random/boot_id').read_text().strip() or None
    except OSError:
        return None


class _NotYet(Exception):
    """The keyboard page has no document origin yet."""


class _Foreign(Exception):
    """A page titled like Steam's keyboard whose document is not Steam's."""


def _short(e):
    """A CDP error as one short line for the status file (Steam's own wording, trimmed)."""
    t = ' '.join(str(e).split())
    return t[:120]


def _browser_id(ver, port):
    """The browser socket path from /json/version, accepted only if it is our own loopback port."""
    host, p, path = config.check_ws_url(ver['webSocketDebuggerUrl'], port)
    prefix = '/devtools/browser/'
    if not path.startswith(prefix):
        raise ValueError('odd browser path')
    return path[len(prefix):]
