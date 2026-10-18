"""Host flatc must compile PIC when Fedora's inherited linker flags require PIE."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = '''ExternalProject_Add(
  flatbuffers_ep
  CMAKE_ARGS -DFLATBUFFERS_BUILD_FLATC=ON
             -DFLATBUFFERS_BUILD_TESTS=OFF
             -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
)
'''


class BuildPatches(unittest.TestCase):
    def test_flatbuffers_pic_patch_is_guarded_and_idempotent(self):
        script = ROOT / 'scripts/apply-build-patches.py'
        self.assertTrue(script.is_file(), 'Missing Fedora host-tool PIC fix')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / 'third-party/CMakeLists.txt'
            target.parent.mkdir()
            target.write_text(SOURCE)
            for _ in range(2):
                subprocess.run(['python3', str(script), str(root)], check=True, capture_output=True)
                self.assertEqual(target.read_text().count('-DCMAKE_POSITION_INDEPENDENT_CODE=ON'), 1)
            target.write_text('unknown upstream version')
            result = subprocess.run(['python3', str(script), str(root)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(target.read_text(), 'unknown upstream version')


if __name__ == '__main__':
    unittest.main()
