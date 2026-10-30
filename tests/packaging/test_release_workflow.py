# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

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
        # WHY steam-deck.md too: the Flatpak-only download is fine print and lives there, not in the README.
        guide = (ROOT/'README.md').read_text() + (ROOT/'docs/steam-deck.md').read_text()
        # WHY: a complete upload is not enough if instructions still send Arch
        # or ARM users to an old release or omit their package altogether.
        for name, target in contract['targets'].items():
            if name == 'source':
                continue
            installer = target['files'][0].format(version=contract['version'], release=contract['release'])
            with self.subTest(target=name):
                self.assertIn(f'https://github.com/itsdestin/swipe-keyboard/releases/download/{tag}/{installer}', guide)
        self.assertTrue((ROOT/'docs/releases'/f'{tag}.md').is_file())
        # WHY (card #200): every download link must name this release; one left on an older version sends people
        # to old files. `python3 scripts/release.py readme` moves them all after a version change.
        self.assertEqual(set(re.findall(r'/releases/download/(v[^/]+)/', guide)), {tag})

    def test_real_producers_and_always_completion_gate(self):
        self.assertEqual(set(self.jobs), {'prepare', 'arch', 'arch-current', 'fedora', 'fedora-arm64', 'flatpak', 'steamos-helper', 'steamos', 'keyboard-ci', 'gaming-tests', 'publish', 'update-site-check', 'update-site'})
        gate = self.jobs['keyboard-ci']
        # WHY changed: the required check keeps the exact name "Keyboard CI" only for full runs; the
        # fast path reports under a different name and so can never satisfy the merge requirement.
        self.assertIn("name: ${{ needs.prepare.outputs.mode == 'fast' && 'Keyboard CI (fast path, not mergeable)' || 'Keyboard CI' }}", gate)
        self.assertIn('if: ${{ always() }}', gate)
        self.assertIn('needs: [prepare, arch, arch-current, update-site-check, fedora, fedora-arm64, flatpak, steamos, gaming-tests]', gate)
        self.assertIn('${{ toJSON(needs) }}', gate)
        self.assertIn('scripts/release.py gate', gate)
        for name, build in [('arch', 'makepkg --noconfirm'), ('fedora', 'rpmbuild -ba'), ('fedora-arm64', 'rpmbuild -bb'), ('flatpak', 'packaging/flatpak/build.py'), ('steamos', 'scripts/steamos_bundle.py assemble')]:
            producer = self.jobs[name]
            self.assertIn(build, producer)
            self.assertIn('--target ' + name, producer)
            self.assertNotIn('continue-on-error', producer)
            if name in ('arch',):
                # WHY changed (card #200): Arch runs in every mode, except a tag reusing main's run for its commit
                # and a documentation-only pull request.
                self.assertIn("    if: ${{ needs.prepare.outputs.reuse_run == '' && needs.prepare.outputs.mode != 'docs' }}", producer)
            else:
                # WHY changed from "no job-level if": these three are optional only in the fast path,
                # and the gate requires them to be 'skipped' there and 'success' everywhere else.
                self.assertIn("    if: ${{ needs.prepare.outputs.mode == 'full' && needs.prepare.outputs.reuse_run == '' }}", producer)

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
        self.assertNotIn('march=native', recipe)
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
        # WHY one: only the release uploader writes to the repository; the update site goes to Pages.
        self.assertEqual(self.workflow.count('contents: write'), 1)
        self.assertIn('contents: write', self.jobs['publish'])
        self.assertIn('needs: [keyboard-ci, gaming-tests]', self.jobs['publish'])
        self.assertIn('environment: keyboard-release', self.jobs['publish'])
        self.assertIn("github.event_name == 'push'", self.jobs['publish'])
        self.assertIn("github.repository == 'itsdestin/swipe-keyboard'", self.jobs['publish'])
        # WHY (v0.5.1-1, 2026-10-30): a tag reusing main's packages skips the builders; with only the implicit
        # success() GitHub then skipped publish too. Both publishing jobs must name the results they wait for.
        self.assertIn("!cancelled() && needs.keyboard-ci.result == 'success' && needs.gaming-tests.result == 'success'", self.jobs['publish'])
        self.assertIn("!cancelled() && needs.publish.result == 'success'", self.jobs['update-site'])
        # WHY changed (card #200, owner decision 2026-10-24): the only download from another run is the gate's, of
        # the main run prepare chose for this exact commit, in this repository.
        self.assertEqual(self.workflow.count('run-id:'), 2)
        self.assertEqual(self.jobs['keyboard-ci'].count('run-id: ${{ needs.prepare.outputs.reuse_run }}'), 2)
        self.assertEqual(self.workflow.count('repository: ${{ github.repository }}'), 2)
        self.assertEqual(len(re.findall(r'(?m)^ +repository:', self.workflow)), 2)
        # WHY changed from "no actions/cache at all": approved dependency-only caches now exist.
        # The release guarantees are pinned in test_dependency_caches_are_trusted_and_never_touch_releases.
        self.assertNotIn('actions/cache@', self.workflow)
        self.assertNotIn('0.2.', self.workflow)

    def test_gaming_tests_and_update_repo_wiring(self):
        # WHY: Gaming Mode tests used to run only in the daily canary (and failed there for want of a built daemon).
        # They now run on every pull request and push, and a release tag cannot publish while they fail.
        gaming = self.jobs['gaming-tests']
        self.assertIn('actions/setup-node@v4', gaming)  # the layer and helper tests drive Node
        for name in ('test_flatpak_gaming.py', 'test_gaming_setup.py', 'test_release_steamos.py'):
            self.assertIn(name, gaming)
        self.assertNotIn('continue-on-error', gaming)
        self.assertNotIn("test_[!d]", gaming)  # the old glob quietly depended on a file name's first letter
        # WHY by name: the tests that need the compiled daemon cannot run in this job (no Qt 6.8 or models), so they are
        # excluded explicitly. Which files those are is read from the tests themselves, so a new daemon-dependent test
        # that is not excluded (or is excluded and never wired into CTest) fails here, not in a run on GitHub.
        needs_daemon = sorted(f.name for f in (ROOT/'tests/gaming').glob('test_*.py')
                              if re.search(r'SWIPEKB_GAMINGD|import test_daemon', f.read_text()) and f.name != 'test_daemon.py') + ['test_daemon.py']
        self.assertEqual(sorted(needs_daemon), ['test_commit_e2e.py', 'test_daemon.py'])
        cmake = (ROOT/'gaming/daemon/CMakeLists.txt').read_text()
        for name in needs_daemon:
            self.assertIn(name, gaming, f'{name} must be excluded from gaming-tests by name')
            self.assertIn('tests/gaming/' + name, cmake, f'{name} must be registered with CTest, which the package builds run')
        self.assertIn('add_test(NAME gaming_daemon', cmake)
        self.assertIn('add_test(NAME gaming_commit_e2e', cmake)
        # The update site (plan 2026-10-23) is built inside the owner-approved publish job, so the signing key is only
        # ever read behind that approval, and every site step waits for the owner's switch.
        publish = self.jobs['publish']
        self.assertIn('environment: keyboard-release', publish)
        self.assertEqual(self.workflow.count('secrets.FLATPAK_REPO_GPG_KEY'), 1)
        self.assertIn('secrets.FLATPAK_REPO_GPG_KEY', publish)
        site_steps = publish[publish.index('scripts/ci-publish.py'):]
        self.assertIn('scripts/update_site.py build', site_steps)
        self.assertLess(publish.index('scripts/ci-publish.py'), publish.index('scripts/update_site.py build'))
        steps = site_steps.split('      - ')[1:]
        self.assertGreaterEqual(len(steps), 5)
        for step in steps:
            self.assertIn("if: ${{ vars.FLATPAK_REPO_PUBLISH == 'true' }}", step)
        self.assertIn('scripts/update_site.py verify --site site --keyring packaging/updates/update-key.gpg', site_steps)
        self.assertNotIn('--test-key', publish)
        # The rehearsal runs the same build and checks on every full non-tag build, with a throwaway key and no secret.
        rehearsal = self.jobs['update-site-check']
        self.assertIn("needs.prepare.outputs.mode == 'full' && !startsWith(github.ref, 'refs/tags/')", rehearsal)
        # WHY: it must not wait for "Keyboard CI" (that put it on the critical path); "Keyboard CI" waits for it instead.
        self.assertIn('needs: [prepare, arch, fedora, fedora-arm64, flatpak, steamos]', rehearsal)
        self.assertNotIn('needs: [prepare, keyboard-ci]', rehearsal)
        self.assertIn('--test-key', rehearsal)
        self.assertIn('scripts/update_site.py verify', rehearsal)
        self.assertNotIn('secrets.', rehearsal)
        self.assertNotIn('upload-pages-artifact', rehearsal)
        # Only the deploy job may write to Pages, and it never sees the key.
        deploy = self.jobs['update-site']
        self.assertIn("startsWith(github.ref, 'refs/tags/v')", deploy)
        self.assertIn("vars.FLATPAK_REPO_PUBLISH == 'true'", deploy)
        self.assertIn('needs: [publish]', deploy)
        self.assertIn('pages: write', deploy)
        self.assertNotIn('secrets.', deploy)
        self.assertNotIn('pull_request', deploy)

    def test_dependency_caches_are_trusted_and_never_touch_releases(self):
        import json
        # WHY: tag candidates never touch caches (they reuse main's tested files or rebuild cold), and only trusted events may write.
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
        save = flatpak[flatpak.index('Save compiler cache'):]
        self.assertIn("steps.flatpakbuild.outcome == 'success'", save.split('uses:')[0])

    def test_fast_path_is_arch_only_and_everything_else_needs_full(self):
        self.assertIn('types: [opened, synchronize, reopened, ready_for_review]', self.workflow)
        self.assertIn('full:', self.workflow.split('workflow_dispatch:', 1)[1].split('permissions:', 1)[0])
        self.assertIn('scripts/release.py mode', self.jobs['prepare'])
        self.assertIn('mode: ${{ steps.mode.outputs.mode }}', self.jobs['prepare'])
        for name in ('fedora', 'fedora-arm64', 'flatpak'):
            self.assertIn("needs.prepare.outputs.mode == 'full'", self.jobs[name].split('steps:')[0], name)
        self.assertNotIn('mode ==', self.jobs['arch'])
        # Publication is tag-only and tags are always full (run_mode), so the release set is unchanged.
        self.assertIn("startsWith(github.ref, 'refs/tags/v')", self.jobs['publish'])

    def test_tags_reuse_main_only_for_the_same_commit_and_still_hash_every_byte(self):
        # Card #200, owner decision 2026-10-24: a tag publishes main's already-tested files for its exact commit.
        prepare, gate = self.jobs['prepare'], self.jobs['keyboard-ci']
        self.assertIn("if: ${{ startsWith(github.ref, 'refs/tags/') }}", prepare.split('id: reuse', 1)[1].split('run:', 1)[0])
        self.assertIn('scripts/ci-reuse.py find', prepare)
        # The reuse lookup runs after the tag/version/ancestry checks, never before.
        self.assertLess(prepare.index('git merge-base --is-ancestor'), prepare.index('scripts/ci-reuse.py find'))
        # The gate swaps in the main run's producer results only on a tag whose own prepare succeeded, and the tag
        # gate still hashes the actual bytes (no --evidence-only on tags).
        self.assertIn('[[ "$GITHUB_REF" == refs/tags/* && "$PREPARE_RESULT" == success ]]', gate)
        self.assertIn('scripts/ci-reuse.py statuses --run "$REUSE_RUN"', gate)
        self.assertIn('if [[ "$GITHUB_REF" != refs/tags/* ]]; then flag=\'--evidence-only\'; fi', gate)
        # Read access to runs is limited to the two jobs that need it.
        self.assertEqual(self.workflow.count('actions: read'), 2)
        self.assertIn('actions: read', prepare)
        self.assertIn('actions: read', gate)

    def test_documentation_only_pull_requests_build_nothing_but_still_check_the_words(self):
        # Card #200: a README-only pull request waited ~35 minutes for packages it does not change.
        prepare, gate = self.jobs['prepare'], self.jobs['keyboard-ci']
        self.assertLess(prepare.index('git diff --name-only "$BASE_SHA" "$GITHUB_SHA"'), prepare.index('scripts/release.py mode --changed'))
        self.assertIn("if: ${{ github.event_name == 'pull_request' }}", prepare.split('List the files a pull request changes', 1)[1].split('run:', 1)[0])
        docs = prepare.split('Documentation checks', 1)[1].split('- name:', 1)[0]
        self.assertIn("if: ${{ steps.mode.outputs.mode == 'docs' }}", docs)
        for name in ('test_release.py', 'test_publish.py', 'test_release_workflow.py', 'test_release_steamos.py', 'test_licensing.py'):
            self.assertIn(name, docs)
        self.assertIn('scripts/ci-publish.py check-links', docs)
        # Nothing is prepared or uploaded, and Arch (which runs in every other mode) is skipped.
        self.assertEqual(prepare.count("if: ${{ steps.reuse.outputs.run == '' && steps.mode.outputs.mode != 'docs' }}"), 5)
        self.assertIn("    if: ${{ needs.prepare.outputs.reuse_run == '' && needs.prepare.outputs.mode != 'docs' }}", self.jobs['arch'])
        # The gate learns the mode from prepare; release.py accepts it only on pull requests.
        self.assertIn('PREPARED_MODE: ${{ needs.prepare.outputs.mode }}', gate)

    def test_only_pull_requests_cancel_superseded_runs(self):
        self.assertIn("cancel-in-progress: ${{ github.event_name == 'pull_request' }}", self.workflow)

    def test_prepare_frees_space_in_the_background_but_waits_before_using_it(self):
        prepare = self.jobs['prepare']
        self.assertIn("touch /tmp/space-freed' >/dev/null 2>&1 &", prepare)
        self.assertLess(prepare.index('touch /tmp/space-freed'), prepare.index('actions/checkout@v4'))
        self.assertLess(prepare.index('until [ -e /tmp/space-freed ]'), prepare.index('scripts/ci-source.py prepared'))

    def test_gate_hashes_real_bytes_on_tags_and_uses_evidence_elsewhere(self):
        gate = self.jobs['keyboard-ci']
        self.assertEqual(gate.count("if: ${{ startsWith(github.ref, 'refs/tags/') }}"), 1)  # verified-release upload
        # Files download: this run's on a cold tag, the reused main run's otherwise (that one is only ever set on tags).
        self.assertEqual(gate.count("if: ${{ startsWith(github.ref, 'refs/tags/') && needs.prepare.outputs.reuse_run == '' }}"), 1)
        self.assertIn('--evidence-only', gate)
        self.assertIn('refs/tags/*', gate)
        self.assertIn('PR_DRAFT', gate)
        self.assertIn('FULL_INPUT', gate)

    def test_all_compiles_may_use_ccache_only_through_the_policy_gate(self):
        # WHY: engine and product objects are now cached on non-release runs (content-hashed by ccache).
        # Tags never get CCACHE_ON/CCACHE_DIR, and platform stores keep their own labels.
        for name in ('arch', 'fedora', 'fedora-arm64'):
            job = self.jobs[name]
            self.assertIn('CMAKE_CXX_COMPILER_LAUNCHER=ccache', job, name)
            self.assertIn('CCACHE_ON: ${{ steps.cachepolicy.outputs.restore }}', job, name)
            self.assertIn('$GITHUB_SHA', job, name)
        self.assertIn('policy', self.jobs['flatpak'])

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

    def test_steamos_download_is_built_from_the_released_flatpak_and_a_fresh_helper_build(self):
        # WHY split (was one job needing flatpak): the helper builds in parallel with the main Flatpak in
        # `steamos-helper`; the slim `steamos` job only assembles, once both exist. Every check is kept.
        helper = self.jobs['steamos-helper']
        steamos = self.jobs['steamos']
        self.assertIn('needs: prepare\n', helper)
        self.assertNotIn('flatpak-files', helper)
        self.assertIn('org.itsdestin.SwipeKeyboard.GamingHelper.json', helper)
        self.assertIn('runtime-lock.json', helper)
        self.assertIn('runs-on: ubuntu-26.04', helper)
        # The helper is installed from its own bundle and checked, as the main Flatpak is.
        self.assertIn('flatpak install --user', helper)
        self.assertIn('--show-ref org.itsdestin.SwipeKeyboard.GamingHelper', helper)
        self.assertIn('shared=network;', helper)
        self.assertIn('name: steamos-helper-bundle', helper)
        self.assertNotIn('steamos-helper-files', helper)
        # The download embeds the very bundle the flatpak job released, not a rebuild.
        self.assertIn('needs: [prepare, flatpak, steamos-helper]', steamos)
        self.assertIn('flatpak-files', steamos)
        self.assertIn('name: steamos-helper-bundle', steamos)
        self.assertIn('scripts/steamos_bundle.py assemble', steamos)
        self.assertIn('scripts/release.py record --target steamos', steamos)
        self.assertIn('steamos-files', steamos)
        self.assertIn('steamos-evidence', steamos)
        for job in (helper, steamos):
            self.assertNotIn('continue-on-error', job)
            self.assertIn("    if: ${{ needs.prepare.outputs.mode == 'full' && needs.prepare.outputs.reuse_run == '' }}", job)
        # The gate counts steamos as a producer; the helper is covered through it (a failed helper skips steamos).
        gate = self.jobs['keyboard-ci']
        self.assertNotIn('steamos-helper', gate)

    def test_current_arch_retest_is_its_own_job_that_the_gate_requires(self):
        # WHY: the retest used to be the last steps of the arch job, on the critical path. It now runs beside the other
        # jobs, from the exact package bytes the arch job saved, and "Keyboard CI" fails unless it passed.
        arch, current = self.jobs['arch'], self.jobs['arch-current']
        self.assertNotIn('Test identical older-built bytes on current Arch', arch)
        self.assertIn('name: arch-retest-input', arch)
        self.assertIn('Test identical older-built bytes on current Arch', current)
        self.assertIn('needs: [prepare, arch]', current)
        self.assertIn("    if: ${{ needs.prepare.outputs.reuse_run == '' && needs.prepare.outputs.mode != 'docs' }}", current)
        self.assertIn('sha256sum -c dist/baseline-package.sha256', current)
        self.assertIn('swipe-keyboard check', current)
        self.assertNotIn('continue-on-error', current)

    def test_gate_expectations_match_the_conditions_of_the_jobs_they_judge(self):
        # WHY: the gate decides "should this job have run?" with its own shell test. If a job's `if:` changes and the
        # gate's copy does not, a skipped check could pass silently (or a correct skip could fail the gate).
        gate = self.jobs['keyboard-ci']
        current_if = re.search(r'(?m)^    if: (.*)$', self.jobs['arch-current']).group(1)
        self.assertEqual(current_if, "${{ needs.prepare.outputs.reuse_run == '' && needs.prepare.outputs.mode != 'docs' }}")
        self.assertIn('if [[ -z "$REUSE_RUN" && "$PREPARED_MODE" != docs ]]; then want_arch=success; fi', gate)
        site_if = re.search(r'(?m)^    if: (.*)$', self.jobs['update-site-check']).group(1)
        self.assertEqual(site_if, "${{ needs.prepare.outputs.mode == 'full' && !startsWith(github.ref, 'refs/tags/') }}")
        self.assertIn('if [[ "$PREPARED_MODE" == full && "$GITHUB_REF" != refs/tags/* ]]; then want_site=success; fi', gate)
        # The retest's inputs are checked against hashes that did not travel in the same artifact.
        current = self.jobs['arch-current']
        self.assertIn('sha256sum < "release/swipe-keyboard-$PACKAGE_VERSION-x86_64.pkg.tar.zst"', current)
        self.assertIn('needs.arch.outputs.stocklayout_sha', current)

    def test_gaming_tests_run_in_every_mode_and_block_keyboard_ci_without_being_a_producer(self):
        # WHY: ~1 minute, no Qt or Flatpak, and it catches Gaming Mode regressions on drafts too. It is not a
        # producer: it writes no release files and is not in release.PRODUCERS. The gate still waits on it, so a
        # green "Keyboard CI" can never sit next to failing Gaming Mode tests.
        gaming = self.jobs['gaming-tests']
        self.assertNotRegex(gaming, r'(?m)^    if:')
        self.assertIn('test_switch_watch.py', gaming)
        gate = self.jobs['keyboard-ci']
        self.assertIn('flatpak, steamos, gaming-tests]', gate.split('steps:')[0])
        # The current-Arch retest and the update-site rehearsal gate merges through "Keyboard CI", not as producers.
        self.assertIn('SITE_CHECK_RESULT: ${{ needs.update-site-check.result }}', gate)
        self.assertIn('CURRENT_ARCH_RESULT: ${{ needs.arch-current.result }}', gate)
        self.assertIn('GAMING_RESULT: ${{ needs.gaming-tests.result }}', gate)
        self.assertIn('if [[ "$GAMING_RESULT" != success ]]', gate)
        self.assertIn('''del(."gaming-tests", ."arch-current", ."update-site-check")''', gate)
        self.assertIn('--statuses "$statuses"', gate)
        self.assertIn('needs: [keyboard-ci, gaming-tests]', self.jobs['publish'])

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


class DeckRedirectTests(unittest.TestCase):
    """The Steam Deck's "this has moved" note for the OpenSwipe rename (card #501)."""

    def load(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('update_site', ROOT / 'scripts/update_site.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_it_points_decks_at_the_openswipe_site(self):
        # WHY exactly this address: once a Deck follows the note it never asks the old site again, so a typo would
        # strand every Deck. It must be the renamed project's own site (on from 0.5.1-1; emptied after the rename).
        self.assertEqual(self.load().flatpak_redirect(), 'https://itsdestin.github.io/openswipe/repo/')

    def test_only_one_https_address_is_accepted(self):
        import tempfile
        module = self.load()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'r.txt'
            path.write_text('# note\nhttps://itsdestin.github.io/openswipe/repo/\n')
            self.assertEqual(module.flatpak_redirect(path), 'https://itsdestin.github.io/openswipe/repo/')
            for bad in ('http://example.org/\n', 'https://a/\nhttps://b/\n'):
                path.write_text(bad)
                with self.assertRaises(SystemExit):
                    module.flatpak_redirect(path)

    def test_the_check_copy_really_drops_the_moved_note(self):
        # WHY: Flatpak also saves the note in the repository's config; a rebuilt summary brought it back (CI, 2026-10-25)
        import shutil, subprocess, tempfile
        if not (shutil.which('ostree') and shutil.which('flatpak')):
            self.skipTest('needs ostree and flatpak')
        module = self.load()
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / 'repo'
            subprocess.run(['ostree', 'init', '--mode=archive-z2', f'--repo={repo}'], check=True)
            subprocess.run(['flatpak', 'build-update-repo', '--redirect-url=https://example.org/moved/', str(repo)],
                           check=True, capture_output=True)
            view = lambda r: subprocess.run(['ostree', 'summary', '--view', f'--repo={r}'], check=True,
                                            capture_output=True, text=True).stdout
            self.assertIn('xa.redirect-url', view(repo))
            copy = module.copy_without_redirect(repo, Path(tmp) / 'copy')
            self.assertNotIn('xa.redirect-url', view(copy))
            self.assertIn('xa.redirect-url', view(repo))   # the real site is untouched
