# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""One tracking issue labelled gaming-canary: open it on the first failure, add to it afterwards."""
import json
import unittest

import support  # noqa: F401  (puts tools/steam-canary on the import path)
import tracker


class FakeGh:
    def __init__(self, open_issues=()):
        self.open_issues = list(open_issues)
        self.calls = []

    def __call__(self, args, input=None):
        self.calls.append((list(args), input))
        if args[:2] == ['issue', 'list']:
            return json.dumps(self.open_issues)
        return ''


TITLE = 'Gaming canary: Steam UI no longer matches the contract'


class Tracker(unittest.TestCase):
    def test_first_failure_creates_the_labelled_issue(self):
        gh = FakeGh()
        tracker.sync_issue(gh, TITLE, 'body text')
        verbs = [c[0][:2] for c in gh.calls]
        self.assertIn(['issue', 'create'], verbs)
        create = next(c for c in gh.calls if c[0][:2] == ['issue', 'create'])
        self.assertIn('gaming-canary', create[0])
        self.assertEqual(create[1], 'body text')
        self.assertIn(['label', 'create'], verbs, 'label is created if it does not exist yet')

    def test_later_failure_comments_on_the_existing_issue_instead_of_opening_another(self):
        gh = FakeGh([{'number': 7, 'title': TITLE}])
        tracker.sync_issue(gh, TITLE, 'newer body')
        self.assertFalse(any(c[0][:2] == ['issue', 'create'] for c in gh.calls))
        comment = next(c for c in gh.calls if c[0][:2] == ['issue', 'comment'])
        self.assertIn('7', comment[0])
        self.assertEqual(comment[1], 'newer body')

    def test_a_different_open_issue_with_the_label_is_not_mistaken_for_ours(self):
        gh = FakeGh([{'number': 3, 'title': 'Gaming canary: no run for 3 days'}])
        tracker.sync_issue(gh, TITLE, 'b')
        self.assertTrue(any(c[0][:2] == ['issue', 'create'] for c in gh.calls))

    def test_body_goes_through_stdin_not_the_command_line(self):
        # WHY: the body can contain text taken from Valve's files; never put it in argv.
        gh = FakeGh()
        tracker.sync_issue(gh, TITLE, 'weird `$(rm -rf /)` body')
        for args, _ in gh.calls:
            self.assertFalse(any('rm -rf' in a for a in args))

    def test_body_is_truncated_to_githubs_limit(self):
        gh = FakeGh()
        tracker.sync_issue(gh, TITLE, 'x' * 100000)
        create = next(c for c in gh.calls if c[0][:2] == ['issue', 'create'])
        self.assertLess(len(create[1]), 65536)


if __name__ == '__main__':
    unittest.main()
