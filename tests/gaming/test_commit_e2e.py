#!/usr/bin/env python3
"""M5 end-to-end on the host: the REAL engine daemon, the REAL helper, a fake Steam (fake_cdp.py). Page messages are canned (recorded bench
swipes); everything between the binding and Steam's text call is the shipped code.

Environment (set by CTest, see gaming/daemon/CMakeLists.txt): SWIPEKB_GAMINGD, SWIPEKB_ASSETS, SWIPEKB_FUTO_LAYOUT (same as test_daemon.py).
Missing pieces are a hard failure, not a skip: a skipped privacy test would read as green.
"""
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'gaming' / 'helper'))

import test_daemon as td  # noqa: E402
from fake_cdp import wait_for  # noqa: E402
from swipekb_helper import LAYER_VERSION  # noqa: E402
from test_commit import snapshot  # noqa: E402
from test_helper import Rig  # noqa: E402


def page_keys():
    """The bench keyboard as the layer reports it: centre points."""
    return [{'k': k['c'], 'x': k['x'] + k['w'] / 2, 'y': k['y'] + k['h'] / 2, 'w': k['w'], 'h': k['h']} for k in td.keys_from_futo(td.LAYOUT)]


class EndToEnd(td.DaemonCase):
    def setUp(self):
        super().setUp()
        self.r = Rig()
        self.addCleanup(self.r.close)
        # The daemon may write only inside this sandbox: its home and every XDG folder point here, all empty to begin with.
        self.sandbox = Path(self.tmp) / 'home'
        for d in ('.', '.config', '.cache', '.local/share', '.local/state', 'data'):
            (self.sandbox / d).mkdir(parents=True, exist_ok=True)
        os.environ.update(HOME=str(self.sandbox), XDG_CONFIG_HOME=str(self.sandbox / '.config'), XDG_CACHE_HOME=str(self.sandbox / '.cache'),
                          XDG_DATA_HOME=str(self.sandbox / '.local/share'), XDG_STATE_HOME=str(self.sandbox / '.local/state'))
        self.addCleanup(lambda: [os.environ.pop(k, None) for k in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME', 'XDG_STATE_HOME')])

    def start_all(self, wrapper=None):
        self.start(wrapper=wrapper)
        self.bp, self.sjs = self.r.steam_up()
        h = self.r.helper(engine_socket=self.sock, engine_token=self.tokfile)
        h.start()
        wait_for(lambda: self.r.attr(self.bp) == LAYER_VERSION, what='layer installed')
        wait_for(lambda: self.r.status().get('state') == 'active', what='active')
        self.r.fake.eval(self.bp, 'window.__got = []; window.__swipekbLayer.session = {result: function (r) { window.__got.push(JSON.stringify(r)); }}; 1')
        self.n = 0
        return h

    def swipe_via_page(self, trace, ctx=(), mode='plain', caps='none'):
        time.sleep(0.2)          # a person cannot swipe faster than the helper's flood limit (6 per second); this paces the canned swipes
        self.n += 1
        msg = {'v': 1, 'type': 'stroke', 'id': self.n, 'trace': trace, 'vw': 1000, 'vh': 1000, 'keys': page_keys(), 'ctx': list(ctx), 'mode': mode,
               'layout': 'qwerty', 'caps': caps}
        self.r.fake.eval(self.bp, 'window.__swipekb(%s)' % json.dumps(json.dumps(msg)))
        wait_for(lambda: len(json.loads(self.r.fake.eval(self.bp, 'JSON.stringify(window.__got)')[1])) >= self.n, timeout=60, what='answer %d' % self.n)
        return json.loads(json.loads(self.r.fake.eval(self.bp, 'JSON.stringify(window.__got)')[1])[self.n - 1])

    def sent(self):
        return json.loads(self.r.fake.eval(self.sjs, 'JSON.stringify(__sent)')[1])

    def direct_top(self, trace, ctx=(), mode='unknown'):
        """What the engine itself says for the same input, asked over a separate connection."""
        c = self.connect(mode=mode)
        c.call('layout', keys=td.keys_from_futo(td.LAYOUT))
        r = c.call('swipe', points=trace, context=list(ctx), top_k=3)
        c.close()              # (the daemon allows only a few connections at once)
        return [x['word'] for x in r['candidates']]

    # ---- the pipeline
    def test_swipes_arrive_in_steams_text_call_exactly_as_the_engine_ranks_them(self):
        self.start_all()
        swipes = td.swipe_words(td.BENCH, 15)
        hits = 0
        for i, (word, pts, ctx) in enumerate(swipes):
            a = self.swipe_via_page(pts, ctx=ctx[-3:])
            self.assertTrue(a['committed'], a)
            want = self.direct_top(pts, ctx[-3:])
            self.assertEqual(a['typed'], want[0] + ' ', 'the typed word is the engine\'s own top candidate')
            self.assertEqual([c['word'] for c in a['cands']][:2], want[:2])
            hits += word == want[0]
        self.assertEqual(self.sent(), [self.direct_top(p, c[-3:])[0] + ' ' for _, p, c in swipes])
        self.assertGreaterEqual(hits, 8, 'sanity: the pipeline reads real swipes')

    def test_a_tapped_candidate_replaces_the_word_with_the_enginess_own_text(self):
        self.start_all()
        word, pts, ctx = td.swipe_words(td.BENCH, 1)[0]
        a = self.swipe_via_page(pts)
        other = a['cands'][1]
        self.n += 1
        self.r.fake.eval(self.bp, 'window.__swipekb(%s)' % json.dumps(json.dumps({'v': 1, 'type': 'pick', 'id': 1000001, 'req': a['req'], 'cand': other['id'], 'erase': len(a['typed'])})))
        wait_for(lambda: len(json.loads(self.r.fake.eval(self.bp, 'JSON.stringify(window.__got)')[1])) >= 2, what='pick answer')
        self.assertEqual(self.sent(), [a['typed'], '\b' * len(a['typed']) + other['word'] + ' '])

    def test_a_forged_candidate_id_gets_no_text_from_the_real_engine(self):
        self.start_all()
        word, pts, ctx = td.swipe_words(td.BENCH, 1)[0]
        a = self.swipe_via_page(pts)
        for forged in ('0' * 16, 'f' * 16, a['cands'][0]['id'][::-1]):
            self.r.fake.eval(self.bp, 'window.__swipekb(%s)' % json.dumps(json.dumps({'v': 1, 'type': 'pick', 'id': 1000001, 'req': a['req'], 'cand': forged, 'erase': len(a['typed'])})))
        wait_for(lambda: len(json.loads(self.r.fake.eval(self.bp, 'JSON.stringify(window.__got)')[1])) >= 4, what='refusals')
        self.assertEqual(self.sent(), [a['typed']])

    # ---- privacy
    def test_the_engine_holds_no_state_between_keyboard_sessions(self):
        """Plan M5: a second request without context returns context-free results, whatever came before."""
        self.start_all()
        swipes = td.swipe_words(td.BENCH, 6)
        baseline = [self.direct_top(p, ()) for _, p, _ in swipes]            # context-free answers on a fresh daemon
        first = [self.swipe_via_page(p, ctx=c[-4:], mode='unknown') for _, p, c in swipes]
        self.assertTrue(all(x['committed'] for x in first))
        self.r.fake.eval(self.bp, 'window.__swipekb(%s)' % json.dumps(json.dumps({'v': 1, 'type': 'closed'})))
        watcher = self.connect()
        wait_for(lambda: watcher.call('status')['connections'] == 1, what='the engine saw the keyboard session end')
        watcher.close()
        again = [self.swipe_via_page(p, ctx=(), mode='unknown') for _, p, _ in swipes]
        self.assertEqual([[c['word'] for c in x['cands']][:3] for x in again], [b[:3] for b in baseline],
                         'after the keyboard session ended, nothing of the earlier context is left in the engine')

    def test_unknown_mode_drops_a_secret_looking_context_instead_of_using_it(self):
        self.start_all()
        word, pts, ctx = td.swipe_words(td.BENCH, 1)[0]
        clean = self.swipe_via_page(pts, ctx=(), mode='unknown')
        with_secret = self.swipe_via_page(pts, ctx=['bcdfghjklmnpqrstvwxz'], mode='unknown')       # 16+ letters with no vowel: looks like a token, not language
        self.assertEqual([c['word'] for c in with_secret['cands']], [c['word'] for c in clean['cands']])

    def test_every_folder_the_apps_can_write_is_unchanged_by_typing_in_unknown_and_plain_mode(self):
        """Plan Sections 8 and 14: file listing with checksums before and after. The daemon's home and every XDG folder are inside the
        sandbox; the helper's own state folder and the Steam folder are in the rig; the engine's runtime folder holds only the socket and token."""
        self.start_all()
        swipes = td.swipe_words(td.BENCH, 6)
        self.swipe_via_page(swipes[0][1], ctx=['warm'], mode='unknown')       # models loaded, connection open
        watched = [self.sandbox, self.r.state, self.r.steam_dir, Path(self.tmp) / 'run']
        before = {str(d): snapshot(d) for d in watched}
        for i, (w, pts, ctx) in enumerate(swipes):
            self.swipe_via_page(pts, ctx=ctx[-3:], mode='unknown' if i % 2 else 'plain', caps='first')
        self.r.fake.eval(self.bp, 'window.__swipekb(%s)' % json.dumps(json.dumps({'v': 1, 'type': 'closed'})))
        self.assertEqual({str(d): snapshot(d) for d in watched}, before)
        run = sorted(p.name for p in (Path(self.tmp) / 'run').iterdir())
        self.assertEqual(run, ['engine.sock', 'engine.token'])

    def test_the_engine_works_with_the_whole_filesystem_read_only_except_its_socket_folder(self):
        """If the daemon ever needed to write anywhere else, this fails: the sandbox makes every other write an error."""
        run = Path(self.tmp) / 'run'
        run.mkdir(exist_ok=True)
        os.chmod(run, 0o700)
        wrapper = ['bwrap', '--ro-bind', '/', '/', '--dev-bind', '/dev', '/dev', '--proc', '/proc', '--bind', str(run), str(run), '--die-with-parent']
        self.start_all(wrapper=wrapper)
        word, pts, ctx = td.swipe_words(td.BENCH, 1)[0]
        a = self.swipe_via_page(pts, ctx=ctx[-3:], mode='unknown')
        self.assertTrue(a['committed'], a)
        self.assertEqual(self.sent(), [a['typed']])


if __name__ == '__main__':
    unittest.main(verbosity=2)
