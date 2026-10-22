"""Small wiring guards, supplementing behavioral validator tests and actionlint."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        # Job boundaries only; actionlint validates the actual YAML/expression schema.
        self.jobs = dict(re.findall(r'^  ([a-z][a-z0-9-]*):\n(.*?)(?=^  [a-z][a-z0-9-]*:|\Z)',
                                    self.workflow.split('jobs:\n', 1)[1], re.M | re.S))

    def test_download_guide_includes_every_required_installer_and_version(self):
        import json
        contract = json.loads((ROOT/'packaging/release-targets.json').read_text())
        tag = f"v{contract['version']}-{contract['release']}"
        guide = (ROOT/'README.md').read_text()
        # WHY: a complete upload is not enough if instructions still send Arch
        # or ARM users to an old release or omit their package altogether.
        for name, target in contract['targets'].items():
            if name == 'source':
                continue
            installer = target['files'][0].format(version=contract['version'], release=contract['release'])
            with self.subTest(target=name):
                self.assertIn(f'https://github.com/itsdestin/swipe-keyboard/releases/download/{tag}/{installer}', guide)
        self.assertTrue((ROOT/'docs/releases'/f'{tag}.md').is_file())

    def test_real_producers_and_always_completion_gate(self):
        self.assertEqual(set(self.jobs), {'prepare', 'arch', 'fedora', 'fedora-arm64', 'flatpak', 'keyboard-ci', 'publish'})
        gate = self.jobs['keyboard-ci']
        self.assertIn('name: Keyboard CI', gate)
        self.assertIn('if: ${{ always() }}', gate)
        self.assertIn('needs: [prepare, arch, fedora, fedora-arm64, flatpak]', gate)
        self.assertIn('${{ toJSON(needs) }}', gate)
        self.assertIn('scripts/release.py gate', gate)
        for name, build in [('arch', 'makepkg --noconfirm'), ('fedora', 'rpmbuild -ba'), ('fedora-arm64', 'rpmbuild -bb'), ('flatpak', 'packaging/flatpak/build.py')]:
            producer = self.jobs[name]
            self.assertIn(build, producer)
            self.assertIn('--target ' + name, producer)
            self.assertNotIn('continue-on-error', producer)
            self.assertNotRegex(producer, r'(?m)^    if:', name)

    def test_arm_is_native_full_recipe_build_and_collision_free_upload(self):
        arm = self.jobs['fedora-arm64']
        self.assertIn('runs-on: ubuntu-24.04-arm', arm)
        self.assertIn('container: fedora:44', arm)
        self.assertIn('test "$(uname -m)" = aarch64', arm)
        self.assertIn('df -h', arm)
        self.assertIn('rpmbuild -bb --noclean', arm)
        self.assertNotIn('.src.rpm', arm)
        self.assertIn('scripts/package-check.py dist/stage', arm)
        self.assertIn('scripts/check-stock-layout.py', arm)
        self.assertIn('swipe-keyboard check', arm)
        self.assertIn('fedora-arm64-files', arm)
        self.assertIn('fedora-arm64-evidence', arm)
        recipe = (ROOT/'packaging/fedora/swipe-keyboard.spec').read_text()
        self.assertIn('ExclusiveArch: x86_64 aarch64', recipe)
        self.assertIn('-DUSE_MARCH_NATIVE=OFF', recipe)
        self.assertIn('QT_QPA_PLATFORM=offscreen %ctest', recipe)
        for suite in ('packaging', 'integration'):
            self.assertIn('discover -s tests/' + suite + ' -v', recipe)

    def test_prepare_runs_only_host_independent_contract_tests(self):
        # WHY: the Ubuntu preparation runner has Python, not the distro Qt/KF
        # development stack. Native suites belong in their actual package jobs.
        prepare = self.jobs['prepare']
        self.assertNotIn('discover -s tests/integration', prepare)
        self.assertNotIn('discover -s tests/packaging -v', prepare)
        for name in ('test_release.py', 'test_publish.py', 'test_release_workflow.py'):
            self.assertIn('-p ' + name, prepare)
        for name in ('arch', 'fedora'):
            self.assertIn('makepkg' if name == 'arch' else 'rpmbuild', self.jobs[name])

    def test_triggers_credentials_and_same_run_downloads(self):
        self.assertIn('  pull_request:\n', self.workflow)
        self.assertIn('    branches: [main]', self.workflow)
        self.assertIn("    tags: ['v*']", self.workflow)
        self.assertIn('  workflow_dispatch:', self.workflow)
        self.assertNotIn('pull_request_target', self.workflow)
        self.assertNotIn('    paths:', self.workflow)
        self.assertEqual(self.workflow.count('contents: write'), 1)
        self.assertIn('contents: write', self.jobs['publish'])
        self.assertIn('needs: [keyboard-ci]', self.jobs['publish'])
        self.assertIn('environment: keyboard-release', self.jobs['publish'])
        self.assertIn("github.event_name == 'push'", self.jobs['publish'])
        self.assertIn("github.repository == 'itsdestin/swipe-keyboard'", self.jobs['publish'])
        self.assertNotIn('run-id:', self.workflow)
        self.assertNotIn('repository:', self.workflow)
        # WHY changed from "no actions/cache at all": approved dependency-only caches now exist.
        # The release guarantees are pinned in test_dependency_caches_are_trusted_and_never_touch_releases.
        self.assertNotIn('actions/cache@', self.workflow)
        self.assertNotIn('0.2.', self.workflow)

    def test_dependency_caches_are_trusted_and_never_touch_releases(self):
        import json
        # WHY: tag candidates must rebuild everything cold, and only trusted events may write.
        for name in ('arch', 'fedora', 'fedora-arm64', 'flatpak'):
            job = self.jobs[name]
            self.assertIn('scripts/ci-cache.py policy', job, name)
            found = 0
            for use in re.finditer(r'(?ms)^      - name:[^\n]*\n(?:(?!^      - ).)*?uses: actions/cache/(restore|save)@v4.*?(?=^      - |\Z)', job):
                step = use[0]
                found += 1
                with self.subTest(job=name, step=step.splitlines()[0]):
                    self.assertIn('steps.cachepolicy.outputs.' + ('restore' if use[1] == 'restore' else 'save') + " == 'true'", step)
            self.assertGreaterEqual(found, 2, name)
        self.assertNotIn('actions/cache', self.jobs['publish'])
        self.assertNotIn('actions/cache', self.jobs['keyboard-ci'])
        self.assertNotIn('actions/cache', self.jobs['prepare'])
        # The saved paths are dependency state only, never packages, QRC, install trees or build dirs.
        saved = re.findall(r'cache/save@v4\n\s+with:\n\s+path: (\S+)', self.workflow)
        self.assertEqual(sorted(set(saved)), ['.flatpak-builder/ccache', '/var/tmp/ccache-et', 'flatpak-stage-cache.tar.zst'])
        # The stage cache is packed by a dependency-only run, before the keyboard module exists.
        flatpak = self.jobs['flatpak']
        self.assertLess(flatpak.index('--dependencies-only'), flatpak.index('ci-cache.py pack-flatpak-state'))
        self.assertLess(flatpak.index('ci-cache.py pack-flatpak-state'), flatpak.index('flatpak-build --repo flatpak-repo --ci-manifest prepared/flatpak-inputs/manifest.json ${{'))
        # Restores are validated and fall back to a rebuild.
        self.assertIn('unpack-flatpak-state', flatpak)
        self.assertIn("steps.stagecheck.outputs.usable != 'true'", flatpak)
        # Platform caches never share keys.
        for label in ('arch', 'fedora', 'fedora-arm64'):
            self.assertIn(f'native-ccache "{label}|', self.jobs[label])
        policy = json.dumps(self.jobs['arch'])
        self.assertNotIn('refs/tags', policy)

    def test_cache_steps_only_use_tools_that_are_already_installed(self):
        # WHY (run 39822604368): the dependency build ran before the Flatpak runtimes were installed,
        # and Fedora's toolchain query failed under pipefail before the compilers existed.
        flatpak = self.jobs['flatpak']
        self.assertLess(flatpak.index('Install signed exact SDK'), flatpak.index('--dependencies-only'))
        for name in ('fedora', 'fedora-arm64'):
            self.assertIn('|| true)', self.jobs[name])
            self.assertLess(self.jobs[name].index('dnf -y install ccache'), self.jobs[name].index('rpm -q gcc-c++'))

    def test_flatpak_compiler_cache_is_saved_only_after_the_build_succeeded(self):
        # WHY: run 39827309452 saved an empty store after a failed step; keys are immutable.
        flatpak = self.jobs['flatpak']
        self.assertIn('id: flatpakbuild', flatpak)
        save = flatpak[flatpak.index('Save ExecuTorch compiler cache'):]
        self.assertIn("steps.flatpakbuild.outcome == 'success'", save.split('uses:')[0])

    def test_flatpak_records_peak_memory_for_the_parallelism_bound(self):
        flatpak = self.jobs['flatpak']
        self.assertIn('ci-memwatch.py sample', flatpak)
        self.assertIn('ci-memwatch.py summary', flatpak)
        self.assertLess(flatpak.index('ci-memwatch.py sample'), flatpak.index('packaging/flatpak/build.py flatpak-build --repo flatpak-repo --ci-manifest prepared/flatpak-inputs/manifest.json ${{'))

    def test_prepare_shallows_dependencies_not_tag_ancestry(self):
        # Fetching full history recursively pulls enormous unused upstream histories.
        # Only the small root repository needs history for the trusted-tag check.
        prepare = self.jobs['prepare']
        self.assertIn('fetch-depth: 1', prepare)
        self.assertNotIn('fetch-depth: 0', prepare)
        self.assertIn('git fetch --unshallow --no-tags origin', prepare)
        self.assertIn('git merge-base --is-ancestor', prepare)

    def test_arch_initializes_container_keyring_before_baseline_downgrade(self):
        arch = self.jobs['arch']
        self.assertIn('container: archlinux:latest', arch)
        self.assertLess(arch.index('pacman-key --init'), arch.index('pacman-key --populate archlinux'))
        self.assertLess(arch.index('pacman-key --populate archlinux'), arch.index('retry_pacman -Syyuu'))
        self.assertNotIn('SigLevel', arch)
        self.assertNotIn('--nosignature', arch)

    def test_arch_archive_transactions_retry_bounded_without_bypassing_checks(self):
        import subprocess
        arch = self.jobs['arch']
        match = re.search(r'          retry_pacman\(\) \{\n.*?^          \}', arch, re.M | re.S)
        self.assertIsNotNone(match, 'Archived mirror downloads need bounded retries')
        function = '\n'.join(line[10:] for line in match[0].splitlines())
        # WHY execute the actual workflow function with a fake pacman: success after
        # a transient timeout must not hide persistent transport/signature failures.
        for failures, attempts, status in ((0, 1, 0), (2, 3, 0), (9, 3, 1)):
            with self.subTest(failures=failures):
                script = f'''attempts=0
pacman() {{ attempts=$((attempts + 1)); echo "pacman:$*"; (( attempts > {failures} )); }}
sleep() {{ echo "sleep:$*"; }}
{function}
retry_pacman -Syyuu --noconfirm
result=$?
echo "attempts:$attempts"
exit "$result"
'''
                result = subprocess.run(['bash', '-c', script], text=True, capture_output=True)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual(result.stdout.count('pacman:-Syyuu --noconfirm'), attempts)
                self.assertEqual(result.stdout.count('sleep:15'), attempts - 1)
                self.assertIn(f'attempts:{attempts}', result.stdout)
        self.assertIn('retry_pacman -Syyuu --noconfirm', arch)
        self.assertIn('retry_pacman -S --noconfirm --needed base-devel', arch)
        self.assertNotIn('--nodeps', arch)

    def test_fedora_resolves_systemd_conflict_before_build_dependencies(self):
        fedora = self.jobs['fedora']
        self.assertLess(fedora.index('dnf -y swap systemd-standalone-tmpfiles systemd'),
                        fedora.index('dnf -y builddep packaging/fedora/swipe-keyboard.spec'))

    def test_flatpak_runner_supports_inherited_wayland_permission(self):
        # WHY Ubuntu 26.04: its signed distribution tools support the unchanged
        # inherit-wayland-socket permission without depending on the extra PPA.
        flatpak = self.jobs['flatpak']
        self.assertIn('runs-on: ubuntu-26.04', flatpak)
        self.assertIn('sudo apt-get install -y flatpak flatpak-builder', flatpak)
        self.assertNotIn('add-apt-repository', flatpak)
        self.assertNotIn('ppa:flatpak/stable', flatpak)
        self.assertIn('dpkg --compare-versions', flatpak)
        self.assertIn('ge 1.16', flatpak)
        self.assertNotIn('--allow-unauthenticated', flatpak)

    def test_installed_flatpak_loader_uses_bundled_stock_layouts(self):
        build = (ROOT/'packaging/flatpak/build-swipe.sh').read_text()
        loader = next(line for line in build.splitlines()
                      if line.startswith('python3 scripts/check-stock-layout.py '))
        self.assertIn('--stock-layouts /app/share/plasma/keyboard/layouts', loader)
        self.assertIn('--runtime-root /app/share/swipe-keyboard', loader)

    def test_default_flatpak_lab_guard_still_exists(self):
        build = (ROOT/'packaging/flatpak/build.py').read_text()
        self.assertIn('owned guest for work item 168', build)
        self.assertIn("'QEMU'", build)
        self.assertIn('verify_runtime(', build)
        self.assertIn('--ci-manifest', self.jobs['flatpak'])
        self.assertNotIn('--no-gpg-verify', self.workflow)


if __name__ == '__main__':
    unittest.main()
