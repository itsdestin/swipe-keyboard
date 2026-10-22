#!/usr/bin/env python3
"""Tests for the outside-door check (owner decision 2026-10-22, plan Section 9): the helper must keep the Gaming Mode
layer off while Steam's debug port is reachable from other devices. Run:  python3 -I tests/gaming/test_exposure.py

The check reads /proc/net/tcp and tcp6 only. These tests feed it made-up copies of those files, so they prove the
reading logic on any machine without opening a real port.
"""
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / 'gaming' / 'helper'))

from swipekb_helper import exposure  # noqa: E402
from fake_cdp import wait_for  # noqa: E402
import test_helper  # noqa: E402  (its Rig builds a fake Steam + a Helper)

HEAD4 = '  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n'
HEAD6 = '  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n'
V4_ANY, V4_LOOP, V4_LAN = '00000000', '0100007F', '0100A8C0'          # 0.0.0.0, 127.0.0.1, 192.168.0.1
V6_ANY = '00000000000000000000000000000000'
V6_LOOP = '00000000000000000000000001000000'                          # the IPv6 loopback address
V6_MAPPED_LOOP = '0000000000000000FFFF00000100007F'                   # IPv4 loopback seen through IPv6
V6_MAPPED_LAN = '0000000000000000FFFF00000100A8C0'


def row(addr, port, state='0A', n=0):
    return '   %d: %s:%04X %s:0000 %s 00000000:00000000 00:00000000 00000000  1000        0 12345 1 0000000000000000 100 0 0 10 0\n' % (
        n, addr, port, '0' * len(addr), state)


class Proc:
    def __init__(self, test, v4=(), v6=()):
        self.dir = Path(tempfile.mkdtemp(prefix='swipekb-proc-'))
        test.addCleanup(shutil.rmtree, self.dir, True)
        self.set(v4, v6)

    def set(self, v4=(), v6=()):
        (self.dir / 'tcp').write_text(HEAD4 + ''.join(row(a, p, s, i) for i, (a, p, s) in enumerate(v4)))
        (self.dir / 'tcp6').write_text(HEAD6 + ''.join(row(a, p, s, i) for i, (a, p, s) in enumerate(v6)))


class OpenDoors(unittest.TestCase):
    def doors(self, v4=(), v6=()):
        return exposure.open_doors(Proc(self, v4, v6).dir)

    def test_nothing_listening_is_closed(self):
        self.assertEqual(self.doors(), [])

    def test_loopback_only_is_closed(self):
        self.assertEqual(self.doors([(V4_LOOP, 8080, '0A'), (V4_LOOP, 8081, '0A')], [(V6_LOOP, 8081, '0A'), (V6_MAPPED_LOOP, 8080, '0A')]), [])

    def test_every_interface_on_8081_is_open_in_both_families(self):
        self.assertEqual(self.doors([(V4_ANY, 8081, '0A')]), [8081])
        self.assertEqual(self.doors(v6=[(V6_ANY, 8081, '0A')]), [8081])

    def test_a_lan_address_or_8080_on_any_interface_is_open(self):
        self.assertEqual(self.doors([(V4_LAN, 8081, '0A')]), [8081])
        self.assertEqual(self.doors([(V4_ANY, 8080, '0A')]), [8080])
        self.assertEqual(self.doors(v6=[(V6_MAPPED_LAN, 8081, '0A')]), [8081])

    def test_only_listening_sockets_count(self):
        # state 01 is an ordinary established connection (for example our own helper talking to Steam), not a door
        self.assertEqual(self.doors([(V4_ANY, 8081, '01'), (V4_ANY, 8080, '06')]), [])

    def test_other_ports_are_ignored(self):
        self.assertEqual(self.doors([(V4_ANY, 22, '0A'), (V4_ANY, 18081, '0A')]), [])

    def test_both_ports_reported_sorted_once(self):
        self.assertEqual(self.doors([(V4_ANY, 8081, '0A'), (V4_ANY, 8080, '0A')], [(V6_ANY, 8081, '0A')]), [8080, 8081])

    def test_unreadable_files_do_not_raise(self):
        # WHY: a helper that crashes on an odd /proc would leave no status at all; unreadable means "cannot tell".
        self.assertEqual(exposure.open_doors(Path('/nonexistent-swipekb')), [])

    def test_garbage_lines_are_skipped(self):
        p = Proc(self)
        (p.dir / 'tcp').write_text(HEAD4 + 'nonsense\n\n   0: zz:zz\n' + row(V4_ANY, 8081))
        self.assertEqual(exposure.open_doors(p.dir), [8081])


class HelperKeepsLayerOff(test_helper.Base):
    def exposed_rig(self, proc):
        r = self.rig()
        b, s = r.steam_up()
        h = test_helper.Helper(r.config(), steam_running=lambda: r.running,
                               exposure_check=lambda: exposure.open_doors(proc.dir), exposure_interval=0.05)
        r.helpers.append(h)
        return r, b, h

    def test_open_door_means_no_layer_and_a_specific_reason(self):
        proc = Proc(self, v4=[(V4_ANY, 8081, '0A')])
        r, b, h = self.exposed_rig(proc)
        h.start()
        wait_for(lambda: r.status().get('state') == 'blocked-outside-access', what='blocked status')
        st = r.status()
        self.assertIn('8081', st['reason'])
        self.assertIn('installer', st['reason'])
        self.assertIsNone(r.attr(b))               # nothing was put into Steam's page
        self.assertEqual(st['attached_pages'], 0)

    def test_closing_the_door_lets_the_layer_in_and_reopening_takes_it_out(self):
        proc = Proc(self, v4=[(V4_ANY, 8081, '0A')])
        r, b, h = self.exposed_rig(proc)
        h.start()
        wait_for(lambda: r.status().get('state') == 'blocked-outside-access', what='blocked')
        proc.set()
        wait_for(lambda: r.attr(b), what='layer installed once the door closed')
        wait_for(lambda: r.status().get('state') == 'active', what='active')
        proc.set(v4=[(V4_ANY, 8081, '0A')])
        wait_for(lambda: r.attr(b) is None, what='layer removed when the door reopened')
        wait_for(lambda: r.status().get('state') == 'blocked-outside-access', what='blocked again')

    def test_closed_door_changes_nothing_for_a_normal_run(self):
        proc = Proc(self)
        r, b, h = self.exposed_rig(proc)
        h.start()
        wait_for(lambda: r.status().get('state') == 'active', what='active')

    def choice_rig(self, proc, choice):
        r = self.rig()
        b, s = r.steam_up()
        h = test_helper.Helper(r.config(), steam_running=lambda: r.running,
                               exposure_check=lambda: exposure.open_doors(proc.dir), exposure_interval=0.05,
                               exposure_choice=lambda: choice[0])
        r.helpers.append(h)
        return r, b, h

    def test_left_open_by_choice_the_layer_runs_and_the_status_says_so(self):
        proc = Proc(self, v4=[(V4_ANY, 8081, '0A')])
        r, b, h = self.choice_rig(proc, ['open'])
        h.start()
        wait_for(lambda: r.status().get('state') == 'active', what='active despite open door')
        self.assertTrue(r.attr(b))
        self.assertIn('left open by your choice', r.status()['reason'])

    def test_changing_the_choice_back_to_blocked_takes_the_layer_out(self):
        proc = Proc(self, v4=[(V4_ANY, 8081, '0A')])
        choice = ['open']
        r, b, h = self.choice_rig(proc, choice)
        h.start()
        wait_for(lambda: r.status().get('state') == 'active', what='active')
        choice[0] = 'blocked'
        wait_for(lambda: r.status().get('state') == 'blocked-outside-access', what='blocked after the choice changed')
        wait_for(lambda: r.attr(b) is None, what='layer removed')

    def test_read_choice_defaults_to_the_strict_reading(self):
        d = Path(tempfile.mkdtemp(prefix='swipekb-choice-'))
        self.addCleanup(shutil.rmtree, d, True)
        self.assertEqual(exposure.read_choice(d), 'blocked')          # no file
        (d / 'outside-access').write_text('open\n')
        self.assertEqual(exposure.read_choice(d), 'open')
        (d / 'outside-access').write_text('whatever')
        self.assertEqual(exposure.read_choice(d), 'blocked')          # anything but exactly "open" keeps the door guarded

    def test_the_main_program_turns_the_check_on(self):
        # WHY a source check: tests build Helper without a check (so a developer machine's open ports cannot break them),
        # which means only __main__ guarantees the installed helper really performs it.
        text = (HERE.parents[1] / 'gaming/helper/swipekb_helper/__main__.py').read_text()
        self.assertIn('exposure_check=exposure.open_doors', text)
        self.assertIn('exposure_choice=', text)


if __name__ == '__main__':
    unittest.main(verbosity=2)
