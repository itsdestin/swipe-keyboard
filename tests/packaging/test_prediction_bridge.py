"""Apply/reverse the policy patch against explicit pinned source, never a host backend."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PredictionBridgePatch(unittest.TestCase):
    def test_patch_applies_and_reverses_without_unrelated_changes(self):
        source = os.environ.get('SWIPEKB_TEST_BACKEND_SOURCE')
        if not source:
            self.skipTest('Set SWIPEKB_TEST_BACKEND_SOURCE to pristine pinned backend source')
        names = ['inputplugin.h', 'inputplugin.cpp', 'inputlisteneritem.h', 'inputlisteneritem.cpp']
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'src').mkdir()
            original = {}
            for name in names:
                data = (Path(source) / 'src' / name).read_bytes()
                original[name] = data
                (work / 'src' / name).write_bytes(data)
            patch = ROOT / 'scripts/patches/plasma-prediction-policy.patch'
            command = ['git', 'apply', '--unsafe-paths', '--directory=' + str(work)]
            subprocess.run(command + ['--check', str(patch)], check=True, capture_output=True)
            subprocess.run(command + [str(patch)], check=True, capture_output=True)
            # Cross-language contract: the Qt event patch and separately compiled
            # state header must travel together in the Flatpak source manifest.
            shutil.copyfile(ROOT / 'scripts/patches/backendpredictionpolicy.h', work / 'src/backendpredictionpolicy.h')
            flags = subprocess.check_output(['pkg-config', '--cflags', 'Qt6VirtualKeyboard', 'Qt6Quick'], text=True).split()
            for name in ['inputplugin.h', 'inputlisteneritem.h']:
                subprocess.run(['c++', '-std=c++20', '-fPIC', '-fsyntax-only', '-DQT_NO_CAST_FROM_ASCII', '-x', 'c++', *flags,
                                str(work / 'src' / name)], check=True, capture_output=True)
            subprocess.run(command + ['--reverse', str(patch)], check=True, capture_output=True)
            for name in names:
                self.assertEqual((work / 'src' / name).read_bytes(), original[name])


if __name__ == '__main__':
    unittest.main()
