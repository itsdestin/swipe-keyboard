# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Test for tools/swipe-stats with a synthetic recording. Run: python3 -I tools/tests/test_swipe_stats.py"""
import importlib.machinery
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "swipe-stats")


def ms(day, hour=12):
    return int(datetime(2026, 10, day, hour).timestamp() * 1000)


class SwipeStats(unittest.TestCase):
    def setUp(self):
        d = tempfile.mkdtemp()
        self.path = os.path.join(d, "swipes.jsonl")
        sw = lambda ts: {"ts": ts, "type": "swipe", "points": [], "keys": [], "context": ["SECRETCONTEXT"],
                         "candidates": ["SECRETWORD"], "typed": "SECRETWORD"}
        a, b, c, d4, e = ms(10, 9), ms(10, 10), ms(10, 11), ms(10, 12), ms(10, 13)
        lines = [sw(a), sw(b), sw(c), sw(d4), sw(e),
                 {"ts": a + 5, "type": "pick", "swipe_ts": a, "word": "SECRETWORD"},
                 {"ts": b + 5, "type": "erase", "swipe_ts": b},
                 {"ts": c + 5, "type": "keep", "swipe_ts": c},
                 sw(ms(1))]  # old swipe, outside the 7 days, no outcome
        with open(self.path, "w") as f:
            for l in lines:
                f.write(json.dumps(l) + "\n")
            f.write("not json\n")
        ldr = importlib.machinery.SourceFileLoader("swipe_stats", TOOL)
        spec = importlib.util.spec_from_loader("swipe_stats", ldr)
        self.mod = importlib.util.module_from_spec(spec)
        ldr.exec_module(self.mod)

    def test_counts(self):
        from datetime import date
        r = self.mod.build(self.path, today=date(2026, 10, 12))
        day = r["days"]["2026-10-10"]
        self.assertEqual((day["swipes"], day["fixes"], day["keeps"], day["unknown"]), (5, 2, 1, 2))
        self.assertEqual(day["fixes_per_100"], 40.0)
        week = r["last_7_days"]
        self.assertEqual((week["swipes"], week["unknown"]), (5, 2))   # Oct 1 swipe excluded
        self.assertEqual(r["days"]["2026-10-01"]["unknown"], 1)

    def test_cli_never_prints_words(self):
        for extra in ([], ["--json"]):
            out = subprocess.run([sys.executable, "-I", TOOL, *extra, self.path], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertNotIn("SECRET", out.stdout)
        self.assertIn("days", json.loads(out.stdout))

    def test_missing_file(self):
        out = subprocess.run([sys.executable, "-I", TOOL, self.path + ".nope"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 1)


if __name__ == "__main__":
    unittest.main()
