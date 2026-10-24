#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Tests for the typing half of the helper (plan M5): the __swipekb binding, the checks on every page message, the engine
conversation, the commit through Steam's text call, and what is (not) written to disk.

Everything runs on the host: a fake Steam debug port (fake_cdp.py), a fake engine daemon that follows the real daemon's rules about
candidate ids (fake_daemon.py), and Node for the page side. Run:  python3 -I tests/gaming/test_commit.py
"""
import hashlib
import json
import os
import sys
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'gaming' / 'helper'))

from fake_cdp import wait_for  # noqa: E402
from fake_daemon import FakeDaemon  # noqa: E402
from swipekb_helper import LAYER_VERSION  # noqa: E402
from swipekb_helper import bridge as bridgemod  # noqa: E402
from test_helper import Rig  # noqa: E402

LETTERS = 'qwertyuiopasdfghjklzxcvbnm'
ROWS = ['qwertyuiop', 'asdfghjkl', 'zxcvbnm']


def keys_msg():
    """The 26 letter keys as the layer reports them: centre point and size, 90 x 74 pixel keys."""
    out = []
    for r, row in enumerate(ROWS):
        for c, ch in enumerate(row):
            out.append({'k': ch, 'x': 145 + c * 90 + r * 40, 'y': 437 + r * 80, 'w': 90, 'h': 74})
    return out


def stroke(id=1, **over):
    m = {'v': 1, 'type': 'stroke', 'id': id, 'trace': [[145, 437, 0], [300, 437, 40], [500, 517, 90]], 'vw': 1280, 'vh': 800,
         'keys': keys_msg(), 'ctx': [], 'mode': 'unknown', 'layout': 'qwerty', 'caps': 'none'}
    m.update(over)
    return m


def snapshot(root):
    """Every file under `root`: path -> (size, checksum). A directory counts too."""
    out = {}
    for p in sorted(Path(root).rglob('*')):
        try:
            if p.is_dir():
                out[str(p)] = ('dir',)
            elif p.is_file():
                out[str(p)] = (p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest())
        except OSError:
            pass
    return out


class Typing(unittest.TestCase):
    def setUp(self):
        self.r = Rig()
        self.addCleanup(self.r.close)
        self.d = FakeDaemon(str(self.r.tmp))
        self.addCleanup(self.d.stop)

    # ---- helpers
    def start(self, **over):
        self.bp, self.sjs = self.r.steam_up()
        h = self.r.helper(engine_socket=self.d.sock_path, engine_token=self.d.token_path, **over)
        h.start()
        wait_for(lambda: self.r.attr(self.bp) == LAYER_VERSION, what='layer installed')
        wait_for(lambda: self.r.status().get('state') == 'active', what='active')
        # The fake page has no real session; this stand-in just records what the helper tells the page.
        ok, v = self.r.fake.eval(self.bp, 'window.__got = []; window.__swipekbLayer.session = {result: function (r) { window.__got.push(JSON.stringify(r)); }}; 1')
        self.assertTrue(ok, v)
        return h

    def send(self, msg, raw=False):
        text = msg if raw else json.dumps(msg)
        ok, v = self.r.fake.eval(self.bp, 'window.__swipekb(%s)' % json.dumps(text))
        self.assertTrue(ok, v)

    def got(self):
        return [json.loads(x) for x in json.loads(self.r.fake.eval(self.bp, 'JSON.stringify(window.__got)')[1])]

    def sent(self):
        return json.loads(self.r.fake.eval(self.sjs, 'JSON.stringify(__sent)')[1])

    def wait_got(self, n=1):
        wait_for(lambda: len(self.got()) >= n, what='%d answer(s) in the page' % n)
        return self.got()

    # ---- the happy path
    def test_a_swipe_types_the_best_word_and_a_space_and_fills_the_strip(self):
        self.start()
        self.send(stroke(ctx=['we']))
        a = self.wait_got()[0]
        self.assertEqual(self.sent(), ['hello '])
        self.assertTrue(a['committed'])
        self.assertEqual((a['kind'], a['id'], a['typed']), ('stroke', 1, 'hello '))
        self.assertEqual([c['word'] for c in a['cands']], ['hello', 'hell', 'jello'])
        self.assertEqual(sorted(a['ms']), ['engine', 'send', 'wait'], 'timings (numbers only) come back for diagnosis')
        self.assertTrue(all(isinstance(v, int) and v >= 0 for v in a['ms'].values()))
        # what the engine was asked
        lay = self.d.ops('layout')[0][2]
        q = next(k for k in lay['keys'] if k['c'] == 'q')
        self.assertEqual((q['x'], q['y'], q['w'], q['h']), (100, 400, 90, 74), 'centre -> top-left corner')
        self.assertEqual(lay['steam_layout'], 'qwerty')
        sw = self.d.ops('swipe')[0][2]
        self.assertEqual(sw['context'], ['we'])
        self.assertEqual(sw['top_k'], 3)
        self.assertEqual(len(sw['points']), 3)
        self.assertEqual(self.d.ops('hello')[0][2]['mode'], 'unknown')

    def test_a_web_page_that_copies_the_hidden_pages_title_never_receives_a_word(self):
        """Review fix 1: typing goes to the page Steam serves, not to the first page that carries the title."""
        evil = self.r.fake.add_target('SharedJSContext', steam='ok', url='https://evil.example/')
        self.start()
        self.send(stroke())
        self.wait_got()
        self.assertEqual(self.sent(), ['hello '])
        self.assertEqual(len(self.r.fake.page(evil).eval_log), 0, 'nothing at all was sent to the look-alike page')

    def test_capitals_are_the_helpers_job_and_follow_the_layers_request(self):
        self.start()
        self.send(stroke(1, caps='first')); self.wait_got(1)
        self.send(stroke(2, caps='all')); self.wait_got(2)
        self.assertEqual(self.sent(), ['Hello ', 'HELLO '])
        self.assertEqual([c['word'] for c in self.got()[1]['cands']], ['HELLO', 'HELL', 'JELLO'])

    def test_tapping_a_candidate_erases_exactly_the_typed_word_and_types_the_other(self):
        self.start()
        self.send(stroke(1, caps='first')); a = self.wait_got(1)[0]
        self.send({'v': 1, 'type': 'pick', 'id': 1000001, 'req': a['req'], 'cand': a['cands'][1]['id'], 'erase': 6})
        b = self.wait_got(2)[1]
        self.assertEqual(self.sent(), ['Hello ', '\b' * 6 + 'Hell '])
        self.assertEqual((b['kind'], b['committed'], b['typed']), ('pick', True, 'Hell '))
        # and the next replacement is measured against the NEW, shorter word
        self.send({'v': 1, 'type': 'pick', 'id': 1000002, 'req': a['req'], 'cand': a['cands'][2]['id'], 'erase': 5})
        self.wait_got(3)
        self.assertEqual(self.sent()[-1], '\b' * 5 + 'Jello ')
        self.send({'v': 1, 'type': 'pick', 'id': 1000003, 'req': a['req'], 'cand': a['cands'][0]['id'], 'erase': 5})   # the old length no longer fits
        self.wait_got(4)
        self.assertFalse(self.got()[3]['committed'])
        self.assertEqual(len(self.sent()), 3)

    def test_a_request_that_waited_too_long_is_dropped(self):
        """The user has moved on; typing a word seconds late would put it in the wrong place."""
        self.d.reply_delay = 0.25
        old = bridgemod.STALE_S
        bridgemod.STALE_S = 0.3
        self.addCleanup(setattr, bridgemod, 'STALE_S', old)
        h = self.start()
        self.send(stroke(1))
        time.sleep(0.05)
        self.send(stroke(2))                    # waits behind the first one (about 1 s of engine calls), longer than the limit
        wait_for(lambda: len(self.got()) >= 1, what='first answer')
        wait_for(lambda: h.bridge.stats['dropped'] >= 1, what='the late one dropped')
        self.assertEqual(len(self.d.ops('swipe')), 1)
        self.assertEqual(self.sent(), ['hello '])

    # ---- only what the engine issued is ever typed
    def test_a_candidate_the_engine_did_not_issue_is_rejected_and_nothing_is_typed(self):
        self.start()
        self.send(stroke(1)); a = self.wait_got(1)[0]
        n = len(self.sent())
        for bad in ('deadbeefdeadbeef', 'x' * 16, 'hello'):
            self.send({'v': 1, 'type': 'pick', 'id': 1000001, 'req': a['req'], 'cand': bad, 'erase': 6})
        wait_for(lambda: len(self.got()) >= 4, what='three refusals')
        self.assertEqual(len(self.sent()), n, 'nothing typed')
        self.assertTrue(all(g['committed'] is False for g in self.got()[1:]))
        self.assertEqual(len(self.d.ops('commit')), 1 + 3, 'each forged id was put to the engine, which refused it')

    def test_an_id_from_an_older_request_is_rejected(self):
        self.start()
        self.send(stroke(1)); a = self.wait_got(1)[0]
        self.send(stroke(2)); self.wait_got(2)
        n = len(self.sent())
        self.send({'v': 1, 'type': 'pick', 'id': 1000001, 'req': a['req'], 'cand': a['cands'][1]['id'], 'erase': 6})
        wait_for(lambda: len(self.got()) >= 3, what='refusal')
        self.assertEqual(len(self.sent()), n)
        self.assertFalse(self.got()[2]['committed'])
        self.assertEqual(len(self.d.ops('commit')), 2, 'refused by the helper itself: the engine was not even asked')

    def test_the_erase_count_must_equal_what_was_typed_last(self):
        self.start()
        self.send(stroke(1)); a = self.wait_got(1)[0]
        for erase in (1, 5, 7, 64):
            self.send({'v': 1, 'type': 'pick', 'id': 1000000 + erase, 'req': a['req'], 'cand': a['cands'][1]['id'], 'erase': erase})
        wait_for(lambda: len(self.got()) >= 5, what='four refusals')
        self.assertEqual(self.sent(), ['hello '])
        self.assertEqual(len(self.d.ops('commit')), 1, 'refused before the engine was even asked')
        self.assertEqual([g['error'] for g in self.got()[1:]], ['stale'] * 4)

    def test_a_pick_with_nothing_typed_yet_is_rejected(self):
        self.start()
        self.send({'v': 1, 'type': 'pick', 'id': 1, 'req': 1, 'cand': 'abc', 'erase': 6})
        wait_for(lambda: len(self.got()) >= 1, what='refusal')
        self.assertEqual(self.sent(), [])

    def test_words_from_the_engine_with_control_characters_are_never_typed(self):
        self.d.words = ['a\bb', 'fine', 'ok']      # a Backspace inside a word would delete text in the user's box
        self.start()
        self.send(stroke(1)); a = self.wait_got(1)[0]
        self.assertEqual(self.sent(), [], 'the whole answer is refused: the second choice is not typed instead')
        self.assertEqual(a['error'], 'bad_text')
        self.d.words = ['ok', 'x\rinjected', 'y']
        self.send(stroke(2)); self.wait_got(2)
        self.assertEqual(self.sent(), ['ok '])
        self.assertEqual([c['word'] for c in self.got()[1]['cands']], ['ok', 'y'], 'an unsafe candidate is not even shown')

    def test_text_the_engine_returns_from_commit_is_checked_again(self):
        self.d.raw_words = {'hello': 'hel\nlo'}
        self.start()
        self.send(stroke(1)); self.wait_got(1)
        self.assertEqual(self.sent(), [])
        self.assertEqual(self.got()[0]['error'], 'bad_text')

    # ---- every message from the page is distrusted
    def test_bad_messages_are_ignored_and_the_helper_stays_healthy(self):
        h = self.start()
        good = stroke(1)
        bad = [
            'not json', '[]', '{}', json.dumps(dict(good, v=2)), json.dumps(dict(good, type='evil')),
            json.dumps(dict(good, trace=[[0, 0, 0]])), json.dumps(dict(good, trace=[[0, 0, 0]] * 2001)),
            json.dumps(dict(good, trace=[[0, 0, 'x'], [1, 1, 1]])), json.dumps(dict(good, trace=[[1e9, 0, 0], [1, 1, 1]])),
            json.dumps(dict(good, keys=[])), json.dumps(dict(good, keys=[dict(good['keys'][0], w=-1)])),
            json.dumps(dict(good, keys=[dict(good['keys'][0], k='ab')])), json.dumps(dict(good, keys=[good['keys'][0], good['keys'][0]])),
            json.dumps(dict(good, ctx=['a b'])), json.dumps(dict(good, ctx=['x\n'])), json.dumps(dict(good, ctx=['a'] * 21)),
            json.dumps(dict(good, ctx=[5])), json.dumps(dict(good, mode='secret')), json.dumps(dict(good, caps='lower')),
            json.dumps(dict(good, layout='../../etc')), json.dumps(dict(good, id='x')),
            json.dumps({'v': 1, 'type': 'pick', 'id': 1, 'req': 1, 'cand': '../x', 'erase': 6}),
            json.dumps({'v': 1, 'type': 'pick', 'id': 1, 'req': 1, 'cand': 'abc', 'erase': 0}),
            json.dumps({'v': 1, 'type': 'pick', 'id': 1, 'req': 1, 'cand': 'abc', 'erase': 65}),
            'x' * (bridgemod.MAX_MESSAGE + 1),
        ]
        for raw in bad:
            self.send(raw, raw=True)
        self.send(good)
        self.wait_got(1)
        self.assertEqual(len(self.d.ops('swipe')), 1, 'only the one good message reached the engine')
        self.assertEqual(self.sent(), ['hello '])
        self.assertEqual(h.bridge.stats['rejected'], len(bad))
        self.assertEqual(self.r.status()['state'], 'active')

    def test_a_flood_of_swipes_is_cut_off(self):
        h = self.start()
        for i in range(40):
            self.send(stroke(i))
        wait_for(lambda: h.bridge.stats['rejected'] > 0, what='flood noticed')
        time.sleep(0.5)      # settle: let the accepted ones finish
        self.assertLess(len(self.d.ops('swipe')), 20)
        self.assertGreater(h.bridge.stats['rejected'], 15)

    def test_messages_from_one_page_do_not_use_another_pages_engine_connection(self):
        self.start()
        b2 = self.r.fake.add_target('Steam Big Picture Mode')
        wait_for(lambda: self.r.attr(b2) == LAYER_VERSION, what='second page')
        self.r.fake.eval(b2, 'window.__got = []; window.__swipekbLayer.session = {result: function (r) { window.__got.push(JSON.stringify(r)); }}; 1')
        self.send(stroke(1)); self.wait_got(1)
        self.r.fake.eval(b2, 'window.__swipekb(%s)' % json.dumps(json.dumps(stroke(5))))
        wait_for(lambda: self.r.fake.eval(b2, 'window.__got.length')[1] == 1, what='second page answered')
        self.assertEqual(self.d.connections, 2, 'one engine connection per keyboard page')

    # ---- the engine conversation
    def test_the_layout_is_sent_once_and_again_only_when_the_keys_move(self):
        self.start()
        self.send(stroke(1)); self.wait_got(1)
        self.send(stroke(2)); self.wait_got(2)
        self.assertEqual(len(self.d.ops('layout')), 1)
        moved = [dict(k, y=k['y'] - 60) for k in keys_msg()]
        self.send(stroke(3, keys=moved)); self.wait_got(3)
        self.assertEqual(len(self.d.ops('layout')), 2)

    def test_a_refused_layout_is_passed_on_and_nothing_is_typed(self):
        self.d.layout_error = 'unsupported'
        self.start()
        self.send(stroke(1, layout='russian')); a = self.wait_got(1)[0]
        self.assertEqual((a['committed'], a['error']), (False, 'unsupported'))
        self.assertEqual(self.sent(), [])
        self.assertEqual(self.d.ops('layout')[0][2]['steam_layout'], 'russian')

    def test_the_mode_decides_the_hello_and_a_change_makes_a_new_connection(self):
        self.start()
        self.send(stroke(1, mode='plain')); self.wait_got(1)
        self.send(stroke(2, mode='unknown')); self.wait_got(2)
        modes = [f['mode'] for _, _, f in self.d.ops('hello')]
        self.assertEqual(modes, ['plain', 'unknown'])
        wait_for(lambda: self.d.open_conns == 1, what='the old connection was closed')

    def test_keyboard_closed_hangs_up_on_the_engine(self):
        self.start()
        self.send(stroke(1)); self.wait_got(1)
        self.assertEqual(self.d.open_conns, 1)
        self.send({'v': 1, 'type': 'closed'})
        wait_for(lambda: self.d.open_conns == 0, what='engine connection closed')
        # a new keyboard session starts a fresh connection, and its first swipe has no memory of the old one
        self.send(stroke(2)); self.wait_got(2)
        self.assertEqual(self.d.connections, 2)
        self.assertEqual(len(self.d.ops('layout')), 2, 'layout sent again')

    def test_a_quiet_page_is_hung_up_on_by_the_helper_itself(self):
        """The page says nothing when a password box takes focus, so the helper must not keep the engine's memory of the last swipe alive."""
        old = bridgemod.IDLE_S
        bridgemod.IDLE_S = 0.4
        self.addCleanup(setattr, bridgemod, 'IDLE_S', old)
        self.start()
        self.send(stroke(1)); self.wait_got(1)
        self.assertEqual(self.d.open_conns, 1)
        wait_for(lambda: self.d.open_conns == 0, what='idle hang-up')

    def test_a_page_that_goes_away_takes_its_engine_connection_with_it(self):
        self.start()
        self.send(stroke(1)); self.wait_got(1)
        self.r.fake.remove_target(self.bp)
        wait_for(lambda: self.d.open_conns == 0, what='engine connection closed')

    # ---- failure: do nothing, stay quiet, stay alive
    def test_with_no_engine_the_swipe_does_nothing_and_the_page_is_told(self):
        self.d.stop()
        os.unlink(self.d.sock_path)
        self.start()
        self.send(stroke(1)); a = self.wait_got(1)[0]
        self.assertEqual((a['committed'], a['error']), (False, 'engine'))
        self.assertEqual(self.sent(), [])
        self.assertEqual(self.r.status()['state'], 'active')

    def test_a_wrong_token_means_no_typing(self):
        self.start()
        Path(self.d.token_path).write_text('0' * 32)
        self.send(stroke(1)); a = self.wait_got(1)[0]
        self.assertFalse(a['committed'])
        self.assertEqual(self.sent(), [])

    def test_the_engine_coming_back_works_on_the_next_swipe(self):
        h = self.start()
        self.send(stroke(1)); self.wait_got(1)
        self.d.stop()
        self.d = FakeDaemon(str(self.r.tmp), token='u' * 32)       # a restart: new socket, new token
        self.send(stroke(2)); a = self.wait_got(2)[1]
        self.assertFalse(a['committed'])
        self.send(stroke(3)); self.wait_got(3)
        self.assertTrue(self.got()[2]['committed'])

    def test_when_steams_text_call_is_unreachable_nothing_is_claimed_as_typed(self):
        self.start()
        self.r.fake.remove_target(self.sjs)
        self.send(stroke(1)); a = self.wait_got(1)[0]
        self.assertEqual((a['committed'], a['error']), (False, 'steam'))

    def test_a_slow_first_swipe_does_not_stop_the_layer_heartbeat(self):
        self.d.reply_delay = 0.4
        h = self.start(layer_watchdog=1.0)
        self.send(stroke(1))
        t0 = time.monotonic()
        while time.monotonic() - t0 < 2.0:        # the swipe takes ~1.6 s (4 calls); the page must stay installed throughout
            self.assertEqual(self.r.attr(self.bp), LAYER_VERSION)
            time.sleep(0.1)
        self.wait_got(1)

    # ---- nothing is stored
    def test_nothing_is_written_to_disk_while_typing_swiping_and_closing(self):
        """Plan Section 8: the helper writes no text anywhere. Proof two ways: Python's audit hook sees every file the process
        opens for writing, and a before/after listing with checksums of everything the helper could reach is identical."""
        self.start()
        self.send(stroke(1, ctx=['sentinelwordzebra'], mode='plain')); self.wait_got(1)          # warm: connection, layout
        before = snapshot(self.r.tmp)
        writes = []
        armed = threading.Event()
        armed.set()

        def hook(event, args):
            if not armed.is_set():
                return
            if event == 'open':
                path, mode, flags = args
                m = mode if isinstance(mode, str) else ''
                wr = any(c in m for c in 'wax+') or (isinstance(flags, int) and flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_APPEND | os.O_TRUNC))
                if wr and not str(path).startswith(('/dev/', '/proc/')) and not isinstance(path, int):
                    writes.append((event, str(path)))
            elif event in ('os.mkdir', 'os.rename', 'os.remove', 'os.rmdir', 'os.truncate', 'os.chmod', 'shutil.copyfile', 'os.symlink'):
                writes.append((event, str(args[0])))
        sys.addaudithook(hook)
        try:
            a = self.got()[0]
            for i in range(2, 8):
                self.send(stroke(i, ctx=['zebracornflux', 'quxword'], mode='unknown' if i % 2 else 'plain', caps='first'))
                self.wait_got(i)          # one at a time, at a human pace (the helper rate-limits floods)

            self.send({'v': 1, 'type': 'pick', 'id': 1000001, 'req': self.got()[-1]['req'], 'cand': self.got()[-1]['cands'][1]['id'], 'erase': self.got()[-1]['typed'].__len__()})
            self.wait_got(8)
            self.send({'v': 1, 'type': 'closed'})
            wait_for(lambda: self.d.open_conns == 0, what='closed')
            time.sleep(0.3)   # settle
        finally:
            armed.clear()
        self.assertEqual(writes, [])
        self.assertEqual(snapshot(self.r.tmp), before, 'identical file listing with checksums')
        self.assertEqual(self.r.status()['state'], 'active')

    def test_the_helper_source_has_no_logging_of_text(self):
        """No print, logging or file-writing call anywhere in the typing code (counts only)."""
        for name in ('bridge.py', 'engine.py'):
            text = (ROOT / 'gaming' / 'helper' / 'swipekb_helper' / name).read_text()
            for bad in ('print(', 'logging', 'open(', '.write(', 'write_text'):
                if name == 'engine.py' and bad == 'open(':
                    continue          # the token file is read, never written
                self.assertNotIn(bad, text, '%s must not contain %s' % (name, bad))


if __name__ == '__main__':
    unittest.main(verbosity=2)
