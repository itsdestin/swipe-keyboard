"""Opt-in: download Valve's real public bundles and run the canary. Off by default (slow, needs network).
Run with: STEAM_CANARY_LIVE=1 python3 -m unittest test_real_bundles
WHY opt-in: the repository must not carry Valve's UI code, so real bundles are fetched at test time only."""
import os
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from support import ROOT
import canary
import fetch


@unittest.skipUnless(os.environ.get('STEAM_CANARY_LIVE') == '1', 'set STEAM_CANARY_LIVE=1 to download real bundles')
class RealBundles(unittest.TestCase):
    def test_current_channels_pass(self):
        d = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(d, ignore_errors=True))
        s = canary.run_canary(ROOT / 'gaming/contract/gaming-contract.json', d / 'ledger.json', d,
                              datetime.now(timezone.utc), fetch.Fetcher('https://client-update.steamstatic.com/'))
        self.assertFalse(s.failed, s.markdown())


if __name__ == '__main__':
    unittest.main()
