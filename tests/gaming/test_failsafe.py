#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Failure injection for the helper (plan Section 14, M9): every way Steam can stop matching, and what the helper does.

Synthetic Steam only (fake_cdp.py); no Valve code. Each test ends on the same three facts: the layer is OUT of the page,
the status file says WHY in specific words, and the helper is not looping (its page connections stop growing).
The page-side half (the keyboard model, stock typing intact) is tests/gaming/test_failsafe.js.
Run:  python3 -I tests/gaming/test_failsafe.py
"""
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / 'gaming' / 'helper'))

from fake_cdp import wait_for  # noqa: E402
from swipekb_helper import LAYER_VERSION  # noqa: E402
from swipekb_helper.bridge import SendFailed  # noqa: E402
from swipekb_helper.retry import RetryLedger  # noqa: E402
from test_helper import BP, SJS, Base  # noqa: E402

REMOVE_METHOD = 'delete SteamClient.Input.ControllerKeyboardSendText'


def failures(r):
    try:
        return json.loads((r.state / 'retry.json').read_text())['failures']
    except (OSError, ValueError, KeyError):
        return 0


def quiet(test, r, seconds=0.6):
    """No new connection to any Steam page for `seconds`: the helper has stopped trying. -> connections at the end."""
    before = r.fake.page_sessions
    time.sleep(seconds)  # settle: a negative can only be shown by waiting
    test.assertEqual(r.fake.page_sessions, before, 'the helper kept opening page connections')
    return before


class RenamedPage(Base):
    def test_a_renamed_keyboard_page_is_named_in_the_status_and_nothing_is_installed(self):
        r = self.rig(not_ready_grace=0.2)
        r.fake.add_target(SJS, steam='ok')
        b = r.fake.add_target('Steam Big Picture')
        r.helper().start()
        wait_for(lambda: BP in r.status().get('reason', ''), what='the status naming the missing page')
        st = r.status()
        self.assertEqual(st['state'], 'waiting-for-steam')
        self.assertIn('renamed', st['reason'])
        self.assertIsNone(r.layer(b))
        # Desktop Mode looks the same from here, so it is a note and never a counted failure.
        self.assertEqual(failures(r), 0)

    def test_a_title_change_while_installed_takes_the_layer_out_and_says_why(self):
        r = self.rig(not_ready_grace=0.2)
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install')
        r.fake.set_title(b, 'Library')
        wait_for(lambda: r.attr(b) is None and r.layer(b) is None, what='the layer to come out')
        wait_for(lambda: BP in r.status().get('reason', ''), what='the reason')
        self.assertEqual(r.status()['attached_pages'], 0)


class RenamedMethod(Base):
    def test_text_call_renamed_while_installed_takes_the_layer_out_and_pauses_for_good(self):
        r = self.rig(recheck_interval=0.15)
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install')
        r.fake.eval(s, REMOVE_METHOD)
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')
        self.assertIsNone(r.layer(b), 'the layer is still in the page after the pause')
        self.assertIn('ControllerKeyboardSendText', r.status()['reason'])
        self.assertEqual(failures(r), 3)
        quiet(self, r)

    def test_a_typing_call_that_fails_does_not_wait_for_the_next_check(self):
        # The periodic check is an hour away: only the failed call itself can have raised the alarm.
        r = self.rig(recheck_interval=3600)
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install')
        wait_for(lambda: h.targets and h.pages, what='attached')
        r.fake.eval(s, REMOVE_METHOD)
        with self.assertRaises(SendFailed):
            h._send_text('hello ')
        wait_for(lambda: r.layer(b) is None, timeout=5, what='the layer to come out at once')

    def test_a_call_that_fails_because_the_link_dropped_is_not_taken_as_a_change(self):
        # Steam's page going away is a restart, not a rename: no recheck, nothing counted.
        r = self.rig(recheck_interval=3600)
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install')
        wait_for(lambda: h.pages, what='attached')
        r.fake.remove_target(s)
        with self.assertRaises(SendFailed):
            h._send_text('hello ')
        self.assertFalse(h._recheck_now)
        self.assertEqual(failures(r), 0)


class PortClosed(Base):
    def test_a_closed_port_leaves_the_layer_to_remove_itself_and_the_status_says_so(self):
        r = self.rig(layer_watchdog=0.5)
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'active', what='active')
        r.fake.close_port()
        wait_for(lambda: r.status().get('state') == 'waiting-for-steam', what='waiting')
        self.assertIn('Steam', r.status()['reason'])
        wait_for(lambda: r.attr(b) is None, what='the page-side watchdog to remove the layer')
        self.assertEqual(failures(r), 0)       # a closed port is not a Steam change
        r.fake.open()                           # Steam comes back on the same port
        wait_for(lambda: r.status().get('state') == 'active' and r.attr(b) == LAYER_VERSION, what='back to active')


class BlockedScripts(Base):
    """A page that refuses to run what the helper sends (content-security policy, blocked evaluation)."""

    def _paused_clean(self, r, b):
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')
        self.assertIn('refused', r.status()['reason'])
        self.assertEqual(failures(r), 3)
        page = r.fake.page(b)
        page.eval_filter = None
        page.refuse = set()
        self.assertIsNone(r.layer(b), 'a layer is in the page')
        self.assertEqual(sum(len(v) for v in page.scripts.values()), 0, 'a start-of-page registration was left behind')
        quiet(self, r)

    def test_evaluation_blocked_from_the_start(self):
        r = self.rig()
        b, s = r.steam_up()
        r.fake.page(b).eval_filter = lambda e: 'Refused to evaluate a string as JavaScript (policy)'
        r.helper().start()
        self._paused_clean(r, b)

    def test_registration_for_new_pages_refused(self):
        r = self.rig()
        b, s = r.steam_up()
        r.fake.page(b).refuse = {'Page.addScriptToEvaluateOnNewDocument'}
        r.helper().start()
        self._paused_clean(r, b)

    def test_evaluation_blocked_after_the_layer_is_in(self):
        r = self.rig(layer_watchdog=0.6)
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install')
        # Only the once-a-second ping is refused: the install itself works every time, so the count can only come from the ping.
        r.fake.page(b).eval_filter = lambda e: 'Refused to evaluate a string as JavaScript (policy)' if 'L.beat()' in e else None
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')
        self.assertIn('refused', r.status()['reason'])
        r.fake.page(b).eval_filter = None
        wait_for(lambda: r.attr(b) is None, what='the layer to be out')
        quiet(self, r)


class NoLoops(Base):
    def test_giving_up_takes_the_layer_out_of_pages_it_is_still_in(self):
        # Whatever pushed the count to the limit, the pause itself must empty the pages.
        r = self.rig()
        b, s = r.steam_up()
        h = r.helper()
        h.start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION and h.pages, what='install')
        h.ledger.failures = h.ledger.max
        wait_for(lambda: r.layer(b) is None, what='the layer to come out')
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')


    def test_a_page_that_keeps_removing_the_layer_ends_in_a_pause(self):
        r = self.rig(layer_watchdog=0.6)
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install')
        r.fake.eval(b, 'setInterval(function(){ delete window.__swipekbLayer; }, 5)')
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', timeout=20, what='pause')
        self.assertIn('keeps disappearing', r.status()['reason'])
        installs = sum(1 for e in r.fake.page(b).eval_log if len(e) > 2000)
        self.assertLessEqual(installs, 16, 'the helper put the layer back %d times' % installs)
        quiet(self, r)
        self.assertEqual(sum(1 for e in r.fake.page(b).eval_log if len(e) > 2000), installs)

    def test_tries_slow_down_and_stop(self):
        led = RetryLedger(Path(tempfile.mkdtemp(prefix='swipekb-led-')) / 'retry.json', 3)
        waits = []
        for _ in range(3):
            led.record_failure('x', led.failures * 100.0, 2.0)
            waits.append(led.backoff(2.0, 60.0))
        self.assertEqual(waits, [2.0, 4.0, 8.0])
        self.assertEqual(led.backoff(2.0, 5.0), 5.0)
        self.assertTrue(led.exhausted)

    def test_one_failure_is_forgiven_after_the_layer_has_stayed_and_answered(self):
        r = self.rig(healthy_after=0.4)
        b, s = r.steam_up()
        r.fake.page(b).refuse = {'Page.addScriptToEvaluateOnNewDocument'}
        r.helper().start()
        wait_for(lambda: failures(r) >= 1, what='the first failure')
        r.fake.page(b).refuse = set()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install on the retry')
        wait_for(lambda: failures(r) == 0, what='the count to be forgiven')

    def test_a_failure_is_not_forgiven_just_because_the_install_worked(self):
        # An install that succeeds and is dropped a moment later must not reset the count (that is the loop).
        r = self.rig(healthy_after=3600)
        b, s = r.steam_up()
        r.fake.page(b).refuse = {'Page.addScriptToEvaluateOnNewDocument'}
        h = r.helper()
        h.start()
        wait_for(lambda: failures(r) >= 1, what='the first failure')
        r.fake.page(b).refuse = set()
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='install')
        time.sleep(0.4)  # settle: the count must still be there
        self.assertGreaterEqual(failures(r), 1)

    def test_a_new_steam_ui_build_without_a_port_close_gets_fresh_tries(self):
        probe = Path(tempfile.mkdtemp(prefix='swipekb-probe-')) / 'sp.js'
        probe.write_text('x')
        os.utime(probe, (1000, 1000))
        r = self.rig(steamui_probe=str(probe))
        b, s = r.steam_up(sjs='nomethod')
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')
        # Valve ships a UI build that has the call again; the port never closed.
        r.fake.eval(s, 'SteamClient.Input.ControllerKeyboardSendText = function(){}')
        os.utime(probe, (2000, 2000))
        wait_for(lambda: r.attr(b) == LAYER_VERSION, what='tries after the new build')
        wait_for(lambda: r.status().get('state') == 'active', what='active')


class LayerReports(Base):
    """What the page layer says about itself reaches the status file."""

    def _active(self, **kw):
        r = self.rig(**kw)
        b, s = r.steam_up()
        r.helper().start()
        wait_for(lambda: r.status().get('state') == 'active' and r.attr(b) == LAYER_VERSION, what='active')
        return r, b

    def test_a_layer_that_stopped_itself_is_removed_and_not_put_back_on_that_page(self):
        r, b = self._active()
        r.fake.eval(b, 'window.__swipekbLayer.gestures = {state: function(){return "failed"}, reason: function(){return "boom"},'
                       ' off: function(){}}')
        wait_for(lambda: r.status().get('state') == 'error', what='error status')
        self.assertIn('boom', r.status()['reason'])
        self.assertIn('Stock keyboard', r.status()['reason'])
        self.assertIsNone(r.layer(b))
        n = sum(1 for e in r.fake.page(b).eval_log if len(e) > 2000)
        time.sleep(0.6)  # settle: it must stay out
        self.assertEqual(sum(1 for e in r.fake.page(b).eval_log if len(e) > 2000), n)
        self.assertIsNone(r.layer(b))

    def test_a_layer_part_that_cannot_report_does_not_get_the_layer_taken_out(self):
        # A ping that throws is read as "Steam refused"; a part in an odd state must come back as "unknown" instead.
        r, b = self._active()
        r.fake.eval(b, 'window.__swipekbLayer.session = {}; window.__swipekbLayer.gestures = {state: function(){ throw new Error("odd"); }}')
        time.sleep(0.6)  # settle: several pings must pass and change nothing
        self.assertEqual(r.attr(b), LAYER_VERSION)
        self.assertEqual(r.status()['state'], 'active')
        self.assertEqual(failures(r), 0)

    def test_a_counted_keyboard_oddity_is_a_note_and_changes_nothing(self):
        r, b = self._active()
        issue = 'the keyboard did not show 24 letter keys on its last 3 openings'
        r.fake.eval(b, 'window.__swipekbLayer.report = function(){return JSON.stringify({gestures:"on",session:"on",'
                       'keyboard:{issue:%s,opens:4,short_run:3}})}' % json.dumps(issue))
        wait_for(lambda: issue in r.status().get('reason', ''), what='the note')
        st = r.status()
        self.assertEqual(st['state'], 'active')
        self.assertEqual(st['keyboard']['short_run'], 3)
        self.assertEqual(r.attr(b), LAYER_VERSION)

    def test_a_keyboard_the_layer_switched_itself_off_for_is_a_pause_with_the_reason(self):
        r, b = self._active()
        issue = 'Steam keys no longer carry data-key-row'
        r.fake.eval(b, 'window.__swipekbLayer.report = function(){return JSON.stringify({gestures:"off",session:"off",'
                       'keyboard:{issue:%s}})}' % json.dumps(issue))
        wait_for(lambda: r.status().get('state') == 'paused-steam-changed', what='pause')
        self.assertIn(issue, r.status()['reason'])
        self.assertIn('Stock keyboard', r.status()['reason'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
