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
            if subprocess.run(reverse + ['--check', str(patch)], cwd=checkout,
                              capture_output=True).returncode == 0:
                subprocess.run(reverse + [str(patch)], cwd=checkout, check=True)
            self.assertNotIn('target_compile_options(xnnpack_backend PRIVATE -fexceptions)', target.read_text())
            # Reproduce the false reverse-check success of the upstream Makefile.
            skipped = subprocess.run(['git', '-C', str(root), 'apply', '--reverse',
                                      '--check', str(patch)], capture_output=True)
            self.assertEqual(skipped.returncode, 0, skipped.stderr)
            import os
            previous = Path.cwd()
            try:
                os.chdir(checkout/'dist/src/swipe')
                for _ in range(2):
                    patches.apply_patch(root, patch)
                    self.assertEqual(target.read_text().count(
                        'target_compile_options(xnnpack_backend PRIVATE -fexceptions)'), 1,
                        f'Patch skipped from actual archive cwd {Path.cwd()}')
                target.write_text('unknown upstream version')
                with self.assertRaises(subprocess.CalledProcessError):
                    patches.apply_patch(root, patch)
                self.assertEqual(target.read_text(), 'unknown upstream version')
            finally:
                os.chdir(previous)

    def test_copied_entrypoint_changes_archive_and_checkout_inputs_from_actual_cwd(self):
        import shutil
        import sys
        upstream = ROOT/'third_party/swipe-library'
        with tempfile.TemporaryDirectory() as tmp:
            outer = Path(tmp)
            subprocess.run(['git', 'init', '-q', str(outer)], check=True)
            for checkout in (False, True):
                with self.subTest(checkout=checkout):
                    project = outer/'dist/src'/('checkout' if checkout else 'archive')
                    swipe = project/'third_party/swipe-library'
                    et = swipe/'third_party/executorch'
                    names = ('CMakeLists.txt', 'tests/test_metadata.cpp',
                             'src/model_metadata.cpp', 'include/swipe_decoder/model_metadata.hpp',
                             'patches/executorch-xnnpack-fexceptions.patch',
                             'third_party/executorch/backends/xnnpack/CMakeLists.txt')
                    for name in names:
                        target = swipe/name
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(upstream/name, target)
                    scripts = project/'scripts'
                    (scripts/'patches').mkdir(parents=True)
                    shutil.copyfile(ROOT/'scripts/apply-build-patches.py', scripts/'apply-build-patches.py')
                    shutil.copyfile(ROOT/'scripts/patches/swipe-metadata.patch', scripts/'patches/swipe-metadata.patch')
                    # Normalize either pristine or already patched source copies.
                    for root, patch in ((swipe, scripts/'patches/swipe-metadata.patch'),
                                        (et, swipe/'patches/executorch-xnnpack-fexceptions.patch')):
                        command = ['git', 'apply', '--unsafe-paths', '--directory=' + str(root), '--reverse']
                        if subprocess.run(command + ['--check', str(patch)], cwd=outer,
                                          capture_output=True).returncode == 0:
                            subprocess.run(command + [str(patch)], cwd=outer, check=True)
                    target = et/'backends/xnnpack/CMakeLists.txt'
                    flag = 'target_compile_options(xnnpack_backend PRIVATE -fexceptions)'
                    self.assertNotIn(flag, target.read_text())
                    metadata = swipe/'tests/test_metadata.cpp'
                    metadata_before = metadata.read_text()
                    flatbuffers = et/'third-party/CMakeLists.txt'
                    flatbuffers.parent.mkdir()
                    flatbuffers.write_text(SOURCE)
                    if checkout:
                        subprocess.run(['git', 'init', '-q', str(project)], check=True)
                        subprocess.run(['git', 'init', '-q', str(et)], check=True)
                    command = [sys.executable, str(scripts/'apply-build-patches.py'), str(et),
                               '--swipe-library', str(swipe)]
                    for _ in range(2):
                        result = subprocess.run(command, cwd=project, capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertEqual(target.read_text().count(flag), 1, f'cwd={project}')
                        self.assertNotEqual(metadata.read_text(), metadata_before, f'cwd={project}')
                        self.assertEqual(flatbuffers.read_text().count('-DCMAKE_POSITION_INDEPENDENT_CODE=ON'), 1)
                    # Packaging tests execute AFTER production patches, from this
                    # nested archive cwd. Exercise their real normalization too.
                    shutil.copytree(upstream/'third_party/executorch/third-party/json/include',
                                    et/'third-party/json/include')
                    test_dir = project/'tests/packaging'
                    test_dir.mkdir(parents=True)
                    for name in ('test_build_patches.py', 'test_metadata_contract.py'):
                        shutil.copyfile(ROOT/'tests/packaging'/name, test_dir/name)
                    for model in ('honorable_sturgeon', 'magic_macaw', 'hungry_jellyfish'):
                        asset = project/f'assets/models/{model}/metadata.json'
                        asset.parent.mkdir(parents=True)
                        shutil.copyfile(ROOT/f'assets/models/{model}/metadata.json', asset)
                    for name, pattern in (('test_build_patches.py', 'xnnpack_patch_works'),
                                          ('test_metadata_contract.py', 'real_contextlm_schema')):
                        checked = subprocess.run([sys.executable, '-m', 'unittest', 'discover',
                                                  '-s', str(test_dir), '-p', name, '-k', pattern, '-v'],
                                                 cwd=project, capture_output=True, text=True)
                        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                    patched_xnnpack = target.read_text()
                    for unknown in (target, metadata):
                        unknown.write_text('unknown upstream version')
                        result = subprocess.run(command, cwd=project, capture_output=True, text=True)
                        self.assertNotEqual(result.returncode, 0, f'unknown input accepted; cwd={project}')
                        self.assertEqual(unknown.read_text(), 'unknown upstream version')
                        target.write_text(patched_xnnpack)

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
