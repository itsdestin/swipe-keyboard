# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""The ledger is the canary's memory (build per channel, last run) and feeds the 'canary dead' check."""
import json
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from support import ROOT
import ledger

NOW = datetime(2026, 10, 22, 12, 0, tzinfo=timezone.utc)


class Ledger(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(self.dir, ignore_errors=True))
        self.path = self.dir / 'ledger.json'

    def test_missing_file_is_an_empty_ledger(self):
        l = ledger.Ledger.load(self.path)
        self.assertIsNone(l.last_run)
        self.assertTrue(l.needs_check('deck-stable', '1', 'chash'))

    def test_corrupt_file_is_an_error_not_a_silent_reset(self):
        self.path.write_text('{ not json')
        with self.assertRaises(ledger.LedgerError):
            ledger.Ledger.load(self.path)
        self.path.write_text(json.dumps({'schema': 99}))
        with self.assertRaises(ledger.LedgerError):
            ledger.Ledger.load(self.path)

    def test_record_save_load_roundtrip_has_the_fields_the_plan_names(self):
        l = ledger.Ledger.load(self.path)
        l.record('deck-stable', '111', 'pass', 'chash', 'rhash', NOW)
        l.touch(NOW)
        l.save(self.path)
        again = ledger.Ledger.load(self.path)
        entry = again.data['channels']['deck-stable']
        for key in ('build', 'result', 'checked_at', 'contract_hash', 'region_hash'):
            self.assertIn(key, entry)
        self.assertEqual(again.last_run, NOW)
        self.assertEqual(again.data['history'][-1]['build'], '111')

    def test_needs_check_only_for_new_build_new_contract_or_unhealthy_result(self):
        l = ledger.Ledger.load(self.path)
        l.record('deck-stable', '111', 'pass', 'c1', 'r1', NOW)
        self.assertFalse(l.needs_check('deck-stable', '111', 'c1'))
        self.assertTrue(l.needs_check('deck-stable', '112', 'c1'), 'new build')
        self.assertTrue(l.needs_check('deck-stable', '111', 'c2'), 'contract changed')
        l.record('deck-stable', '111', 'fail', 'c1', 'r1', NOW)
        self.assertTrue(l.needs_check('deck-stable', '111', 'c1'), 'a red result is re-checked')
        l.record('deck-stable', '111', 'error', 'c1', '', NOW)
        self.assertTrue(l.needs_check('deck-stable', '111', 'c1'), 'a fetch error is retried')

    def test_region_drift_is_measured_against_last_verified_build(self):
        l = ledger.Ledger.load(self.path)
        self.assertFalse(l.region_drifted('deck-stable', 'r1'), 'first build becomes the baseline')
        l.record('deck-stable', '111', 'pass', 'c', 'r1', NOW)
        self.assertFalse(l.region_drifted('deck-stable', 'r1'))
        self.assertTrue(l.region_drifted('deck-stable', 'r2'))
        l.record('deck-stable', '112', 'review', 'c', 'r2', NOW)
        self.assertTrue(l.region_drifted('deck-stable', 'r2'), 'review does not move the baseline')
        l.accept_drift('deck-stable')
        self.assertFalse(l.region_drifted('deck-stable', 'r2'), 'a person (or the behavioural canary) accepted it')

    def test_history_is_trimmed(self):
        l = ledger.Ledger.load(self.path)
        for i in range(ledger.HISTORY_MAX + 20):
            l.record('deck-stable', str(1000000 + i), 'pass', 'c', 'r', NOW)
        self.assertEqual(len(l.data['history']), ledger.HISTORY_MAX)

    def test_save_is_deterministic_and_leaves_no_temp_files(self):
        l = ledger.Ledger.load(self.path)
        l.record('a', '1', 'pass', 'c', 'r', NOW)
        l.save(self.path)
        first = self.path.read_text()
        l.save(self.path)
        self.assertEqual(self.path.read_text(), first)
        self.assertEqual([p.name for p in self.dir.iterdir()], ['ledger.json'])


class CanaryDead(unittest.TestCase):
    def ledger_with(self, last_run):
        l = ledger.Ledger({'schema': 1, 'last_run': last_run.isoformat() if last_run else None,
                           'channels': {}, 'history': []})
        return l

    def test_fresh_run_is_alive(self):
        self.assertFalse(ledger.canary_dead(self.ledger_with(NOW - timedelta(hours=26)), NOW))

    def test_exactly_three_days_is_still_alive_but_beyond_is_dead(self):
        self.assertFalse(ledger.canary_dead(self.ledger_with(NOW - timedelta(days=3)), NOW))
        self.assertTrue(ledger.canary_dead(self.ledger_with(NOW - timedelta(days=3, minutes=1)), NOW))

    def test_never_run_counts_as_dead(self):
        self.assertTrue(ledger.canary_dead(self.ledger_with(None), NOW))

    def test_naive_injected_date_is_treated_as_utc(self):
        self.assertTrue(ledger.canary_dead(self.ledger_with(NOW - timedelta(days=5)), NOW.replace(tzinfo=None)))


if __name__ == '__main__':
    unittest.main()
