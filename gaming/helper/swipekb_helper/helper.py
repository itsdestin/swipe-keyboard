"""The helper's main loop (plan Sections 3 and 6).

Single thread, no framework:  find Steam's debug port -> watch its pages -> check the contract -> install the
layer stub -> keep it there (Steam restarts, UI reloads, our own restarts) -> report status.

Failure rule everywhere: do nothing and leave Steam's own keyboard alone. We only ever ADD things to a page
(one window property, one attribute, one timer) and can always remove them.
"""
import json
import os
import select
import threading
import time
from pathlib import Path

from . import LAYER_VERSION, cdp, config, contract, ws
from .retry import RetryLedger
from .status import Status

_STUB_JS = Path(__file__).with_name('layer_stub.js')


def default_steam_running():
    """When did the oldest running Steam client start (epoch seconds)? None if no Steam is running.
    Used only to tell 'Steam has not read our debug switch' from 'Steam is busy (starting, updating)'."""
    try:
        hz = os.sysconf('SC_CLK_TCK')
        boot = next(float(l.split()[1]) for l in Path('/proc/stat').read_text().splitlines() if l.startswith('btime'))
        starts = []
        for p in Path('/proc').iterdir():
            if p.name.isdigit():
                try:
                    if (p / 'comm').read_text().strip() == 'steam':
                        # field 22 of /proc/<pid>/stat, counted after the ")" that ends the command name
                        ticks = int((p / 'stat').read_text().rsplit(')', 1)[1].split()[19])
                        starts.append(boot + ticks / hz)
                except (OSError, ValueError, IndexError):
                    continue
        return min(starts) if starts else None
    except (OSError, StopIteration, ValueError):
        return None


class PageSession:
    """Our open websocket to one of Steam's pages that carries a stub."""

    def __init__(self, target_id, c):
        self.id = target_id
        self.cdp = c
        self.last_beat = time.monotonic()


class Helper:
    def __init__(self, cfg, steam_running=default_steam_running):
        self.cfg = cfg
        self.steam_running = steam_running
        self.status = Status(cfg.state_dir)
        self.ledger = RetryLedger(cfg.state_dir / 'retry.json', cfg.max_failures)
        self.thread = None
        self._stop = threading.Event()
        self._abort = False            # crash(): leave without any cleanup (tests, and what kill -9 looks like)
        self.device = None
        self.browser = None            # Cdp on Steam's browser socket
        self.browser_string = None
        self.targets = {}              # id -> {'type', 'title'}
        self.pages = {}                # id -> PageSession
        self.first_seen = {}           # id -> monotonic time we first saw the SteamClient page
        self.next_attempt = 0.0
        self.last_inject = None
        self.marker_recreated = False
        self.closed_since = None
        self.next_beat = 0.0
        self.last_written = None
        self.paused_reason = ''
        self._stub_source = None

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
        try:
            self._load_contract()
            self._loop()
        finally:
            if not self._abort:
                self._shutdown_cleanly()

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
            cfg = {'version': LAYER_VERSION, 'watchdog_ms': int(self.cfg.layer_watchdog * 1000),
                   'device': self.device.page_config}
            self._stub_source = _STUB_JS.read_text().replace('__CONFIG__', json.dumps(cfg))
        return self._stub_source

    # ---- main loop ------------------------------------------------------------------------------------
    def _loop(self):
        next_marker = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if now >= next_marker:
                self._marker_check()
                next_marker = now + self.cfg.beat_interval
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

    # ---- marker file ----------------------------------------------------------------------------------
    def _marker_check(self):
        name = '.cef-enable-remote-debugging'
        if any((d / name).exists() for d in self.cfg.marker_dirs):
            return
        for d in self.cfg.marker_dirs:
            if d.is_dir():
                try:
                    (d / name).touch()      # WHY: Steam reads this file only at start; it is the debug switch.
                    self.marker_recreated = True
                except OSError:
                    continue
                return

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
        self.marker_recreated = False
        self.browser_string = ver.get('Browser')
        self.ledger.use_build(self._fingerprint())
        self.targets = {}
        self.first_seen = {}
        self.next_beat = now
        self._load_contract()      # a fixed contract file takes effect on the next connect without a restart
        self._reconcile(now)
        self._publish_running()

    def _port_closed(self, now):
        if self.closed_since is None:
            self.closed_since = now
        running = self.steam_running()
        if not any(d.is_dir() for d in self.cfg.marker_dirs):
            self._publish('steam-not-found', 'No Steam folder found for this user.')
        elif running and (self.marker_recreated or self._marker_newer_than(running)):
            self._publish('needs-steam-restart', 'Steam is running without its debug switch; restart Steam once.')
        else:
            self._publish('waiting-for-steam', 'Steam is not running or is restarting.')

    def _marker_newer_than(self, steam_start):
        """True if the debug-switch file was made after Steam started (so Steam never read it).
        WHY not 'port closed for a while': Steam updating itself keeps the port closed for minutes with the
        switch perfectly fine, and telling the user to restart then would be wrong."""
        if steam_start is True:
            return False
        for d in self.cfg.marker_dirs:
            try:
                return (d / '.cef-enable-remote-debugging').stat().st_mtime > steam_start
            except OSError:
                continue
        return False

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
        socks = [self.browser.ws] + [p.cdp.ws for p in self.pages.values()]
        if not any(s.has_buffered() for s in socks):
            r, _, _ = select.select(socks, [], [], self.cfg.poll_interval)
        else:
            r = socks
        for s in r:
            if s is self.browser.ws:
                self._browser_events()
            else:
                self._page_traffic(s)
        now = time.monotonic()
        if now >= self.next_beat:
            self.next_beat = now + self.cfg.beat_interval
            self._safety_poll()
            self._beat_all()
        self._reconcile(now)
        self._publish_running()

    def _browser_events(self):
        # Drain what is already buffered/readable without blocking longer than one message.
        while True:
            ev = self.browser.next_event(timeout=0.5)
            m, p = ev.get('method'), ev.get('params', {})
            if m in ('Target.targetCreated', 'Target.targetInfoChanged'):
                i = p['targetInfo']
                self.targets[i['targetId']] = {'type': i.get('type'), 'title': i.get('title', '')}
            elif m == 'Target.targetDestroyed':
                self.targets.pop(p['targetId'], None)
            if not self.browser.ws.has_buffered():
                return

    def _page_traffic(self, sock):
        for pid, p in list(self.pages.items()):
            if p.cdp.ws is sock:
                try:
                    p.cdp.next_event(timeout=0.5)   # we enable no events; this notices a closed socket
                except ws.Timeout:
                    pass
                except (ws.WsError, OSError, ValueError):
                    self._detach(pid, remove=False)

    def _safety_poll(self):
        """WHY: the event stream is the fast path; this listing every second catches anything it dropped, and
        notices a closed port even if the browser socket still looks open."""
        lst = cdp.http_json(self.cfg.port, '/json/list', timeout=self.cfg.call_timeout)
        seen = {t['id']: {'type': t.get('type'), 'title': t.get('title', '')} for t in lst if 'id' in t}
        self.targets = seen

    def _beat_all(self):
        for pid, p in list(self.pages.items()):
            try:
                alive = p.cdp.evaluate(
                    '(function(){var L=window.__swipekbLayer;return L&&L.version===%s?L.beat():null})()'
                    % json.dumps(LAYER_VERSION))
                if alive != LAYER_VERSION:
                    # The page lost our stub (reload that skipped the registered script): put it back.
                    p.cdp.evaluate(self._stub())
                    self.last_inject = time.time()
            except (ws.WsError, cdp.CdpError, OSError, ValueError):
                self._detach(pid, remove=False)

    # ---- deciding what to attach ----------------------------------------------------------------------
    def _reconcile(self, now):
        if self.ledger.exhausted:
            self.paused_reason = self.ledger.detail
            return
        kb = self.device.keyboard_title
        wanted = [i for i, t in self.targets.items() if t['type'] == 'page' and t['title'] == kb]
        for pid in list(self.pages):
            if pid not in wanted:
                self._detach(pid, remove=True)
        pending = [i for i in wanted if i not in self.pages]
        if not pending or now < self.next_attempt:
            return
        sjs = [i for i, t in self.targets.items() if t['type'] == 'page' and t['title'] == self.device.client_title]
        if not sjs:
            return          # Steam's hidden page is not there yet; we are woken by its Target event
        for i in sjs:
            self.first_seen.setdefault(i, now)
        verdict, detail = self._check_contract(sjs[0])
        if verdict == 'notready':
            if now - self.first_seen[sjs[0]] < self.cfg.not_ready_grace:
                return
            verdict, detail = 'fail', 'Steam page never finished loading SteamClient'
        if verdict == 'fail':
            if self.ledger.record_failure(detail, now, self.cfg.failure_spacing):
                self.next_attempt = now + self.cfg.failure_spacing
            return
        for i in pending:
            try:
                self._attach(i)
            except (ws.WsError, cdp.CdpError, OSError, ValueError):
                self.ledger.record_failure('could not install into the keyboard page', now, self.cfg.failure_spacing)
                self.next_attempt = now + self.cfg.failure_spacing
                return
        self.ledger.record_success()

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
            # WHY both: the registration brings the stub back at the start of every later document in this
            # page (UI reload) with no round trip; the evaluate puts it into the page that is open right now.
            # The registration lives only as long as this socket, so a dead helper leaves none behind.
            c.call('Page.addScriptToEvaluateOnNewDocument', {'source': self._stub()})
            c.evaluate(self._stub())
        except Exception:
            c.close()
            raise
        self.pages[target_id] = PageSession(target_id, c)
        self.last_inject = time.time()

    def _detach(self, target_id, remove):
        p = self.pages.pop(target_id, None)
        if not p:
            return
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
            self._publish('paused-steam-changed', 'Steam changed: ' + (self.ledger.detail or 'a needed part is missing') +
                          '. Stock keyboard in use.')
        elif self.pages:
            self._publish('active', '')
        else:
            self._publish('waiting-for-steam', 'Steam is up; waiting for its keyboard page.')

    def _publish(self, state, reason):
        self._write(state, reason)

    def _write(self, state, reason):
        key = (state, reason, len(self.pages), self.last_inject, self.browser_string)
        if key == self.last_written:
            return
        self.last_written = key
        self.status.write(state, reason, steam_browser=self.browser_string, layer_version=LAYER_VERSION,
                          last_inject=self.last_inject, attached_pages=len(self.pages))


def _browser_id(ver, port):
    """The browser socket path from /json/version, accepted only if it is our own loopback port."""
    host, p, path = config.check_ws_url(ver['webSocketDebuggerUrl'], port)
    prefix = '/devtools/browser/'
    if not path.startswith(prefix):
        raise ValueError('odd browser path')
    return path[len(prefix):]
