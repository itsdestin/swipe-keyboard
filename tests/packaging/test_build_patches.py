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
    def test_xnnpack_patch_works_in_source_archive_inside_unrelated_git_checkout(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('build_patches', ROOT/'scripts/apply-build-patches.py')
        patches = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(patches)
        upstream = ROOT/'third_party/swipe-library'
        with tempfile.TemporaryDirectory() as tmp:
            checkout = Path(tmp)
            subprocess.run(['git', 'init', '-q', str(checkout)], check=True)
            # Source archives have no .git; CI extracts them below its outer checkout.
            root = checkout/'dist/src/swipe/third_party/executorch'
            target = root/'backends/xnnpack/CMakeLists.txt'
            target.parent.mkdir(parents=True)
            target.write_text((upstream/'third_party/executorch/backends/xnnpack/CMakeLists.txt').read_text())
            patch = upstream/'patches/executorch-xnnpack-fexceptions.patch'
            reverse = ['git', 'apply', '--unsafe-paths', '--directory=' + str(root), '--reverse']
            if subprocess.run(reverse + ['--check', str(patch)], capture_output=True).returncode == 0:
                subprocess.run(reverse + [str(patch)], check=True)
            self.assertNotIn('target_compile_options(xnnpack_backend PRIVATE -fexceptions)', target.read_text())
            # Reproduce the false reverse-check success of the upstream Makefile.
            skipped = subprocess.run(['git', '-C', str(root), 'apply', '--reverse',
                                      '--check', str(patch)], capture_output=True)
            self.assertEqual(skipped.returncode, 0, skipped.stderr)
            for _ in range(2):
                patches.apply_patch(root, patch)
                self.assertEqual(target.read_text().count(
                    'target_compile_options(xnnpack_backend PRIVATE -fexceptions)'), 1)
            target.write_text('unknown upstream version')
            with self.assertRaises(subprocess.CalledProcessError):
                patches.apply_patch(root, patch)
            self.assertEqual(target.read_text(), 'unknown upstream version')

    def test_build_applies_pinned_xnnpack_patch_before_upstream_make(self):
        source = (ROOT/'scripts/apply-build-patches.py').read_text()
        self.assertIn("'patches/executorch-xnnpack-fexceptions.patch'", source)
        script = (ROOT/'scripts/build-futo.sh').read_text()
        self.assertLess(script.index('scripts/apply-build-patches.py'), script.index('make -C'))

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
