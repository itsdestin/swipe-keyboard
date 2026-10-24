# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""The whole canary against a fake Valve server: good runs, negative controls, hostile zips, dead canary."""
import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from datetime import datetime, timedelta, timezone
from pathlib import Path

from support import BASE, ROOT, FakeServer, add_channel, native_zip, ui_zip
import canary
import fetch
import ledger as ledger_mod

CONTRACT = ROOT / 'gaming/contract/gaming-contract.json'
NOW = datetime(2026, 10, 22, 6, 0, tzinfo=timezone.utc)
CHANNELS = {'steam_client_ubuntu12': 'desktop-stable', 'steam_client_publicbeta_ubuntu12': 'desktop-beta',
            'steam_client_steamdeck_stable_ubuntu12': 'deck-stable',
            'steam_client_steamdeck_publicbeta_ubuntu12': 'deck-beta'}


def sp_edit(fn):
    def mutate(files):
        k = 'steamui/sp.js'
        files[k] = fn(files[k].decode()).encode()
    return mutate


class CanaryRun(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(self.dir, ignore_errors=True))
        self.ledger_path = self.dir / 'ledger.json'
        self.work = self.dir / 'work'
        self.work.mkdir()
        self.files = {}
        good_ui, good_native = ui_zip(), native_zip()
        # WHY a different (harmless) file in beta: a real beta is a different build, so a different zip.
        beta_ui = ui_zip(extra=[('steamui/beta-only.txt', b'beta')])
        add_channel(self.files, 'steam_client_ubuntu12', '1792618285', good_ui, good_native)
        add_channel(self.files, 'steam_client_steamdeck_stable_ubuntu12', '1792618285', good_ui, good_native)
        add_channel(self.files, 'steam_client_publicbeta_ubuntu12', '1792108788', beta_ui, good_native)
        add_channel(self.files, 'steam_client_steamdeck_publicbeta_ubuntu12', '1792108788', beta_ui, good_native)

    def run_canary(self, now=NOW, **kw):
        self.server = FakeServer(self.files)
        f = fetch.Fetcher(BASE, opener=self.server, sleep=lambda s: None)
        return canary.run_canary(CONTRACT, self.ledger_path, self.work, now, f, **kw)

    def status(self, summary):
        return {o.channel: o.status for o in summary.outcomes}

    def test_all_four_channels_pass_and_shared_packages_download_once(self):
        s = self.run_canary()
        self.assertEqual(self.status(s), {c: 'pass' for c in CHANNELS.values()})
        self.assertFalse(s.failed)
        zips = [u for u in self.server.calls if '.zip.' in u]
        self.assertEqual(len(zips), 3, 'stable UI, beta UI, shared native: not 8 downloads')
        self.assertEqual(len([u for u in self.server.calls if u.rsplit('/', 1)[1] in CHANNELS]), 4)

    def test_ledger_is_written_with_every_channel_and_heartbeat(self):
        self.run_canary()
        l = ledger_mod.Ledger.load(self.ledger_path)
        self.assertEqual(sorted(l.data['channels']), sorted(CHANNELS.values()))
        self.assertEqual(l.data['channels']['deck-stable']['build'], '1792618285')
        self.assertEqual(l.last_run, NOW)

    def test_second_run_with_same_builds_downloads_no_packages_but_still_beats(self):
        self.run_canary()
        later = NOW + timedelta(days=1)
        s = self.run_canary(now=later)
        self.assertEqual(set(self.status(s).values()), {'unchanged'})
        self.assertEqual([u for u in self.server.calls if '.zip.' in u], [])
        self.assertEqual(ledger_mod.Ledger.load(self.ledger_path).last_run, later)

    def test_negative_control_anchor_removed_turns_one_channel_red(self):
        bad_ui = ui_zip(sp_edit(lambda t: t.replace('"data-extended-chars"', '"data-xtra"')))
        add_channel(self.files, 'steam_client_publicbeta_ubuntu12', '1799999999', bad_ui, native_zip())
        s = self.run_canary()
        self.assertTrue(s.failed)
        self.assertEqual(self.status(s)['desktop-beta'], 'fail')
        self.assertEqual(self.status(s)['deck-stable'], 'pass')
        md = s.markdown()
        self.assertIn('key-extended-chars', md)
        self.assertIn('1799999999', md)
        self.assertIn('desktop-beta', md)
        self.assertIn('devices fail safe to stock', md)

    def test_negative_control_method_renamed_turns_red(self):
        bad = ui_zip(sp_edit(lambda t: t.replace('ControllerKeyboardSendText', 'ControllerKeyboardSendString')))
        for m in CHANNELS:
            add_channel(self.files, m, '1800000000', bad, native_zip())
        s = self.run_canary()
        self.assertEqual(set(self.status(s).values()), {'fail'})
        self.assertIn('send-text-call', s.markdown())

    def test_red_channel_is_rechecked_next_run_until_fixed(self):
        bad = ui_zip(sp_edit(lambda t: t.replace('ControllerKeyboardSendText', 'X')))
        add_channel(self.files, 'steam_client_ubuntu12', '1800000000', bad, native_zip())
        self.run_canary()
        s = self.run_canary(now=NOW + timedelta(days=1))
        self.assertEqual(self.status(s)['desktop-stable'], 'fail')
        self.assertEqual(self.status(s)['deck-beta'], 'unchanged')

    def test_malicious_zip_entry_is_refused_and_writes_nothing_outside(self):
        evil = ui_zip(extra=[('steamui/../../../outside.txt', b'pwn')])
        add_channel(self.files, 'steam_client_ubuntu12', '1800000001', evil, native_zip())
        s = self.run_canary()
        self.assertEqual(self.status(s)['desktop-stable'], 'unsafe')
        self.assertTrue(s.failed)
        self.assertFalse((self.dir / 'outside.txt').exists())
        self.assertFalse((self.work.parent / 'outside.txt').exists())
        self.assertIn('unsafe', s.markdown().lower())

    def test_backslash_traversal_zip_is_refused_too(self):
        evil = ui_zip(extra=[('steamui\\..\\..\\outside.txt', b'pwn')])
        add_channel(self.files, 'steam_client_ubuntu12', '1800000002', evil, native_zip())
        self.assertEqual(self.status(self.run_canary())['desktop-stable'], 'unsafe')

    def test_backslash_directory_names_from_real_valve_zips_are_handled(self):
        # ui_zip() already stores 'steamui\\css/' style entries; the good run above proves they extract.
        self.assertEqual(self.status(self.run_canary())['deck-stable'], 'pass')

    def test_hash_mismatch_is_an_error_not_a_pass(self):
        for name in [k for k in self.files if k.startswith('steamui_websrc_all.zip.')]:
            self.files[name] = self.files[name] + b'tampered'
        s = self.run_canary()
        self.assertTrue(s.failed)
        self.assertEqual(set(self.status(s).values()), {'error'})

    def test_one_dead_manifest_does_not_stop_the_other_channels(self):
        del self.files['steam_client_publicbeta_ubuntu12']
        s = self.run_canary()
        self.assertEqual(self.status(s)['desktop-beta'], 'error')
        self.assertEqual(self.status(s)['desktop-stable'], 'pass')
        self.assertTrue(s.failed)

    def test_manifest_without_the_native_package_is_an_error(self):
        from support import vdf
        self.files['steam_client_ubuntu12'] = vdf('1800000003', {}).encode()
        self.assertEqual(self.status(self.run_canary())['desktop-stable'], 'error')

    def test_keyboard_region_drift_raises_review_without_failing(self):
        self.run_canary()
        drift = ui_zip(sp_edit(lambda t: t.replace('break;case"ArrowUp":', 'break;case"ArrowUp" :')))
        add_channel(self.files, 'steam_client_steamdeck_stable_ubuntu12', '1800000004', drift, native_zip())
        s = self.run_canary(now=NOW + timedelta(days=1))
        self.assertEqual(self.status(s)['deck-stable'], 'review')
        self.assertFalse(s.failed)
        self.assertIn('keyboard', s.markdown().lower())

    def test_workdir_is_empty_afterwards(self):
        self.run_canary()
        self.assertEqual(list(self.work.iterdir()), [], 'extracted Valve files must not be left behind')

    def test_channel_filter(self):
        s = self.run_canary(only_channels=['deck-beta'])
        self.assertEqual(list(self.status(s)), ['deck-beta'])

    def test_unknown_channel_filter_is_an_error(self):
        with self.assertRaises(ValueError):
            self.run_canary(only_channels=['nope'])


class CanaryCli(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(self.dir, ignore_errors=True))
        self.path = self.dir / 'ledger.json'

    def cli(self, *args):
        return subprocess.run([sys.executable, '-I', str(ROOT / 'tools/steam-canary/canary.py'), *args],
                              capture_output=True, text=True)

    def test_dead_check_by_injected_date(self):
        l = ledger_mod.Ledger.load(self.path)
        l.touch(NOW)
        l.save(self.path)
        alive = self.cli('dead-check', '--ledger', str(self.path), '--now', '2026-10-24T06:00:00Z')
        self.assertEqual(alive.returncode, 0, alive.stderr)
        dead = self.cli('dead-check', '--ledger', str(self.path), '--now', '2026-10-26T06:01:00Z')
        self.assertEqual(dead.returncode, 1)
        self.assertIn('has not run', dead.stdout)

    def test_dead_check_with_no_ledger_is_dead(self):
        self.assertEqual(self.cli('dead-check', '--ledger', str(self.path), '--now', '2026-10-26T00:00:00Z').returncode, 1)

    def test_bad_date_is_a_usage_error_not_a_pass(self):
        self.assertEqual(self.cli('dead-check', '--ledger', str(self.path), '--now', 'yesterday').returncode, 2)

    def test_corrupt_ledger_is_exit_3_with_a_message_not_a_traceback_or_a_false_dead_alarm(self):
        self.path.write_text('{ broken')
        r = self.cli('dead-check', '--ledger', str(self.path), '--now', '2026-10-26T00:00:00Z')
        self.assertEqual(r.returncode, 3)
        self.assertNotIn('Traceback', r.stderr)
        self.assertIn('ledger', r.stderr.lower())

    def test_accept_drift_moves_the_baseline(self):
        l = ledger_mod.Ledger.load(self.path)
        l.record('deck-stable', '1', 'pass', 'c', 'r1', NOW)
        l.record('deck-stable', '2', 'review', 'c', 'r2', NOW)
        l.save(self.path)
        self.assertEqual(self.cli('accept-drift', '--ledger', str(self.path), '--channel', 'deck-stable').returncode, 0)
        self.assertFalse(ledger_mod.Ledger.load(self.path).region_drifted('deck-stable', 'r2'))

    def test_main_run_writes_ledger_summary_and_exit_code(self):
        files = {}
        add_channel(files, 'steam_client_ubuntu12', '1792618285', ui_zip(), native_zip())
        for m in list(CHANNELS)[1:]:
            add_channel(files, m, '1792618285', ui_zip(), native_zip())
        server = FakeServer(files)
        out = io.StringIO()
        with redirect_stdout(out):
            code = canary.main(['run', '--contract', str(CONTRACT), '--ledger', str(self.path),
                                '--work-dir', str(self.dir), '--summary-md', str(self.dir / 's.md'),
                                '--now', '2026-10-22T06:00:00Z'],
                               fetcher_factory=lambda base: fetch.Fetcher(base, opener=server, sleep=lambda s: None))
        self.assertEqual(code, 0)
        self.assertIn('desktop-stable', (self.dir / 's.md').read_text())
        self.assertEqual(len(json.loads(self.path.read_text())['channels']), 4)

    def test_a_download_that_is_not_a_zip_is_a_broken_canary_exit_3_not_a_traceback(self):
        """Review fix 8: a file that matches its checksum but cannot be opened must not crash the run with exit 1 (= "Steam changed")."""
        files = {}
        for m in CHANNELS:
            add_channel(files, m, '1792618285', b'this is not a zip file', native_zip())
        server = FakeServer(files)
        err = io.StringIO()
        from contextlib import redirect_stderr
        with redirect_stdout(io.StringIO()), redirect_stderr(err):
            code = canary.cli(['run', '--contract', str(CONTRACT), '--ledger', str(self.path),
                                         '--work-dir', str(self.dir), '--now', '2026-10-22T06:00:00Z'],
                                        fetcher_factory=lambda base: fetch.Fetcher(base, opener=server, sleep=lambda s: None))
        self.assertEqual(code, 3)
        self.assertNotIn('Traceback', err.getvalue())
        self.assertIn('zip', err.getvalue().lower())


if __name__ == '__main__':
    unittest.main()
