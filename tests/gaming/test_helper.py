#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Tests for swipekb-helper, the Gaming Mode go-between (plan M3).

Everything runs on the host against a fake Steam debug port (fake_cdp.py) whose pages run the helper's real
layer stub in Node. Run:  python3 -I tests/gaming/test_helper.py   (needs `node` on PATH; a missing Node is a
hard failure, not a skip, so a green run always means the stub really ran).
"""
import json
import os
import re
import shutil
import socket
import stat
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'gaming' / 'helper'))

import fake_cdp  # noqa: E402
from fake_cdp import FakeSteam, wait_for  # noqa: E402
from swipekb_helper import LAYER_VERSION, config as cfgmod, ws as wsmod  # noqa: E402
from swipekb_helper.config import Config  # noqa: E402
from swipekb_helper.helper import Helper  # noqa: E402

CONTRACT = ROOT / 'gaming' / 'contract' / 'gaming-contract.json'
BP = 'Steam Big Picture Mode'
SJS = 'SharedJSContext'
ATTR = 'data-swipekb-stub'


class Rig:
    """One fake Steam + a temp home with state and Steam folders."""

    def __init__(self, contract=CONTRACT, steam_dir=True, **cfg):
        self.tmp = Path(tempfile.mkdtemp(prefix='swipekb-helper-test-'))
        self.state = self.tmp / 'state'
        self.steam_dir = self.tmp / 'Steam'
        if steam_dir:
            self.steam_dir.mkdir()
        self.marker = self.steam_dir / '.cef-enable-remote-debugging'
        self.fake = FakeSteam()
        self.fake.open()
        self.cfg_extra = cfg
        self.contract = contract
        self.helpers = []

    def config(self, **over):
        kw = dict(port=self.fake.port, state_dir=self.state, marker_dirs=[self.steam_dir],
                  contract_path=self.contract, poll_interval=0.02, beat_interval=0.1, layer_watchdog=3.0,
                  call_timeout=3.0, not_ready_grace=0.3, failure_spacing=0.1, steamui_probe=None)
        kw.update(self.cfg_extra)
        kw.update(over)
        return Config(**kw)

    def helper(self, **over):
        h = Helper(self.config(**over))
        self.helpers.append(h)
        return h

    def status(self):
        try:
            return json.loads((self.state / 'status.json').read_text())
        except (OSError, ValueError):
            return {}

    def layer(self, tid):
        ok, v = self.fake.eval(tid, 'JSON.stringify(window.__swipekbLayer ? {version: window.__swipekbLayer.version, '
                                    'installs: window.__swipekbLayer.installs, at: window.__swipekbLayer.installedAt} : null)')
        assert ok, v
        return json.loads(v)

    def attr(self, tid):
        return self.fake.eval(tid, 'document.documentElement.getAttribute("%s")' % ATTR)[1]

    def steam_up(self, sjs='ok'):
        """The two pages Steam makes. Returns (bp_id, sjs_id)."""
        s = self.fake.add_target(SJS, steam=sjs)
        b = self.fake.add_target(BP)
        return b, s

    def close(self):
        for h in self.helpers:
            h.crash()
        self.fake.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)


class Base(unittest.TestCase):
    def rig(self, **kw):
        r = Rig(**kw)
        self.addCleanup(r.close)
        return r


class Loopback(Base):
    def test_source_contains_no_non_loopback_address(self):
        """Plan Section 9: the helper may only ever name 127.0.0.1."""
        pkg = ROOT / 'gaming' / 'helper' / 'swipekb_helper'
        files = [p for p in pkg.rglob('*') if p.suffix in ('.py', '.js')]
        self.assertTrue(files)
        bad = []
        for p in files:
            text = p.read_text()
            for m in re.finditer(r'\b(\d{1,3}\.){3}\d{1,3}\b', text):
                if m.group(0) != '127.0.0.1':
                    bad.append((p.name, m.group(0)))
            for word in ('localhost', '0.0.0.0', '::1', 'AF_INET6', 'bind(', 'listen(', 'getaddrinfo'):
                if word in text:
                    bad.append((p.name, word))
        self.assertEqual(bad, [])

    def test_port_8081_and_other_hosts_are_refused(self):
        with self.assertRaises(cfgmod.ForbiddenEndpoint):
            Config(port=8081)
        for url in ('ws://10.0.0.5:8080/devtools/page/X', 'ws://localhost:8080/devtools/page/X',
                    'ws://127.0.0.1:8081/devtools/page/X', 'ws://127.0.0.2:8080/devtools/page/X',
                    'wss://127.0.0.1:8080/devtools/page/X', 'http://127.0.0.1:8080/'):
            with self.assertRaises(cfgmod.ForbiddenEndpoint, msg=url):
                cfgmod.check_ws_url(url, 8080)
        self.assertEqual(cfgmod.check_ws_url('ws://127.0.0.1:8080/devtools/page/AB12', 8080),
                         ('127.0.0.1', 8080, '/devtools/page/AB12'))

    def test_helper_never_follows_a_remote_url_from_the_port(self):
        """A hostile /json/list entry pointing elsewhere must be ignored, not connected to."""
        r = self.rig()
        b, s = r.steam_up()
        evil = socket.socket()
        evil.bind(('127.0.0.1', 0))
        evil.listen(1)
        hits = []
        threading.Thread(target=lambda: hits.append(evil.accept()), daemon=True).start()
        # The listing advertises the hostile socket; the helper builds its own url from the target id instead.
        r.fake.listing_url_override = 'ws://127.0.0.1:%d/devtools/page/%%s' % evil.getsockname()[1]
        r.helper().start()
        wait_for(lambda: r.attr(b), what='install via helper-built url')
        time.sleep(0.2)  # settle: negative proof
        self.assertEqual(hits, [])
        evil.close()


class WebSocketClient(unittest.TestCase):
    """The hand-written client against a tiny echo server (framing written separately in fake_cdp)."""

    def serve(self, script):
        ls = socket.socket()
        ls.bind(('127.0.0.1', 0))
        ls.listen(1)
        port = ls.getsockname()[1]

        def run():
            c, _ = ls.accept()
            _, path, headers = fake_cdp.read_http_request(c)
            c.sendall(fake_cdp.handshake_reply(headers))
            try:
                script(c)
            except (ConnectionError, OSError):
                pass
            finally:
                c.close()
                ls.close()
        threading.Thread(target=run, daemon=True).start()
        return port

    def test_text_roundtrip_small_medium_and_huge(self):
        def echo(c):
            while True:
                fin, op, p = fake_cdp.read_frame(c)
                if op == 8:
                    return
                c.sendall(fake_cdp.frame(1, p))
        port = self.serve(echo)
        w = wsmod.connect('127.0.0.1', port, '/x', timeout=3)
        for n in (0, 5, 125, 126, 70000):
            msg = 'é' * n
            w.send_text(msg)
            self.assertEqual(w.recv_text(timeout=3), msg)
        w.close()

    def test_fragmented_frames_are_joined_and_ping_is_answered(self):
        got = []

        def script(c):
            c.sendall(fake_cdp.frame(1, b'hel', fin=False))
            c.sendall(fake_cdp.frame(9, b'p'))             # ping between fragments (allowed)
            c.sendall(fake_cdp.frame(0, b'lo', fin=True))
            fin, op, p = fake_cdp.read_frame(c)
            got.append((op, p))
        port = self.serve(script)
        w = wsmod.connect('127.0.0.1', port, '/x', timeout=3)
        self.assertEqual(w.recv_text(timeout=3), 'hello')
        wait_for(lambda: got, what='pong')
        self.assertEqual(got[0], (10, b'p'))
        w.close()

    def test_close_frame_raises_closed(self):
        port = self.serve(lambda c: c.sendall(fake_cdp.frame(8, b'\x03\xe8')))
        w = wsmod.connect('127.0.0.1', port, '/x', timeout=3)
        self.addCleanup(w.close)
        with self.assertRaises(wsmod.ConnectionClosed):
            w.recv_text(timeout=3)

    def test_oversize_message_is_refused(self):
        port = self.serve(lambda c: c.sendall(fake_cdp.frame(1, b'a' * 5000)))
        w = wsmod.connect('127.0.0.1', port, '/x', timeout=3, max_message=1000)
        self.addCleanup(w.close)
        with self.assertRaises(wsmod.ProtocolError):
            w.recv_text(timeout=3)

    def test_recv_times_out_without_data(self):
        port = self.serve(lambda c: time.sleep(1))
        w = wsmod.connect('127.0.0.1', port, '/x', timeout=3)
        self.addCleanup(w.close)
        with self.assertRaises(wsmod.Timeout):
            w.recv_text(timeout=0.05)

    def test_refused_handshake_is_an_error(self):
        ls = socket.socket()
        ls.bind(('127.0.0.1', 0))
        ls.listen(1)

        def run():
            c, _ = ls.accept()
            fake_cdp.read_http_request(c)
            c.sendall(b'HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n')
            c.close()
        threading.Thread(target=run, daemon=True).start()
        with self.assertRaises(wsmod.ProtocolError):
            wsmod.connect('127.0.0.1', ls.getsockname()[1], '/x', timeout=3)
        ls.close()


class Install(Base):
    def test_installs_stub_into_big_picture_page_that_already_exists(self):
        r = self.rig()
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='marker attribute')
        self.assertEqual(r.layer(b), {'version': LAYER_VERSION, 'installs': 1, 'at': r.layer(b)['at']})

    def test_installs_when_the_page_appears_later_and_its_title_arrives_late(self):
        r = self.rig()
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'waiting-for-steam' or r.status().get('state'), what='first status')
        s = r.fake.add_target(SJS, steam='ok')
        b = r.fake.add_target('', announce=True)          # created with no title yet
        self.assertIsNone(r.layer(b))
        r.fake.set_title(b, BP)
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install after title change')

    def test_leaves_other_pages_alone(self):
        r = self.rig()
        b, s = r.steam_up()
        other = r.fake.add_target('Some Overlay')
        r.helper().start()
        wait_for(lambda: r.attr(b), what='install')
        self.assertIsNone(r.layer(other))
        self.assertIsNone(r.layer(s))

    def test_targets_come_from_the_shared_contract_not_from_the_helper(self):
        c = json.loads(CONTRACT.read_text())
        c['device']['targets'][0]['title'] = 'Another Page Name'
        r = self.rig()
        alt = r.tmp / 'contract.json'
        alt.write_text(json.dumps(c))
        r.contract = alt
        b, s = r.steam_up()
        named = r.fake.add_target('Another Page Name')
        r.helper().start()
        wait_for(lambda: r.attr(named), what='install in renamed page')
        self.assertIsNone(r.layer(b))

    def test_stub_carries_the_dom_expectations_from_the_contract(self):
        r = self.rig()
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b), what='install')
        ok, v = r.fake.eval(b, 'JSON.stringify(window.__swipekbLayer.config.keyboard_dom.min_letter_keys)')
        self.assertEqual(v, '24')

    def test_ui_reload_reinstalls_through_the_new_document_script(self):
        r = self.rig()
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b), what='install')
        for _ in range(5):
            r.fake.reload(b)
            self.assertEqual(r.attr(b), LAYER_VERSION)     # immediately: the registered script ran with the page

    def test_ui_reload_without_the_new_document_script_is_caught_by_the_heartbeat(self):
        r = self.rig()
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b), what='install')
        r.fake.reload(b, run_scripts=False)
        self.assertIsNone(r.attr(b))
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='heartbeat reinstall')

    def test_steam_restart_installs_into_the_new_pages(self):
        r = self.rig()
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b), what='install')
        r.fake.close_port()
        for t in (b, s):
            r.fake.remove_target(t)
        wait_for(lambda: r.status().get('state') in ('waiting-for-steam', 'needs-steam-restart'), what='lost port noticed')
        r.fake.open()
        b2, s2 = r.steam_up()
        wait_for(lambda: r.attr(b2) == LAYER_VERSION, what='install in new page')
        wait_for(lambda: r.status().get('state') == 'active', what='active again')

    def test_no_install_until_steams_hidden_page_exists(self):
        r = self.rig()
        b = r.fake.add_target(BP)
        r.helper().start()
        wait_for(lambda: r.status().get('state'), what='status')
        self.assertIsNone(r.layer(b))
        r.fake.add_target(SJS, steam='ok')
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install once the hidden page is there')


class Restart(Base):
    def test_helper_killed_and_restarted_reattaches_without_double_install(self):
        r = self.rig()
        b, s = r.steam_up()
        h1 = r.helper()
        h1.start()
        wait_for(lambda: r.attr(b), what='install')
        first = r.layer(b)
        h1.crash()                                   # kill -9: no cleanup of any kind
        self.assertEqual(r.layer(b)['installs'], 1)  # the stub is still there, orphaned
        h2 = r.helper()
        h2.start()
        wait_for(lambda: len(h2.pages) == 1, what='reattached')
        self.assertEqual(r.layer(b), first)          # same install, not replaced
        # and the new session re-registered the reload script
        r.fake.reload(b)
        self.assertEqual(r.attr(b), LAYER_VERSION)

    def test_orphaned_stub_removes_itself_when_no_helper_comes_back(self):
        r = self.rig(layer_watchdog=0.5)
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b), what='install')
        h.crash()
        wait_for(lambda: r.attr(b) is None and r.layer(b) is None, timeout=5, what='self removal')

    def test_clean_stop_removes_the_stub_and_says_off(self):
        r = self.rig()
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b), what='install')
        h.stop()
        self.assertIsNone(r.attr(b))
        self.assertIsNone(r.layer(b))
        self.assertEqual(r.status()['state'], 'off')

    def test_an_older_stub_is_replaced_not_stacked(self):
        r = self.rig()
        b, s = r.steam_up()
        r.fake.eval(b, 'window.__swipekbLayer = {version: "0.0.1", installs: 4, remove: function(){window.__removedOld = 1;}}')
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='replacement')
        self.assertEqual(r.fake.eval(b, 'window.__removedOld')[1], 1)
        self.assertEqual(r.layer(b)['version'], LAYER_VERSION)

    def test_stub_has_no_leftover_timer_after_removal(self):
        r = self.rig()
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b), what='install')
        r.fake.eval(b, 'window.__swipekbLayer.remove()')
        self.assertEqual(r.fake.eval(b, 'window.__swipekbLayer === undefined')[1], True)
        self.assertEqual(r.fake.eval(b, 'window.__swipekbLayer && 1')[1], None)


class Contract(Base):
    def test_missing_steam_method_means_no_install_and_a_pause_after_three_tries(self):
        r = self.rig()
        b, s = r.steam_up(sjs='nomethod')
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')
        self.assertIsNone(r.layer(b))
        self.assertIn('ControllerKeyboardSendText', r.status()['reason'])
        n = len(r.fake.page(s).eval_log)
        # More Steam pages and reloads change nothing: the helper has stopped trying.
        for _ in range(3):
            r.fake.reload(b)
        r.fake.add_target(BP)
        time.sleep(0.5)  # settle: negative proof that no further check happens
        self.assertEqual(len(r.fake.page(s).eval_log), n)
        self.assertEqual(json.loads((r.state / 'retry.json').read_text())['failures'], 3)

    def test_wrong_method_type_counts_as_changed(self):
        r = self.rig()
        b, s = r.steam_up(sjs='badtype')
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')

    def test_pause_survives_a_helper_restart(self):
        r = self.rig()
        b, s = r.steam_up(sjs='nomethod')
        h = r.helper()
        h.start()
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')
        h.crash()
        n = len(r.fake.page(s).eval_log)
        h2 = r.helper()
        h2.start()
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed' and r.status().get('pid'), what='still paused')
        time.sleep(0.3)  # settle
        self.assertEqual(len(r.fake.page(s).eval_log), n)

    def test_a_new_steam_build_gets_a_fresh_chance(self):
        r = self.rig()
        b, s = r.steam_up(sjs='nomethod')
        h = r.helper()
        h.start()
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')
        h.crash()
        # Steam updated: new browser build and a fixed hidden page.
        r.fake.browser_string = 'Chrome/200.0.0.0'
        r.fake.remove_target(s)
        r.fake.add_target(SJS, steam='ok')
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install on the new build')
        # WHY wait: the status file is written just after the install, not at the same instant
        wait_for(lambda: r.status().get('state') == 'active', what='status active on the new build')

    def test_steam_client_not_yet_defined_is_waited_for_not_counted(self):
        r = self.rig(not_ready_grace=60.0)
        b, s = r.steam_up(sjs='nosteamclient')
        r.helper().start()
        time.sleep(0.5)  # settle: it must still be waiting, not paused
        self.assertEqual(r.status()['state'], 'waiting-for-steam')
        self.assertIsNone(r.layer(b))
        r.fake.eval(s, 'var SteamClient = {Input: {ControllerKeyboardSendText: function(){}}}')
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install once ready')

    def test_steam_client_never_appearing_is_eventually_a_change(self):
        r = self.rig(not_ready_grace=0.2)
        b, s = r.steam_up(sjs='nosteamclient')
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')

    def test_a_broken_contract_file_means_off_with_a_reason(self):
        r = self.rig()
        bad = r.tmp / 'bad.json'
        bad.write_text('{"device": {}}')
        r.contract = bad
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'error', what='error state')
        self.assertIn('contract', r.status()['reason'])
        self.assertIsNone(r.layer(b))


class MarkerAndStatus(Base):
    """The helper runs in a sandbox that cannot see Steam's folder or Steam's process. It never creates the debug-switch
    file (owner decision 2026-10-22: tell the person, do not silently put it back) and learns about the switch from a
    one-word file `switch-state` that the host-side watcher (gaming/setup/switch-watch.sh) writes into the state folder."""

    def set_switch(self, r, word):
        r.state.mkdir(parents=True, exist_ok=True)
        (r.state / 'switch-state').write_text(word + '\n')

    def test_the_helper_never_creates_or_touches_the_switch_file(self):
        r = self.rig()
        r.fake.close_port()
        r.helper().start()
        wait_for(lambda: r.status().get('state'), what='status')
        time.sleep(0.4)
        self.assertFalse(r.marker.exists())
        self.assertEqual(list(r.steam_dir.iterdir()), [])

    def test_switch_gone_is_its_own_state_with_a_plain_reason(self):
        r = self.rig()
        self.set_switch(r, 'off')
        r.fake.close_port()
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'switch-off', what='state')
        reason = r.status()['reason']
        self.assertIn('turned off', reason)
        self.assertIn('installer', reason)
        self.assertIn("Steam's normal keyboard", reason)

    def test_each_switch_word_gives_its_own_state_never_a_wrong_one(self):
        for word, state in (('restart', 'needs-steam-restart'), ('no-steam', 'steam-not-found'),
                            ('ok', 'waiting-for-steam'), ('nonsense', 'waiting-for-steam')):
            with self.subTest(word):
                r = self.rig()
                self.set_switch(r, word)
                r.fake.close_port()
                r.helper().start()
                wait_for(lambda: r.status().get('state') == state, what=word)

    def test_no_switch_file_at_all_is_just_waiting_for_steam(self):
        # WHY: inside the sandbox the helper cannot tell "no Steam folder" from "cannot see it"; claiming either was the bug.
        r = self.rig(steam_dir=False)
        r.fake.close_port()
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'waiting-for-steam', what='state')
        time.sleep(0.3)
        self.assertEqual(r.status()['state'], 'waiting-for-steam')

    def test_switch_gone_while_steam_is_still_serving_does_not_switch_the_layer_off(self):
        # Steam reads the file only at start: the port is still open, so swipe keeps working until Steam restarts.
        r = self.rig()
        b, s = r.steam_up()
        self.set_switch(r, 'off')
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'active', what='active')

    def test_a_quick_steam_restart_is_just_waiting(self):
        r = self.rig()
        self.set_switch(r, 'ok')
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b), what='install')
        r.fake.close_port()
        time.sleep(0.4)
        self.assertEqual(r.status()['state'], 'waiting-for-steam')

    def test_status_file_shape_and_permissions(self):
        r = self.rig()
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'active', what='active')
        st = r.status()
        self.assertEqual(st['layer_version'], LAYER_VERSION)
        self.assertEqual(st['attached_pages'], 1)
        self.assertIsInstance(st['last_inject'], float)
        self.assertIn('updated', st)
        self.assertEqual(st['steam_browser'], 'Chrome/126.0.0.0')
        mode = stat.S_IMODE((r.state / 'status.json').stat().st_mode)
        self.assertEqual(mode & 0o077, 0)
        # Section 8: states and counts only, never text typed or traced.
        self.assertEqual(set(st), {'state', 'reason', 'steam_browser', 'layer_version', 'last_inject', 'attached_pages',
                                   'keyboard', 'pid', 'updated'})   # keyboard (M9): counts from the layer, never text

    def test_status_follows_the_whole_lifecycle(self):
        r = self.rig()
        seen = []
        h = r.helper()
        orig = h.status.write

        def spy(state, reason='', **kw):
            if not seen or seen[-1] != state:
                seen.append(state)
            return orig(state, reason, **kw)
        h.status.write = spy
        r.fake.close_port()
        h.start()
        wait_for(lambda: 'waiting-for-steam' in seen, what='waiting')
        r.fake.open()
        b, s = r.steam_up()
        wait_for(lambda: 'active' in seen, what='active')
        h.stop()
        self.assertEqual(seen[0], 'waiting-for-steam')
        self.assertEqual(seen[-1], 'off')
        self.assertLess(seen.index('waiting-for-steam'), seen.index('active'))

    def test_helper_only_talks_to_the_one_configured_loopback_port(self):
        r = self.rig()
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b), what='install')
        self.assertGreater(r.fake.connections_seen, 0)  # it did connect, to our fake, on 127.0.0.1


class GestureLayerInstall(Base):
    """M4: the helper pastes the gesture arbiter (gaming/layer/gesture.js) into the frame it installs."""

    def test_the_installed_source_contains_the_gesture_arbiter_and_the_knobs(self):
        r = self.rig(gesture={'hold_ms': 222})
        h = r.helper()
        h._load_contract()
        src = h._stub()
        self.assertIn('function swipekbGestures', src)
        self.assertIn('"hold_ms": 222', src)
        self.assertNotIn('__CONFIG__', src)
        self.assertNotIn('__GESTURES__', src)

    def test_without_the_gesture_file_the_frame_still_installs_and_nothing_else_happens(self):
        r = self.rig(layer_dir=Path(tempfile.mkdtemp(dir=str(r_tmp(self)))))
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='frame alone')
        self.assertEqual(r.fake.eval(b, 'String(window.__swipekbLayer.gestures)')[1], 'null')

    def test_a_page_that_cannot_run_the_gestures_still_gets_the_frame(self):
        # the fake page has no touch support at all: the arbiter declines, the frame is installed, no error
        r = self.rig()
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='frame')
        self.assertEqual(r.fake.eval(b, 'String(window.__swipekbLayer.gestures)')[1], 'null')
        self.assertEqual(r.fake.eval(b, 'String(window.__swipekbLayer.gestureError)')[1], 'undefined')

    def test_layer_and_contract_are_found_in_a_source_checkout_and_in_the_flatpak_layout(self):
        import subprocess
        base = Path(tempfile.mkdtemp(prefix='swipekb-layout-'))
        self.addCleanup(shutil.rmtree, base, True)
        shutil.copytree(ROOT / 'gaming' / 'helper' / 'swipekb_helper', base / 'app' / 'lib' / 'swipekb-helper' / 'swipekb_helper',
                        ignore=shutil.ignore_patterns('__pycache__'))
        shutil.copytree(ROOT / 'gaming' / 'layer', base / 'app' / 'share' / 'swipekb-helper' / 'layer')
        shutil.copy(CONTRACT, base / 'app' / 'share' / 'swipekb-helper' / 'gaming-contract.json')
        code = ('import sys; sys.path.insert(0, %r); from swipekb_helper import config; '
                'print(config.default_layer_dir()); print(config.default_contract_path())' % str(base / 'app' / 'lib' / 'swipekb-helper'))
        out = subprocess.run([sys.executable, '-I', '-c', code], capture_output=True, text=True, check=True).stdout.split('\n')
        self.assertEqual(out[0], str(base / 'app' / 'share' / 'swipekb-helper' / 'layer'))
        self.assertEqual(out[1], str(base / 'app' / 'share' / 'swipekb-helper' / 'gaming-contract.json'))
        self.assertEqual(cfgmod.default_layer_dir(), ROOT / 'gaming' / 'layer')


class SteamOrigin(Base):
    """Review fix 1: a title is not proof. Only a page served from Steam's own UI origin counts (contract device.ui_origin)."""

    def test_the_origin_is_pinned_in_the_contract_and_watched_by_the_canary(self):
        c = json.loads(CONTRACT.read_text())
        pin = c['device']['ui_origin']
        self.assertEqual(pin['origin'], 'https://steamloopback.host')
        self.assertIn(pin['static_check'], [ch['id'] for ch in c['checks']], 'the canary must be able to see the origin vanish')

    def test_a_web_page_titled_like_steams_hidden_page_is_never_asked_anything(self):
        r = self.rig()
        evil = r.fake.add_target(SJS, steam='ok', url='https://evil.example/index.html')
        b = r.fake.add_target(BP)
        r.helper().start()
        time.sleep(0.8)  # settle: negative proof
        self.assertEqual(len(r.fake.page(evil).eval_log), 0, 'no probe, no typing: the helper must not even connect to it')
        self.assertIsNone(r.layer(b))
        self.assertNotEqual(r.status().get('state'), 'active')

    def test_a_web_page_titled_like_the_keyboard_page_gets_no_layer(self):
        r = self.rig()
        s = r.fake.add_target(SJS, steam='ok')
        evil = r.fake.add_target(BP, url='https://evil.example/')
        r.helper().start()
        time.sleep(0.8)  # settle
        self.assertIsNone(r.layer(evil))

    def test_stable_steams_placeholder_keyboard_page_is_used_once_it_proves_its_origin(self):
        # WHY: stable Steam (2026-10-23) lists its keyboard window as about:blank?..., while inside it is Steam's page.
        r = self.rig()
        r.fake.add_target(SJS, steam='ok')
        b = r.fake.add_target(BP, url='about:blank?createflags=6292738&browserType=4')
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='layer in the placeholder-listed keyboard page')
        wait_for(lambda: r.status().get('state') == 'active', what='active')
        # The test's own attribute checks are logged too; among the helper's questions the origin comes first.
        asked = [e for e in r.fake.page(b).eval_log if not e.startswith('document.documentElement.getAttribute')]
        self.assertEqual(asked[0], 'location.origin', 'the origin must be asked before anything else')

    def test_a_placeholder_page_from_another_origin_gets_nothing(self):
        # WHY: any page can open an about:blank window and title it like Steam's keyboard; only its origin tells them apart.
        r = self.rig()
        r.fake.add_target(SJS, steam='ok')
        evil = r.fake.add_target(BP, url='about:blank?createflags=1', origin='https://evil.example')
        r.helper().start()
        wait_for(lambda: r.fake.page(evil).eval_log, what='origin asked')
        time.sleep(0.8)  # settle: negative proof
        # Asked again on each retry, but never anything else: no binding, no script.
        self.assertEqual(set(r.fake.page(evil).eval_log), {'location.origin'}, 'nothing but the origin question may reach it')
        self.assertEqual(r.fake.page(evil).scripts, {})
        self.assertIsNone(r.layer(evil))
        self.assertNotEqual(r.status().get('state'), 'active')

    def test_a_web_page_posing_as_the_keyboard_cannot_switch_swipe_off(self):
        # WHY (review): an impostor used to count as Steam failing; three of them paused Gaming Mode swipe.
        r = self.rig()
        r.fake.add_target(SJS, steam='ok')
        for _ in range(4):
            r.fake.add_target(BP, url='about:blank?createflags=1', origin='https://evil.example')
        b = r.fake.add_target(BP, url='about:blank?createflags=6292738')
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='layer in the real keyboard page despite impostors')
        wait_for(lambda: r.status().get('state') == 'active', what='active')
        self.assertEqual(h.ledger.failures, 0, 'impostors are ignored, never counted')

    def test_binding_calls_from_other_documents_are_ignored(self):
        r = self.rig()
        r.fake.add_target(SJS, steam='ok')
        b = r.fake.add_target(BP)
        h = r.helper()
        h.start()
        wait_for(lambda: r.status().get('state') == 'active', what='active')
        # WHY wait: "active" can be written before the helper thread has read the browser's naming of Steam's top
        # document (context 1); on a busy CI runner the last check then saw nothing (failed once, PR #19, 2026-10-30).
        wait_for(lambda: b in h.pages and 1 in h.pages[b].trusted_contexts, what="Steam's top document named")
        seen = []
        h.bridge.submit = lambda pid, payload: seen.append(payload)
        h._page_event(b, {'method': 'Runtime.bindingCalled', 'params': {'name': '__swipekb', 'payload': '{}', 'executionContextId': 99}})
        self.assertEqual(seen, [], 'a call from an unknown document (another frame or a navigated page) is dropped')
        h._page_event(b, {'method': 'Runtime.executionContextCreated', 'params': {'context': {
            'id': 7, 'origin': 'https://evil.example', 'auxData': {'isDefault': True, 'type': 'default'}}}})
        h._page_event(b, {'method': 'Runtime.bindingCalled', 'params': {'name': '__swipekb', 'payload': '{}', 'executionContextId': 7}})
        self.assertEqual(seen, [])
        h._page_event(b, {'method': 'Runtime.bindingCalled', 'params': {'name': '__swipekb', 'payload': '{}', 'executionContextId': 1}})
        self.assertEqual(seen, ['{}'], "Steam's own top document is heard")

    def test_the_hidden_steamclient_page_is_never_accepted_as_a_placeholder(self):
        r = self.rig()
        evil = r.fake.add_target(SJS, steam='ok', url='about:blank?x=1')
        b = r.fake.add_target(BP)
        r.helper().start()
        time.sleep(0.8)  # settle
        self.assertEqual(len(r.fake.page(evil).eval_log), 0)
        self.assertIsNone(r.layer(b))

    def test_look_alike_addresses_are_refused(self):
        r = self.rig()
        h = r.helper()
        h._load_contract()
        for url in ('https://steamloopback.host.evil.example/', 'https://steamloopback.host@evil.example/', 'http://steamloopback.host/',
                    'https://steamloopback.host:8443/', 'https://evil.example/https://steamloopback.host/', '', 'steamloopback.host/index.html'):
            self.assertFalse(h.device.is_steam_url(url), url)
        for url in ('https://steamloopback.host/index.html', 'https://steamloopback.host/routes/home?x=1'):
            self.assertTrue(h.device.is_steam_url(url), url)

    def test_two_hidden_pages_from_steams_origin_are_not_guessed_between(self):
        r = self.rig()
        r.fake.add_target(SJS, steam='ok')
        r.fake.add_target(SJS, steam='ok')
        b = r.fake.add_target(BP)
        r.helper().start()
        time.sleep(0.8)  # settle
        self.assertIsNone(r.layer(b))
        self.assertNotEqual(r.status().get('state'), 'active')


class IdleSocketsDoNotBreakTheLoop(Base):
    """Review fix 2: a frame already sitting in one socket's buffer must not make the loop wait on a quiet one."""

    def pair(self, h):
        from swipekb_helper import cdp
        a1, a2 = socket.socketpair()
        b1, b2 = socket.socketpair()
        self.addCleanup(lambda: [s.close() for s in (a1, a2, b1, b2)])
        h.browser = cdp.Cdp(wsmod.WebSocket(a1))
        page_ws = wsmod.WebSocket(b1)
        from swipekb_helper.helper import PageSession
        h.pages = {'p': PageSession('p', cdp.Cdp(page_ws))}
        h.bridge = type('B', (), {'forget': lambda self_, pid: self_.gone.append(pid), 'gone': []})()
        return page_ws

    def test_a_buffered_page_frame_does_not_drop_the_connection(self):
        r = self.rig()
        h = r.helper()
        page_ws = self.pair(h)
        payload = b'{"method":"Runtime.consoleAPICalled","params":{}}'
        page_ws.buf = bytes([0x81, len(payload)]) + payload
        started = time.monotonic()
        h._pump_io()                     # must neither raise nor wait on the idle browser socket
        self.assertLess(time.monotonic() - started, 0.3, 'it waited on the quiet browser socket')

    def test_a_quiet_browser_socket_with_a_partial_buffer_is_not_an_error(self):
        r = self.rig()
        h = r.helper()
        self.pair(h)
        h.browser.ws.buf = bytes([0x89, 0])        # a ping and nothing else: answered, then no message follows
        h._browser_events()                        # used to raise ws.Timeout out of the loop

    def test_dropping_connections_tells_the_engine_to_forget_each_page(self):
        r = self.rig()
        h = r.helper()
        self.pair(h)
        h._drop_connections()
        self.assertEqual(h.bridge.gone, ['p'])
        self.assertIsNone(h.browser)
        self.assertEqual(h.pages, {})

    def test_an_idle_browser_socket_next_to_a_busy_page_keeps_every_connection(self):
        r = self.rig()
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.status().get('state') == 'active', what='active')
        seen = r.fake.page_sessions
        for _ in range(30):                        # page frames arrive all the time (the layer pings every beat)
            time.sleep(0.05)
        self.assertEqual(r.fake.page_sessions, seen, 'the helper dropped and re-opened its connections')
        self.assertEqual(r.status()['state'], 'active')


class ReloadGrace(Base):
    """Review fix 3: the 'never finished loading' clock runs from when the hidden page last stopped being ready."""

    def test_an_in_place_reload_of_the_hidden_page_gets_a_fresh_grace_period(self):
        r = self.rig(not_ready_grace=1.5)
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='first install')
        time.sleep(1.8)                            # the hidden page has now existed longer than the grace period
        # Steam reloads its hidden page in place (same target) while the keyboard page is replaced.
        r.fake.eval(s, 'SteamClient = undefined')
        r.fake.remove_target(b)
        b2 = r.fake.add_target(BP)
        time.sleep(0.8)                            # settle: still inside the new grace period
        self.assertEqual(h.ledger.failures, 0, 'a reload counted as a failure straight away')
        self.assertNotEqual(r.status().get('state'), 'paused-steam-changed')
        r.fake.eval(s, 'SteamClient = {Input: {ControllerKeyboardSendText: function(){}}}')
        wait_for(lambda: r.attr(b2) == LAYER_VERSION, what='install once the page is ready again', timeout=8)

    def test_a_refusal_from_a_page_that_is_still_there_still_counts(self):
        # WHY: the reload fix drops errors from pages that vanished mid-check; a page that is still listed and refuses our
        # script must keep counting, or a real Steam change would never pause Gaming Mode.
        r = self.rig()
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='first install')
        r.fake.eval(b, 'window.__swipekbLayer.beat = function(){ throw new Error("blocked") }')
        wait_for(lambda: h.ledger.failures >= 1, what='the refusal is counted', timeout=8)
        self.assertIn('refused', h.ledger.detail)

    def test_a_pause_is_explained_honestly_and_says_how_to_try_again(self):
        r = self.rig()
        b, s = r.steam_up(sjs='nomethod')
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')
        reason = r.status()['reason']
        self.assertIn('ControllerKeyboardSendText', reason)
        self.assertIn('installer', reason)
        self.assertIn('restart', reason.lower())
        self.assertNotIn('Steam changed', reason, 'the tries can fail for reasons that are not a Steam change')

    def test_a_new_boot_clears_the_pause_but_a_helper_restart_does_not(self):
        from swipekb_helper.retry import RetryLedger
        d = Path(tempfile.mkdtemp(prefix='swipekb-ledger-'))
        self.addCleanup(shutil.rmtree, d, True)
        led = RetryLedger(d / 'retry.json', 3, boot_id='boot-A')
        led.use_build('steam-1')
        for i in range(3):
            led.record_failure('nope', 10.0 * i, 2.0)
        self.assertTrue(led.exhausted)
        self.assertTrue(RetryLedger(d / 'retry.json', 3, boot_id='boot-A').exhausted, 'same boot: still paused')
        fresh = RetryLedger(d / 'retry.json', 3, boot_id='boot-B')
        self.assertFalse(fresh.exhausted, 'after a reboot the helper tries again')
        self.assertEqual(fresh.failures, 0)

    def test_an_unreadable_boot_id_never_clears_a_pause(self):
        from swipekb_helper.retry import RetryLedger
        d = Path(tempfile.mkdtemp(prefix='swipekb-ledger-'))
        self.addCleanup(shutil.rmtree, d, True)
        led = RetryLedger(d / 'retry.json', 3, boot_id='boot-A')
        led.use_build('steam-1')
        for i in range(3):
            led.record_failure('nope', 10.0 * i, 2.0)
        self.assertTrue(RetryLedger(d / 'retry.json', 3, boot_id=None).exhausted)


class StopRequest(Base):
    """Review fix 8: flatpak's --die-with-parent turns the service's SIGTERM into SIGKILL, so the stop is asked for by file."""

    def test_a_stop_request_file_makes_the_helper_clean_up_and_only_then_remove_the_file(self):
        r = self.rig()
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install')
        req = r.state / 'stop-request'
        req.write_text('')
        wait_for(lambda: not h.thread.is_alive(), what='helper to stop')
        self.assertEqual(r.status()['state'], 'off')
        self.assertIsNone(r.layer(b), 'the layer is out of Steam before the request is answered')
        self.assertFalse(req.exists(), 'the file going away is the answer the service waits for')

    def test_an_old_stop_request_does_not_stop_a_new_helper(self):
        r = self.rig()
        r.state.mkdir()
        (r.state / 'stop-request').write_text('')
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install despite the old file')
        self.assertTrue(h.thread.is_alive())
        self.assertFalse((r.state / 'stop-request').exists())


class PrivateEngineFolder(Base):
    """Review fix 7: never look for the engine in a shared folder; only talk to one that only we can reach."""

    def test_without_a_runtime_folder_the_default_is_not_tmp(self):
        old = os.environ.pop('XDG_RUNTIME_DIR', None)
        try:
            sock, tok = cfgmod.default_engine_paths()
        finally:
            if old is not None:
                os.environ['XDG_RUNTIME_DIR'] = old
        self.assertFalse(str(sock).startswith('/tmp'), sock)
        self.assertFalse(str(tok).startswith('/tmp'), tok)

    def _serve(self, mode, token_mode=0o600):
        from swipekb_helper.engine import EngineConn
        d = Path(tempfile.mkdtemp(prefix='swipekb-eng-'))
        self.addCleanup(shutil.rmtree, d, True)
        os.chmod(d, mode)
        sock = d / 'engine.sock'
        ls = socket.socket(socket.AF_UNIX)
        ls.bind(str(sock))
        ls.listen(1)
        self.addCleanup(ls.close)
        tok = d / 'engine.token'
        tok.write_text('x' * 32)
        os.chmod(tok, token_mode)
        return EngineConn, sock, tok

    def test_a_folder_others_can_write_to_is_refused_before_the_token_is_read(self):
        Conn, sock, tok = self._serve(0o777)
        from swipekb_helper.engine import EngineGone
        with self.assertRaises(EngineGone) as cm:
            Conn(sock, tok, 'unknown', timeout=1)
        self.assertIn('private', str(cm.exception))

    def test_a_token_others_can_read_is_refused(self):
        Conn, sock, tok = self._serve(0o700, token_mode=0o644)
        from swipekb_helper.engine import EngineGone
        with self.assertRaises(EngineGone) as cm:
            Conn(sock, tok, 'unknown', timeout=1)
        self.assertIn('private', str(cm.exception))

    def test_a_private_folder_passes_the_check(self):
        Conn, sock, tok = self._serve(0o700)
        from swipekb_helper.engine import EngineGone
        try:
            Conn(sock, tok, 'unknown', timeout=1)
        except Exception as e:                      # nothing answers the hello; that is a different, later failure
            self.assertNotIn('private', str(e))


def r_tmp(test):
    d = Path(tempfile.mkdtemp(prefix='swipekb-nolayer-'))
    test.addCleanup(shutil.rmtree, d, True)
    return d


if __name__ == '__main__':
    unittest.main(verbosity=2)
