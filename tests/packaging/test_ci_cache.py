"""Dependency-cache rules: who may read/write, what the key covers, what a restore must prove."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ci_cache', ROOT/'scripts/ci-cache.py')
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)
memspec = importlib.util.spec_from_file_location('ci_memwatch', ROOT/'scripts/ci-memwatch.py')
memwatch = importlib.util.module_from_spec(memspec)
memspec.loader.exec_module(memwatch)

REPO = 'itsdestin/swipe-keyboard'


def env(event, ref, repo=REPO, fork=''):
    return {'GITHUB_EVENT_NAME': event, 'GITHUB_REF': ref, 'GITHUB_REPOSITORY': repo, 'PR_FROM_FORK': fork}


class PolicyTests(unittest.TestCase):
    def test_release_tags_never_read_or_write_a_cache(self):
        # WHY: release policy requires every tag candidate to rebuild everything cold.
        for event in ('push', 'workflow_dispatch'):
            self.assertEqual(cache.policy(env(event, 'refs/tags/v0.3.1')), (False, False))

    def test_only_push_and_dispatch_on_a_branch_write(self):
        self.assertEqual(cache.policy(env('push', 'refs/heads/main')), (True, True))
        self.assertEqual(cache.policy(env('workflow_dispatch', 'refs/heads/feature/ci-speed')), (True, True))

    def test_pull_requests_read_but_never_write(self):
        self.assertEqual(cache.policy(env('pull_request', 'refs/pull/5/merge', fork='false')), (True, False))

    def test_fork_pull_requests_and_other_repositories_use_nothing(self):
        self.assertEqual(cache.policy(env('pull_request', 'refs/pull/5/merge', fork='true')), (False, False))
        self.assertEqual(cache.policy(env('pull_request', 'refs/pull/5/merge', fork='')), (False, False))
        self.assertEqual(cache.policy(env('push', 'refs/heads/main', repo='someone/else')), (False, False))

    def test_unknown_events_use_nothing(self):
        self.assertEqual(cache.policy(env('schedule', 'refs/heads/main')), (False, False))


class KeyTests(unittest.TestCase):
    def tree(self, directory, **changes):
        root = Path(directory)
        for pattern in cache.KEY_FILES['flatpak-stages']:
            path = root/pattern
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(changes.get(pattern, 'base'))
        return root

    def test_key_is_stable_and_changes_with_any_covered_file_or_salt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.tree(directory)
            first = cache.cache_key('flatpak-stages', root, 'salt')
            self.assertEqual(first, cache.cache_key('flatpak-stages', root, 'salt'))
            self.assertNotEqual(first, cache.cache_key('flatpak-stages', root, 'other'))
            for pattern in cache.KEY_FILES['flatpak-stages']:
                with self.subTest(file=pattern):
                    (root/pattern).write_text('changed')
                    self.assertNotEqual(first, cache.cache_key('flatpak-stages', root, 'salt'))
                    (root/pattern).write_text('base')
            self.assertEqual(first, cache.cache_key('flatpak-stages', root, 'salt'))

    def test_a_missing_covered_file_is_an_error_not_a_silent_smaller_key(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.tree(directory)
            (root/cache.KEY_FILES['flatpak-stages'][0]).unlink()
            with self.assertRaises(FileNotFoundError):
                cache.cache_key('flatpak-stages', root, 'salt')

    def test_file_renames_change_the_key(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            ra, rb = Path(a), Path(b)
            (ra/'x').write_text('1'); (rb/'y').write_text('1')
            self.assertNotEqual(cache.digest_files(ra, ['x'], ''), cache.digest_files(rb, ['y'], ''))

    def test_every_key_set_exists_in_this_repository(self):
        # WHY: a typo here would silently stop covering a build input.
        for name in cache.KEY_FILES:
            with self.subTest(name=name):
                self.assertRegex(cache.cache_key(name, ROOT, 'x'), r'^' + name + r'-v[0-9]+-[0-9a-f]{32}$')

    def test_platform_labels_keep_compiled_caches_apart(self):
        arch = cache.cache_key('native-ccache', ROOT, 'x', 'arch')
        fedora = cache.cache_key('native-ccache', ROOT, 'x', 'fedora')
        self.assertTrue(arch.startswith('native-ccache-arch-v1-'))
        self.assertFalse(fedora.startswith(cache.key_prefix('native-ccache', 'arch')))
        self.assertFalse(fedora.startswith(cache.key_prefix('native-ccache', 'fedora-arm64')))

    def test_key_sets_cover_the_inputs_that_change_dependency_output(self):
        stages = ' '.join(cache.KEY_FILES['flatpak-stages'])
        for needed in ('org.itsdestin.SwipeKeyboard.json', 'runtime-lock.json', 'plasma-prediction-policy.patch', 'build.py'):
            self.assertIn(needed, stages)
        for name in ('flatpak-ccache', 'native-ccache'):
            joined = ' '.join(cache.KEY_FILES[name])
            for needed in ('apply-build-patches.py', 'provision-build-deps.py', 'swipe-metadata.patch', 'requirements'):
                self.assertIn(needed, joined)


class FlatpakStateTests(unittest.TestCase):
    def make_state(self, directory):
        state = Path(directory)/'.flatpak-builder'
        (state/'cache').mkdir(parents=True)
        (state/'cache'/'object').write_bytes(b'stage bytes')
        # Product and scratch state must never be packed.
        (state/'build').mkdir()
        (state/'build'/'product').write_bytes(b'product output')
        (state/'ccache').mkdir()
        (state/'ccache'/'x').write_bytes(b'ccache is cached separately')
        return state

    def test_round_trip_keeps_only_the_stage_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            state = self.make_state(directory)
            archive = Path(directory)/'state.tar.zst'
            cache.pack_state(state, archive, 'identity-1')
            restored = Path(directory)/'restored'/'.flatpak-builder'
            self.assertTrue(cache.unpack_state(archive, restored, 'identity-1', fsck=lambda repo: True))
            self.assertEqual((restored/'cache'/'object').read_bytes(), b'stage bytes')
            self.assertFalse((restored/'build').exists())
            self.assertFalse((restored/'ccache').exists())

    def test_relative_paths_work_as_in_the_workflow(self):
        # WHY (run 39827309452): the workflow passes relative paths; GNU tar's second -C is relative
        # to the first, so an unresolved state path pointed inside the scratch directory.
        import os
        with tempfile.TemporaryDirectory() as directory:
            self.make_state(directory)
            previous = os.getcwd()
            os.chdir(directory)
            try:
                cache.pack_state('.flatpak-builder', 'state.tar.zst', 'identity-1')
                self.assertTrue(cache.unpack_state('state.tar.zst', 'restored/.flatpak-builder', 'identity-1', fsck=lambda repo: True))
            finally:
                os.chdir(previous)
            self.assertEqual((Path(directory)/'restored/.flatpak-builder/cache/object').read_bytes(), b'stage bytes')

    def test_identity_mismatch_is_discarded(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory)/'state.tar.zst'
            cache.pack_state(self.make_state(directory), archive, 'identity-1')
            restored = Path(directory)/'restored'/'.flatpak-builder'
            self.assertFalse(cache.unpack_state(archive, restored, 'identity-2', fsck=lambda repo: True))
            self.assertFalse(restored.exists())

    def test_failed_integrity_check_is_discarded(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory)/'state.tar.zst'
            cache.pack_state(self.make_state(directory), archive, 'identity-1')
            restored = Path(directory)/'restored'/'.flatpak-builder'
            self.assertFalse(cache.unpack_state(archive, restored, 'identity-1', fsck=lambda repo: False))
            self.assertFalse(restored.exists())

    def test_corrupt_or_hostile_archives_are_discarded_not_extracted_outside(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            garbage = root/'garbage.tar.zst'
            garbage.write_bytes(b'not an archive')
            self.assertFalse(cache.unpack_state(garbage, root/'a'/'.flatpak-builder', 'i', fsck=lambda repo: True))
            hostile = root/'hostile.tar'
            (root/'payload').write_text('x')
            subprocess.run(['tar', '-C', str(root), '-cf', str(hostile), '--transform', 's,^payload,../escape,', 'payload'], check=True)
            self.assertFalse(cache.unpack_state(hostile, root/'b'/'.flatpak-builder', 'i', fsck=lambda repo: True))
            self.assertFalse((root/'escape').exists())
            self.assertFalse((root/'b'/'.flatpak-builder').exists())


class MemoryTests(unittest.TestCase):
    def test_summary_reports_peak_used_and_lowest_available(self):
        lines = ['100 1000 15000', '105 4000 12000', '110 2500 13500']
        summary = memwatch.summarize(lines)
        self.assertEqual(summary['peak_used_kib'], 4000)
        self.assertEqual(summary['min_available_kib'], 12000)
        self.assertEqual(summary['samples'], 3)

    def test_empty_samples_are_reported_not_hidden(self):
        self.assertEqual(memwatch.summarize([])['samples'], 0)


if __name__ == '__main__':
    unittest.main()
