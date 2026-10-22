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
            if name == 'steamos':
                # TODO(first release that contains the steamos target): the README's Gaming Mode section (M10) is
                # written and marked preview, but deliberately has no download link yet because the only
                # published tag (v0.3.1-1) has no steamos asset and assets are never replaced. In the change that
                # bumps the version (release-targets.json + recipes), delete this exemption and add the link
                # .../releases/download/<tag>/swipe-keyboard-<version>-steamos-x86_64.tar.gz to the README.
                continue
            installer = target['files'][0].format(version=contract['version'], release=contract['release'])
            with self.subTest(target=name):
                self.assertIn(f'https://github.com/itsdestin/swipe-keyboard/releases/download/{tag}/{installer}', guide)
        self.assertTrue((ROOT/'docs/releases'/f'{tag}.md').is_file())

    def test_real_producers_and_always_completion_gate(self):
        self.assertEqual(set(self.jobs), {'prepare', 'arch', 'fedora', 'fedora-arm64', 'flatpak', 'steamos', 'keyboard-ci', 'gaming-tests', 'publish', 'flatpak-repo'})
        gate = self.jobs['keyboard-ci']
        self.assertIn('name: Keyboard CI', gate)
        self.assertIn('if: ${{ always() }}', gate)
        self.assertIn('needs: [prepare, arch, fedora, fedora-arm64, flatpak, steamos]', gate)
        self.assertIn('${{ toJSON(needs) }}', gate)
        self.assertIn('scripts/release.py gate', gate)
        for name, build in [('arch', 'makepkg --noconfirm'), ('fedora', 'rpmbuild -ba'), ('fedora-arm64', 'rpmbuild -bb'), ('flatpak', 'packaging/flatpak/build.py'), ('steamos', 'scripts/steamos_bundle.py assemble')]:
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
        # WHY two: the release uploader and the update-repository publisher (the latter writes only the
        # flatpak-repo branch and is gated off by default; see test_gaming_tests_and_update_repo_wiring).
        self.assertEqual(self.workflow.count('contents: write'), 2)
        self.assertIn('contents: write', self.jobs['publish'])
        self.assertIn('contents: write', self.jobs['flatpak-repo'])
        self.assertIn('needs: [keyboard-ci, gaming-tests]', self.jobs['publish'])
        self.assertIn('environment: keyboard-release', self.jobs['publish'])
        self.assertIn("github.event_name == 'push'", self.jobs['publish'])
        self.assertIn("github.repository == 'itsdestin/swipe-keyboard'", self.jobs['publish'])
        self.assertNotIn('run-id:', self.workflow)
        self.assertNotIn('repository:', self.workflow)
        self.assertNotIn('actions/cache', self.workflow)
        self.assertNotIn('0.2.', self.workflow)

    def test_gaming_tests_and_update_repo_wiring(self):
        # WHY: Gaming Mode tests used to run only in the daily canary (and failed there for want of a built daemon).
        # They now run on every pull request and push, and a release tag cannot publish while they fail.
        gaming = self.jobs['gaming-tests']
        self.assertIn('actions/setup-node@v4', gaming)  # the layer and helper tests drive Node
        self.assertIn("-p 'test_[!d]*.py'", gaming)  # test_daemon.py needs the built daemon and runs under CTest
        for name in ('test_flatpak_gaming.py', 'test_gaming_setup.py', 'test_release_steamos.py'):
            self.assertIn(name, gaming)
        self.assertNotIn('continue-on-error', gaming)
        # WHY: the daemon test is wired through CTest in the native package builds, never by name here.
        self.assertIn('gaming_daemon', (ROOT/'gaming/daemon/CMakeLists.txt').read_text())
        # The update repository can never run on a pull request, a branch or without the owner's switch.
        repo = self.jobs['flatpak-repo']
        self.assertIn("startsWith(github.ref, 'refs/tags/v')", repo)
        self.assertIn("vars.FLATPAK_REPO_PUBLISH == 'true'", repo)
        self.assertIn('needs: [publish]', repo)
        self.assertIn('environment: keyboard-release', repo)
        self.assertIn('secrets.FLATPAK_REPO_GPG_KEY', repo)
        self.assertNotIn('pull_request', repo)

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

    def test_steamos_download_is_built_from_the_released_flatpak_and_a_fresh_helper_build(self):
        steamos = self.jobs['steamos']
        # WHY needs flatpak: the download embeds the very bundle the flatpak job released, not a rebuild.
        self.assertIn('needs: [prepare, flatpak]', steamos)
        self.assertIn('flatpak-files', steamos)
        self.assertIn('org.itsdestin.SwipeKeyboard.GamingHelper.json', steamos)
        self.assertIn('scripts/steamos_bundle.py assemble', steamos)
        self.assertIn('scripts/release.py record --target steamos', steamos)
        self.assertIn('steamos-files', steamos)
        self.assertIn('steamos-evidence', steamos)
        # WHY: the helper is installed from its own bundle and checked, as the main Flatpak is.
        self.assertIn('flatpak install --user', steamos)
        self.assertIn('--show-ref org.itsdestin.SwipeKeyboard.GamingHelper', steamos)
        self.assertNotIn('continue-on-error', steamos)
        self.assertNotRegex(steamos, r'(?m)^    if:')
        self.assertIn('runs-on: ubuntu-26.04', steamos)
        self.assertIn('runtime-lock.json', steamos)

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
