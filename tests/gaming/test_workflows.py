"""Text-level guards on the two workflow files (no YAML library in the standard library)."""
import re
import unittest

from support import ROOT

CANARY = (ROOT / '.github/workflows/steam-canary.yml').read_text()
WATCHDOG = (ROOT / '.github/workflows/steam-canary-watchdog.yml').read_text()


def run_blocks(text):
    """Every 'run:' script body (the part where a pasted expression becomes shell code)."""
    out, lines, i = [], text.splitlines(), 0
    while i < len(lines):
        m = re.match(r'^(\s*)(?:- )?run:\s*(.*)$', lines[i])
        if m:
            indent, first = len(m.group(1)), m.group(2)
            body = [first] if first and first != '|' else []
            i += 1
            while i < len(lines) and (not lines[i].strip() or len(lines[i]) - len(lines[i].lstrip()) > indent):
                body.append(lines[i])
                i += 1
            out.append('\n'.join(body))
        else:
            i += 1
    return out


class Workflows(unittest.TestCase):
    def test_canary_runs_daily_and_on_demand(self):
        self.assertRegex(CANARY, r"cron: '\d+ \d+ \* \* \*'")
        self.assertIn('workflow_dispatch:', CANARY)

    def test_canary_runs_its_own_tests_before_trusting_itself(self):
        self.assertLess(CANARY.index('unittest discover -s tests/gaming'), CANARY.index('canary.py run'))

    def test_channels_come_from_the_contract_not_the_workflow(self):
        for name in ('steam_client_ubuntu12', 'steamdeck_stable', 'publicbeta'):
            self.assertNotIn(name, CANARY)
        self.assertIn('gaming/contract/gaming-contract.json', CANARY)

    def test_workflow_inputs_are_never_spliced_into_shell_text(self):
        for text in (CANARY, WATCHDOG):
            for block in run_blocks(text):
                self.assertNotIn('${{', block.replace('${{ github', 'X'), block)

    def test_least_privilege_and_no_third_party_actions(self):
        self.assertIn('issues: write', CANARY)
        self.assertIn('contents: read', CANARY.split('jobs:')[0], 'default token is read-only')
        for text in (CANARY, WATCHDOG):
            for use in re.findall(r'uses:\s*(\S+)', text):
                self.assertRegex(use, r'^actions/(checkout|setup-python|setup-node)@v\d+$')

    def test_failed_canary_opens_the_tracking_issue_and_fails_the_run(self):
        self.assertIn('canary.py issue --kind red', CANARY)
        self.assertRegex(CANARY, r"Fail the run if the canary failed[\s\S]*exit 1")

    def test_ledger_goes_to_its_own_branch(self):
        self.assertIn('canary-ledger', CANARY)
        self.assertIn('git -c http.extraheader', CANARY)
        self.assertNotIn('git push -f', CANARY)

    def test_watchdog_runs_on_a_different_clock_and_accepts_an_injected_date(self):
        hour = lambda t: re.search(r"cron: '(\d+) (\d+)", t).groups()
        self.assertNotEqual(hour(CANARY), hour(WATCHDOG))
        self.assertIn('--now', WATCHDOG)
        self.assertIn('canary.py issue --kind dead', WATCHDOG)


if __name__ == '__main__':
    unittest.main()
