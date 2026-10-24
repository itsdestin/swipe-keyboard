# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Which main run a release tag may reuse (card #200, owner decision 2026-10-24)."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ci_reuse', ROOT/'scripts/ci-reuse.py')
reuse = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reuse)

SHA, REPO = 'a'*40, 'itsdestin/swipe-keyboard'


def run(**over):
    base = {'id': 7, 'run_number': 7, 'head_sha': SHA, 'head_branch': 'main', 'event': 'push',
            'path': '.github/workflows/packages.yml', 'status': 'completed', 'conclusion': 'success',
            'repository': {'full_name': REPO}, 'head_repository': {'full_name': REPO}}
    base.update(over)
    return base


class Choose(unittest.TestCase):
    def test_successful_main_push_run_of_the_same_commit_is_reused(self):
        self.assertEqual(reuse.choose([run()], SHA, REPO), ('reuse', run()))

    def test_anything_else_is_never_reused(self):
        for bad in (dict(head_sha='b'*40), dict(head_branch='feature'), dict(event='pull_request'),
                    dict(event='workflow_dispatch'), dict(path='.github/workflows/other.yml'),
                    dict(repository={'full_name': 'someone/fork'}), dict(head_repository={'full_name': 'someone/fork'}),
                    dict(head_repository=None)):
            with self.subTest(bad=bad):
                self.assertEqual(reuse.choose([run(**bad)], SHA, REPO)[0], 'cold')

    def test_failed_run_means_cold_build(self):
        self.assertEqual(reuse.choose([run(conclusion='failure')], SHA, REPO)[0], 'cold')
        self.assertEqual(reuse.choose([run(conclusion='cancelled')], SHA, REPO)[0], 'cold')

    def test_running_main_run_is_waited_for(self):
        self.assertEqual(reuse.choose([run(status='in_progress', conclusion=None)], SHA, REPO)[0], 'wait')

    def test_an_earlier_success_wins_over_a_later_failed_rerun(self):
        runs = [run(id=8, run_number=8, conclusion='failure'), run()]
        self.assertEqual(reuse.choose(runs, SHA, REPO), ('reuse', run()))


class Statuses(unittest.TestCase):
    JOBS = [{'name': n, 'conclusion': 'success'} for n in reuse.PRODUCERS] + [
        {'name': 'gaming-tests', 'conclusion': 'success'}, {'name': 'Keyboard CI', 'conclusion': 'success'}]

    def test_exactly_the_producers_in_gate_shape(self):
        self.assertEqual(reuse.producer_statuses(self.JOBS), {n: {'result': 'success'} for n in reuse.PRODUCERS})

    def test_missing_or_duplicated_producer_is_refused(self):
        with self.assertRaises(ValueError):
            reuse.producer_statuses([j for j in self.JOBS if j['name'] != 'flatpak'])
        with self.assertRaises(ValueError):
            reuse.producer_statuses(self.JOBS + [{'name': 'arch', 'conclusion': 'success'}])

    def test_producer_list_matches_the_gate(self):
        spec = importlib.util.spec_from_file_location('release', ROOT/'scripts/release.py')
        release = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(release)
        self.assertEqual(reuse.PRODUCERS, release.PRODUCERS)


class Find(unittest.TestCase):
    def test_not_a_tag_never_reuses(self):
        self.assertEqual(reuse.find({'GITHUB_SHA': SHA, 'GITHUB_REPOSITORY': REPO, 'GITHUB_REF': 'refs/heads/main'})[0], '')


if __name__ == '__main__':
    unittest.main()
