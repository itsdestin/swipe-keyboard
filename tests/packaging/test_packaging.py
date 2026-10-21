"""Packaging contract tests; stage fixtures exercise the real CMake installer."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

class Packaging(unittest.TestCase):
    def test_multilingual_context_payload_license_and_package_copy_are_pinned(self):
        import hashlib
        import importlib.util
        import json
        spec=importlib.util.spec_from_file_location('package_check',ROOT/'scripts/package-check.py')
        checker=importlib.util.module_from_spec(spec);spec.loader.exec_module(checker)
        source_lock=json.loads((ROOT/'packaging/assets.lock.json').read_text())
        with tempfile.TemporaryDirectory() as tmp:
            base=pathlib.Path(tmp)/'usr/share/swipe-keyboard';packs=base/'language-packs';packs.mkdir(parents=True)
            terms=(ROOT/'packaging/language-models/TATOEBA-CC-BY-2.0-FR.txt').read_bytes()
            terms_path=packs/'TATOEBA-CC-BY-2.0-FR.txt';terms_path.write_bytes(terms)
            for language,pin in source_lock['multilingual_context_models'].items():
                model=f'{language}.ngram';vocab=f'{language}.vocab.tsv';credits=f'{language}-CONTRIBUTORS.txt'
                for name in (model,vocab,credits):
                    (packs/name).write_bytes((ROOT/'packaging/language-models'/name).read_bytes())
                manifest={'schema':2,'language':language,'contextFormat':'swk-ngram-v2',
                    'contextLicense':{'id':'CC-BY-2.0-FR','sourceSha256':pin['corpusSha256'],
                        'termsPath':'TATOEBA-CC-BY-2.0-FR.txt','termsSha256':hashlib.sha256(terms).hexdigest(),
                        'attributionPath':credits,'attributionSha256':pin['contributorsSha256']},
                    'assets':{'contextModel':{'path':model,'sha256':pin['modelSha256'],'size':pin['modelBytes'],'compatibility':f'{language}:context-ngram-v1'},
                        'contextVocab':{'path':vocab,'sha256':pin['vocabSha256'],'size':pin['vocabBytes'],'compatibility':f'{language}:context-ngram-v1'}}}
                (packs/f'{language}.json').write_text(json.dumps(manifest))
            checker.check_multilingual_context_assets(base,source_lock)
            old=(packs/'fr.ngram').read_bytes()
            (packs/'fr.ngram').write_bytes(old+b'bad')
            with self.assertRaisesRegex(AssertionError,'size mismatch'):
                checker.check_multilingual_context_assets(base,source_lock)
            (packs/'fr.ngram').write_bytes(old)
            self.assertEqual(hashlib.sha256(terms).hexdigest(),source_lock['tatoeba_license_notice_sha256'])

    def test_package_descriptions_and_license_metadata_match_multilingual_payloads(self):
        texts=[(ROOT/'packaging/arch/PKGBUILD').read_text(),
               (ROOT/'packaging/fedora/swipe-keyboard.spec').read_text(),
               (ROOT/'packaging/flatpak/org.itsdestin.SwipeKeyboard.desktop').read_text()]
        for text in texts:
            for language in ('English','Spanish','French','German'):
                self.assertIn(language,text)
            self.assertIn('suggestions',text)
            self.assertNotIn('English-only',text)
        self.assertIn('LicenseRef-Tatoeba-CC-BY-2.0-FR',texts[1])
        self.assertIn('custom:TATOEBA-CC-BY-2.0-FR.txt',texts[0])
        self.assertTrue((ROOT/'packaging/language-models/TATOEBA-CC-BY-2.0-FR.txt').is_file())

    def test_source_archive_retains_tracked_executorch_build_inputs(self):
        import shutil
        import tarfile
        with tempfile.TemporaryDirectory() as tmp:
            fixture = pathlib.Path(tmp)/'source'
            fixture.mkdir()
            (fixture/'scripts').mkdir()
            (fixture/'packaging').mkdir()
            shutil.copy2(ROOT/'scripts/package-source.sh', fixture/'scripts/package-source.sh')
            shutil.copy2(ROOT/'packaging/release-targets.json', fixture/'packaging/release-targets.json')
            tracked = 'third_party/swipe-library/third_party/executorch/build/build_variables.bzl'
            generated = ('build/generated.o', 'third_party/swipe-library/build/generated.o',
                         'third_party/swipe-library/third_party/executorch/cmake-out-host/generated.a')
            shim_tracked = 'third_party/swipe-library/third_party/executorch/shim_et/xplat/executorch/build/build_variables.bzl'
            for name in (tracked, shim_tracked, *generated):
                target = fixture/name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('fixture')
            subprocess.run(['git', 'init', '-q', str(fixture)], check=True)
            subprocess.run(['git', '-C', str(fixture), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(fixture), '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
                            'commit', '-qm', 'fixture'], check=True)
            (fixture/'personal-secret.txt').write_text('synthetic private content: never archive untracked files')
            output = pathlib.Path(tmp)/'dist'
            result = subprocess.run(['bash', str(fixture/'scripts/package-source.sh'), str(output)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with tarfile.open(output/'swipe-keyboard-0.2.1.tar.gz') as archive:
                names = {name.replace('/./', '/') for name in archive.getnames()}
            prefix = 'swipe-keyboard-0.2.1/'
            self.assertNotIn(prefix + 'personal-secret.txt', names)
            self.assertIn(prefix + tracked, names)
            self.assertIn(prefix + shim_tracked, names)
            for name in generated:
                self.assertNotIn(prefix + name, names)

    def test_desktop_entry_uses_real_kwin_protocol(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('package_check', ROOT/'scripts/package-check.py')
        checker = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(checker)
        self.assertTrue(hasattr(checker, 'check_desktop'), 'Validate actual KWin desktop entry keys, not an InputMethod substring')
        checker.check_desktop(ROOT/'integration/swipe-keyboard.desktop')
        with tempfile.TemporaryDirectory() as tmp:
            bad = pathlib.Path(tmp)/'bad.desktop'
            bad.write_text('\n'.join(['[Desktop Entry]', 'Type=Application', 'Exec=wrong-command',
                                      'X-KDE-Wayland-VirtualKeyboard=true', '']))
            with self.assertRaises((AssertionError, ValueError)):
                checker.check_desktop(bad)

    def test_native_package_descriptions_and_jurisdiction_license_metadata(self):
        arch=(ROOT/'packaging/arch/PKGBUILD').read_text()
        fedora=(ROOT/'packaging/fedora/swipe-keyboard.spec').read_text()
        desktop=(ROOT/'packaging/flatpak/org.itsdestin.SwipeKeyboard.desktop').read_text()
        notice=(ROOT/'packaging/NOTICE').read_text()
        for description in (arch, fedora, desktop):
            self.assertIn('English',description)
            self.assertIn('Spanish',description)
            self.assertIn('French',description)
            self.assertIn('German',description)
            self.assertIn('next-word suggestions',description)
            self.assertNotIn('English-only',description)
        # SPDX's verified catalog has CC-BY-2.0 (Generic), not a France-port identifier;
        # package metadata therefore uses an explicit custom ref and installs its exact notice.
        self.assertIn('LicenseRef-Tatoeba-CC-BY-2.0-FR',fedora)
        self.assertIn('custom:TATOEBA-CC-BY-2.0-FR.txt',arch)
        self.assertIn('TATOEBA-CC-BY-2.0-FR.txt',notice)
        self.assertIn('an unavailable or corrupt pack never falls back to English',notice)
        self.assertIn('raw application completion preference',notice)
        self.assertIn('sensitive and restricted fields remain blocked',notice.lower())

    def test_stage_install(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp)
            assets = p / 'assets'
            module = p / 'build/qml/SwipeKeyboard'
            module.mkdir(parents=True)
            for name in ('qmldir', 'SwipeCapture.qml', 'libswipekeyboard.so', 'libswipekeyboardplugin.so', 'libswipekb_engine.so'):
                (module / name).write_text('fixture')
            for name in ('models/honorable_sturgeon/model_fp32.pte', 'models/magic_macaw/model_fp32.pte', 'models/hungry_jellyfish/context_lm.pte', 'models/hungry_jellyfish/vocab.txt', 'models/FUTO-MODEL-LICENSE.md', 'dict/en_swipe.combined', 'language-packs/es.json', 'language-packs/fr.json', 'language-packs/de.json', 'licenses/NOTICE'):
                target = assets / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('fixture')
            source = p / 'src'
            source.mkdir()
            (source / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(stage NONE)
set(Qt6Core_VERSION_MAJOR 6)
set(Qt6Core_VERSION_MINOR 10)
set(SWIPEKB_SOURCE "{ROOT}")
set(SWIPEKB_ASSETS "{assets}")
set(SWIPEKB_QML_DIR "{module.parent}")
include("{ROOT}/cmake/Install.cmake")
''')
            subprocess.run(['cmake', '-S', str(source), '-B', str(p/'build'), '-DCMAKE_INSTALL_PREFIX=/usr'], check=True, capture_output=True)
            subprocess.run(['cmake', '--install', str(p/'build'), '--prefix', str(p/'stage/usr')], check=True, capture_output=True)
            stage = p/'stage/usr'
            for name in ('bin/swipe-keyboard', 'share/applications/swipe-keyboard.desktop', 'share/swipe-keyboard/integration/setup.py', 'share/swipe-keyboard/integration/layout.py', 'share/swipe-keyboard/qml/SwipeKeyboard/libswipekb_engine.so', 'share/swipe-keyboard/models/honorable_sturgeon/model_fp32.pte', 'share/swipe-keyboard/dict/en_swipe.combined', 'share/licenses/swipe-keyboard/LICENSE', 'share/licenses/swipe-keyboard/TATOEBA-CC-BY-2.0-FR.txt'):
                self.assertTrue((stage/name).is_file(), name)
            for language in ('es', 'fr', 'de'):
                self.assertTrue((stage/f'share/swipe-keyboard/language-packs/{language}.json').is_file())
            self.assertFalse((stage/'share/swipe-keyboard/eval').exists())
            self.assertEqual((stage/'share/swipe-keyboard/runtime.json').read_text(), '{"qt": "6.10", "package": "0.2.1-1"}\n')
            self.assertTrue((stage/'bin/swipe-keyboard').stat().st_mode & 0o111)
            self.assertTrue((stage/'share/swipe-keyboard/qml/SwipeKeyboard/libswipekb_engine.so').stat().st_mode & 0o111,
                            'Native libraries must retain executable mode for RPM debug-info discovery')

    def run_legacy_installer(self, extra_env):
        import os
        import shutil
        with tempfile.TemporaryDirectory() as tmp:
            fixture = pathlib.Path(tmp)
            shutil.copy2(ROOT/'install.sh', fixture/'install.sh')
            for directory in ('scripts/lm', 'assets/models', 'assets/dict', 'assets/language-packs', 'build/qml', 'bin', 'third_party/swipe-library/third_party/executorch/cmake-out-host'):
                (fixture/directory).mkdir(parents=True, exist_ok=True)
            (fixture/'third_party/swipe-library/third_party/executorch/cmake-out-host/libexecutorch.a').write_text('fixture')
            for command, text in {
                'scripts/fetch-assets.sh': 'echo fetch >> "$LOG"',
                'scripts/lm/build-lm.sh': 'echo ngram >> "$LOG"',
                'bin/cmake': 'echo cmake >> "$LOG"',
                'bin/ctest': 'echo ctest >> "$LOG"',
                'bin/pidof': 'echo forbidden-pidof >> "$LOG"; exit 99',
            }.items():
                script = fixture/command
                script.write_text('#!/bin/sh\n' + text + '\n')
                script.chmod(0o755)
            env = {key: value for key, value in os.environ.items() if not key.startswith('SWIPEKB_')}
            env.update(HOME=str(fixture/'home'), XDG_DATA_HOME=str(fixture/'home/data'),
                       PATH=str(fixture/'bin') + ':' + env['PATH'], LOG=str(fixture/'commands'))
            env.update(extra_env)
            result = subprocess.run(['bash', str(fixture/'install.sh'), '--no-restart'], env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            commands = (fixture/'commands').read_text().splitlines()
            self.assertIn('ctest', commands)
            self.assertNotIn('forbidden-pidof', commands)
            return commands

    def test_legacy_installer_does_not_build_ngram_by_default(self):
        self.assertNotIn('ngram', self.run_legacy_installer({}))

    def test_legacy_installer_builds_ngram_only_on_explicit_opt_in(self):
        self.assertIn('ngram', self.run_legacy_installer({'SWIPEKB_BUILD_LM': '1'}))

    def test_legacy_installer_skip_overrides_opt_in(self):
        self.assertNotIn('ngram', self.run_legacy_installer({'SWIPEKB_BUILD_LM': '1', 'SWIPEKB_SKIP_LM': '1'}))

    def test_fetch_checks_cached_and_downloaded_bytes(self):
        import importlib.util
        import hashlib
        spec = importlib.util.spec_from_file_location('fetch_assets', ROOT/'scripts/fetch-assets.py')
        fetcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fetcher)
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp)/'source'
            source.write_bytes(b'pinned asset')
            target = pathlib.Path(tmp)/'cached'
            target.write_bytes(b'corrupt cached content')
            asset = {'path': 'model', 'url': source.as_uri(), 'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
            fetcher.fetch(asset, target)
            self.assertEqual(target.read_bytes(), b'pinned asset')
            source.write_bytes(b'upstream changed')
            target.unlink()
            with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                fetcher.fetch(asset, target)
            self.assertFalse(target.exists())
            self.assertFalse(target.with_name('cached.part').exists())

    def test_portable_default(self):
        cmake = (ROOT/'CMakeLists.txt').read_text()
        self.assertIn('set(USE_MARCH_NATIVE OFF', cmake)
        script = (ROOT/'scripts/build-futo.sh').read_text()
        self.assertIn('-DUSE_MARCH_NATIVE=OFF', script)

    def test_config_probe_is_part_of_native_build_install_and_elf_checks(self):
        self.assertIn('add_subdirectory(integration)', (ROOT/'CMakeLists.txt').read_text())
        native = (ROOT/'integration/CMakeLists.txt').read_text()
        self.assertIn('KF6::ConfigCore', native)
        self.assertIn('install(TARGETS swipekb-config', native)
        self.assertIn('$<TARGET_FILE:swipekb-config>', (ROOT/'plugin/CMakeLists.txt').read_text())
        self.assertIn('integration/swipekb-config', (ROOT/'scripts/package-check.py').read_text())
        self.assertRegex((ROOT/'packaging/fedora/swipe-keyboard.spec').read_text(),
                         r'(?m)^BuildRequires: [^\n]*\bkf6-kconfig-devel\b')

    def test_recipes_declare_busctl_provider_and_run_integration_tests(self):
        arch = (ROOT/'packaging/arch/PKGBUILD').read_text()
        rpm = (ROOT/'packaging/fedora/swipe-keyboard.spec').read_text()
        self.assertRegex(arch, r"(?m)^depends=\([^\n]*'systemd'")
        self.assertRegex(rpm, r'(?m)^Requires: [^\n]*\bsystemd\b')
        for text in (arch, rpm):
            self.assertIn('-m unittest discover -s tests/integration -v', text)

    def test_native_checks_have_real_stock_keyboard_not_dependency_bypass(self):
        rpm = (ROOT/'packaging/fedora/swipe-keyboard.spec').read_text()
        workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        self.assertRegex(rpm, r'(?m)^BuildRequires: [^\n]*\bplasma-keyboard\b')
        self.assertIn('gtest curl plasma-keyboard', workflow)
        self.assertNotIn('--nodeps', workflow)

    def test_fedora_container_prepares_real_busctl_provider(self):
        workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        self.assertIn('dnf -y swap systemd-standalone-tmpfiles systemd', workflow)
        self.assertIn('rpm -q systemd-standalone-tmpfiles', workflow)

    def test_ci_checks_installed_wrapper_as_ordinary_user(self):
        workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        # Arch baseline/current plus Fedora x86_64 and mandatory Fedora ARM64.
        self.assertEqual(workflow.count('XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=KDE swipe-keyboard check'), 4)
        self.assertEqual(workflow.count('test ! -e /usr/bin/swipe-keyboard'), 4)

    def test_fedora_ci_retains_test_executable_until_package_probe(self):
        workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        self.assertIn('rpmbuild -ba --noclean', workflow)

    def test_ci_loads_extracted_package_in_real_stock_inputpanel(self):
        workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        self.assertEqual(workflow.count('--runtime-root dist/stage/usr/share/swipe-keyboard'), 4)
        self.assertEqual(workflow.count('python3 scripts/check-stock-layout.py'), 4)

    def test_ci_extracts_main_arch_package_not_debug_archive_wildcard(self):
        workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        self.assertNotIn('tar -xf dist/*.pkg.tar.zst', workflow)
        self.assertIn('tar -xf dist/swipe-keyboard-$PACKAGE_VERSION-x86_64.pkg.tar.zst', workflow)

    def test_arch_ci_uses_older_baseline_and_retests_identical_bytes_on_current_runtime(self):
        workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        self.assertIn('archive.archlinux.org/repos/2026/07/01/$repo/os/$arch', workflow)
        self.assertIn('pacman -Syyuu --noconfirm', workflow)
        self.assertIn('cp /tmp/current-mirrorlist /etc/pacman.d/mirrorlist', workflow)
        self.assertIn('sha256sum -c dist/baseline-package.sha256', workflow)
        self.assertIn('packaging/BASELINE-VERSIONS.txt', workflow)
        self.assertIn('dist/CURRENT-VERSIONS.txt', workflow)
        self.assertLess(workflow.index('pacman -Syyuu --noconfirm'), workflow.index('makepkg --noconfirm'))
        self.assertGreater(workflow.index('sha256sum -c'), workflow.index('makepkg --noconfirm'))

    def test_native_builds_include_kirigami_for_accent_test(self):
        arch = (ROOT/'packaging/arch/PKGBUILD').read_text()
        rpm = (ROOT/'packaging/fedora/swipe-keyboard.spec').read_text()
        workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        self.assertRegex(arch, r"(?m)^makedepends=\([^\n]*'kirigami'")
        self.assertRegex(rpm, r'(?m)^BuildRequires: [^\n]*\bkf6-kirigami\b')
        self.assertIn('kconfig kirigami gtest', workflow)

    def test_arch_metadata_can_be_read_before_build(self):
        import os
        import shutil
        if not shutil.which('makepkg'):
            self.skipTest('makepkg is only required on the Arch build target')
        with tempfile.TemporaryDirectory() as tmp:
            env = {**os.environ, 'HOME': tmp}
            shutil.copy2(ROOT/'packaging/arch/PKGBUILD', pathlib.Path(tmp)/'PKGBUILD')
            result = subprocess.run(['makepkg', '--printsrcinfo'], cwd=tmp, env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("package '' was not found", result.stderr)

    def test_arch_native_dependencies_merge_without_duplicate_provider_entries(self):
        import json
        import os
        import shutil
        import sys
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp)
            version = json.loads((ROOT/'packaging/release-targets.json').read_text())['version']
            source = p/f'src/swipe-keyboard-{version}/scripts'
            source.mkdir(parents=True)
            (source/'package-check.py').write_text('pass')
            command = '''
set -e
source "$1"
srcdir="$2/src"; pkgdir="$2/pkg"
cmake() { :; }
python() {
  if [[ $1 == scripts/arch-dependencies.py ]]; then
    printf '%s\\n' 'qt6-base>=6.11' 'qt6-declarative>=6.11' 'kwindowsystem'
  else command python3 "$@"; fi
}
package
printf '%s\\n' "${depends[@]}"
'''
            # WHY: Fedora provides python3 without an unversioned python command.
            # The shell shim must work in that environment too, not just on Arch.
            binary = p/'bin'
            binary.mkdir()
            (binary/'python3').symlink_to(sys.executable)
            result = subprocess.run([shutil.which('bash'), '-c', command, 'test', str(ROOT/'packaging/arch/PKGBUILD'), str(p)],
                                    env={**os.environ, 'PATH': str(binary)}, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            dependencies = result.stdout.splitlines()
            self.assertIn('qt6-base>=6.11', dependencies)
            self.assertIn('qt6-declarative>=6.11', dependencies)
            self.assertNotIn('qt6-declarative', dependencies)
            self.assertEqual(dependencies.count('kwindowsystem'), 1)
            self.assertEqual(sum(d.startswith('qt6-declarative') for d in dependencies), 1)

    def test_recipes_no_transaction_hooks(self):
        arch = (ROOT/'packaging/arch/PKGBUILD').read_text()
        rpm = (ROOT/'packaging/fedora/swipe-keyboard.spec').read_text()
        self.assertIn('plasma-keyboard>=6.7', arch)
        self.assertIn('plasma-keyboard<6.9', arch)
        self.assertIn('Requires: plasma-keyboard >= 1:6.7', rpm)
        self.assertIn('Requires: plasma-keyboard < 1:6.9', rpm)
        for text in (arch, rpm):
            for forbidden in ('kwriteconfig', 'kreadconfig', '%post', '%preun', 'post_install', '-march=native'):
                self.assertNotIn(forbidden, text)

if __name__ == '__main__':
    unittest.main()
