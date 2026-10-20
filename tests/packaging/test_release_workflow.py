"""Small wiring guards, supplementing behavioral validator tests and actionlint."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.workflow = (ROOT/'.github/workflows/packages.yml').read_text()
        # Job boundaries only; actionlint validates the actual YAML/expression schema.
        self.jobs = dict(re.findall(r'^  ([a-z][a-z-]*):\n(.*?)(?=^  [a-z][a-z-]*:|\Z)',
                                    self.workflow.split('jobs:\n', 1)[1], re.M | re.S))

    def test_real_producers_and_always_completion_gate(self):
        self.assertEqual(set(self.jobs), {'prepare', 'arch', 'fedora', 'flatpak', 'keyboard-ci', 'publish'})
        gate = self.jobs['keyboard-ci']
        self.assertIn('name: Keyboard CI', gate)
        self.assertIn('if: ${{ always() }}', gate)
        self.assertIn('needs: [prepare, arch, fedora, flatpak]', gate)
        self.assertIn('${{ toJSON(needs) }}', gate)
        self.assertIn('scripts/release.py gate', gate)
        for name, build in [('arch', 'makepkg --noconfirm'), ('fedora', 'rpmbuild -ba'), ('flatpak', 'packaging/flatpak/build.py')]:
            producer = self.jobs[name]
            self.assertIn(build, producer)
            self.assertIn('--target ' + name, producer)
            self.assertNotIn('continue-on-error', producer)
            self.assertNotRegex(producer, r'(?m)^    if:', name)

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
        self.assertNotIn('actions/cache', self.workflow)
        self.assertNotIn('0.2.', self.workflow)

    def test_prepare_shallows_dependencies_not_tag_ancestry(self):
        # Fetching full history recursively pulls enormous unused upstream histories.
        # Only the small root repository needs history for the trusted-tag check.
        prepare = self.jobs['prepare']
        self.assertIn('fetch-depth: 1', prepare)
        self.assertNotIn('fetch-depth: 0', prepare)
        self.assertIn('git fetch --unshallow --no-tags origin', prepare)
        self.assertIn('git merge-base --is-ancestor', prepare)

    def test_fedora_resolves_systemd_conflict_before_build_dependencies(self):
        fedora = self.jobs['fedora']
        self.assertLess(fedora.index('dnf -y swap systemd-standalone-tmpfiles systemd'),
                        fedora.index('dnf -y builddep packaging/fedora/swipe-keyboard.spec'))

    def test_flatpak_runner_supports_inherited_wayland_permission(self):
        # Ubuntu 24.04's stock Flatpak 1.14 cannot express our unchanged
        # inherit-wayland-socket permission; install signed upstream stable tools.
        flatpak = self.jobs['flatpak']
        self.assertIn('ppa:flatpak/stable', flatpak)
        self.assertIn('dpkg --compare-versions', flatpak)
        self.assertIn('ge 1.16', flatpak)
        self.assertNotIn('--allow-unauthenticated', flatpak)

    def test_default_flatpak_lab_guard_still_exists(self):
        build = (ROOT/'packaging/flatpak/build.py').read_text()
        self.assertIn('owned guest for work item 168', build)
        self.assertIn("'QEMU'", build)
        self.assertIn('verify_runtime(', build)
        self.assertIn('--ci-manifest', self.jobs['flatpak'])
        self.assertNotIn('--no-gpg-verify', self.workflow)


if __name__ == '__main__':
    unittest.main()
