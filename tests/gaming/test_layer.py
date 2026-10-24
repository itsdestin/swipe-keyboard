#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Runs the gesture-layer tests (tests/gaming/test_layer.js, plan M4) under Node so `unittest discover` sees them.
A missing Node is a hard failure, not a skip: a green run must mean the layer really ran.
The page frame is assembled by the helper's own code (Helper._stub) so the test runs exactly what Steam would get."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'gaming' / 'helper'))

from swipekb_helper.config import Config  # noqa: E402
from swipekb_helper.helper import Helper  # noqa: E402


class GestureLayer(unittest.TestCase):
    def test_node_suite(self):
        node = shutil.which('node')
        self.assertIsNotNone(node, 'node is required for the gesture layer tests')
        tmp = Path(tempfile.mkdtemp(prefix='swipekb-layer-test-'))
        self.addCleanup(shutil.rmtree, tmp, True)
        h = Helper(Config(state_dir=tmp / 'state', marker_dirs=[tmp], layer_watchdog=0.6,
                          contract_path=ROOT / 'gaming' / 'contract' / 'gaming-contract.json'))
        h._load_contract()
        stub = tmp / 'frame.js'
        stub.write_text(h._stub())
        # A second frame for the typing-session tests: quick ticks, and a watchdog that will not fire during a test.
        h2 = Helper(Config(state_dir=tmp / 'state2', marker_dirs=[tmp], layer_watchdog=3600, session={'tick_ms': 20, 'trail_ms': 80},
                           failsafe={'tick_ms': 20, 'focus_ms': 60},
                           contract_path=ROOT / 'gaming' / 'contract' / 'gaming-contract.json'))
        h2._load_contract()
        stub2 = tmp / 'frame-long.js'
        stub2.write_text(h2._stub())
        env = dict(os.environ, SWIPEKB_STUB_FILE=str(stub), SWIPEKB_STUB_LONG=str(stub2))
        r = subprocess.run([node, '--test', '--test-reporter=spec', str(HERE / 'test_layer.js'), str(HERE / 'test_session.js'), str(HERE / 'test_failsafe.js')],
                           capture_output=True, text=True, timeout=300, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
