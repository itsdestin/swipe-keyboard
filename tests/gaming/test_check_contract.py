"""The contract engine: good bundle passes; each negative control turns it red."""
import copy
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from support import FIXTURES, ROOT
import check_contract as cc

CONTRACT_PATH = ROOT / 'gaming/contract/gaming-contract.json'
SP = 'steamui/sp.js'


def native_fixture(root, drop=()):
    """Synthetic stand-ins for Valve's binaries: only the strings the contract looks for."""
    files = {
        'ubuntu12_32/steamui.so': b'\x7fELF....\x00.cef-enable-remote-debugging\x00more',
        'ubuntu12_32/steamclient.so': b'\x00--remote-debugging-port=%s\x00--remote-allow-origins=*\x00',
    }
    for name, data in files.items():
        if name in drop:
            continue
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


class ContractEngine(unittest.TestCase):
    def setUp(self):
        self.contract = cc.load_contract(CONTRACT_PATH)
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.ui = self.tmp / 'ui'
        shutil.copytree(FIXTURES / 'ui', self.ui)
        self.native = self.tmp / 'native'
        native_fixture(self.native)

    def run_checks(self, **kw):
        return cc.evaluate(self.contract, {'ui': self.ui, 'native': self.native}, **kw)

    def edit_sp(self, fn):
        # WHY bytes: the real fragments contain carriage returns; text mode would silently rewrite them
        # and look like a change in the bundle.
        p = self.ui / SP
        p.write_bytes(fn(p.read_bytes().decode()).encode())

    def failed_ids(self, report):
        return sorted(r.id for r in report.results if r.status == 'fail')

    def test_good_fixture_passes_everything(self):
        report = self.run_checks()
        self.assertEqual(self.failed_ids(report), [])
        self.assertEqual([r.id for r in report.results if r.status == 'review'], [])
        self.assertEqual(report.status, 'pass')
        self.assertEqual(len(report.results), len(self.contract['checks']))

    def test_negative_control_anchor_removed(self):
        self.edit_sp(lambda t: t.replace('"data-extended-chars"', '"data-xtra"'))
        report = self.run_checks()
        self.assertEqual(self.failed_ids(report), ['key-extended-chars'])
        self.assertEqual(report.status, 'fail')

    def test_negative_control_method_renamed(self):
        self.edit_sp(lambda t: t.replace('ControllerKeyboardSendText', 'ControllerKeyboardSendString'))
        report = self.run_checks()
        self.assertEqual(self.failed_ids(report), ['send-text-call'])

    def test_send_text_call_with_two_arguments_fails(self):
        self.edit_sp(lambda t: t.replace('ControllerKeyboardSendText(k)', 'ControllerKeyboardSendText(k,1)'))
        self.assertIn('send-text-call', self.failed_ids(self.run_checks()))

    def test_game_password_mode_regex_needs_the_real_shape(self):
        self.edit_sp(lambda t: t.replace('(ca="password")', '(ca="text")'))
        self.assertEqual(self.failed_ids(self.run_checks()), ['game-password-mode'])

    def test_missing_file_fails_its_checks_instead_of_crashing(self):
        (self.ui / 'steamui/localization/steamui_english-json.js').unlink()
        self.assertEqual(self.failed_ids(self.run_checks()), ['big-picture-title', 'developer-debug-setting'])

    def test_native_marker_missing_fails(self):
        (self.native / 'ubuntu12_32/steamui.so').write_bytes(b'nothing here')
        self.assertEqual(self.failed_ids(self.run_checks()), ['native-marker-file'])

    def test_missing_native_package_fails_native_checks_only(self):
        report = cc.evaluate(self.contract, {'ui': self.ui})
        self.assertEqual(self.failed_ids(report),
                         ['native-allow-origins-option', 'native-debug-port-option', 'native-marker-file'])

    def test_review_severity_does_not_fail_the_run(self):
        self.edit_sp(lambda t: t.replace('AddShortcut(', 'AddShortKut('))
        report = self.run_checks()
        self.assertEqual(report.status, 'review')
        self.assertEqual([r.id for r in report.results if r.status == 'review'], ['add-shortcut-api'])

    def test_swipe_appearing_in_keyboard_region_raises_review(self):
        self.edit_sp(lambda t: t.replace('"data-extended-chars"', '"data-extended-chars",swipeToType:1', 1))
        report = self.run_checks()
        self.assertEqual([r.id for r in report.results if r.status == 'review'], ['no-swipe-in-keyboard'])
        self.assertEqual(report.status, 'review')

    def test_fail_outranks_review(self):
        self.edit_sp(lambda t: t.replace('AddShortcut(', 'x(').replace('SharedJSContext', 'Other'))
        self.assertEqual(self.run_checks().status, 'fail')

    def test_region_hash_ignores_build_stamp_but_sees_real_change(self):
        base = self.run_checks().region_hash
        self.assertRegex(base, r'^[0-9a-f]{64}$')
        # Stamp: Valve embeds a per-build 8-digit number in quotes; it must not count as drift.
        self.edit_sp(lambda t: t.replace('"11146165"', '"11999999"'))
        self.assertEqual(self.run_checks().region_hash, base)
        # A real code change near the keyboard must change the hash.
        self.edit_sp(lambda t: t.replace('ControllerKeyboardSendText(k)', 'ControllerKeyboardSendText(k.trim())'))
        self.assertNotEqual(self.run_checks().region_hash, base)

    def test_contract_hash_is_stable_and_tracks_content(self):
        h = cc.contract_hash(self.contract)
        self.assertEqual(h, cc.contract_hash(copy.deepcopy(self.contract)))
        changed = copy.deepcopy(self.contract)
        changed['checks'][0]['min'] = 2
        self.assertNotEqual(h, cc.contract_hash(changed))

    def test_unreadable_oversize_file_is_a_failure_not_a_hang(self):
        big = self.ui / SP
        big.write_bytes(b'x' * 2048)
        report = cc.evaluate(self.contract, {'ui': self.ui, 'native': self.native}, max_file_bytes=1024)
        self.assertIn('send-text-call', self.failed_ids(report))


class ContractValidation(unittest.TestCase):
    def load(self, mutate):
        data = json.loads(CONTRACT_PATH.read_text())
        mutate(data)
        p = Path(tempfile.mkdtemp()) / 'c.json'
        p.write_text(json.dumps(data))
        self.addCleanup(lambda: shutil.rmtree(p.parent, ignore_errors=True))
        return p

    def test_real_contract_is_valid_and_names_four_channels(self):
        c = cc.load_contract(CONTRACT_PATH)
        self.assertEqual([x['name'] for x in c['source']['channels']],
                         ['desktop-stable', 'desktop-beta', 'deck-stable', 'deck-beta'])

    def test_duplicate_ids_rejected(self):
        p = self.load(lambda d: d['checks'].append(dict(d['checks'][0])))
        with self.assertRaises(cc.ContractError):
            cc.load_contract(p)

    def test_bad_regex_rejected_at_load(self):
        p = self.load(lambda d: d['checks'][0].update(match='regex', pattern='(unclosed'))
        with self.assertRaises(cc.ContractError):
            cc.load_contract(p)

    def test_unknown_severity_and_package_rejected(self):
        for mut in (lambda d: d['checks'][0].update(severity='maybe'),
                    lambda d: d['checks'][0].update(package='nope')):
            with self.assertRaises(cc.ContractError):
                cc.load_contract(self.load(mut))

    def test_device_block_must_point_at_real_checks(self):
        p = self.load(lambda d: d['device']['targets'][0].update(static_check='does-not-exist'))
        with self.assertRaises(cc.ContractError):
            cc.load_contract(p)

    def test_path_in_contract_must_stay_relative(self):
        p = self.load(lambda d: d['checks'][0].update(file='../../etc/passwd'))
        with self.assertRaises(cc.ContractError):
            cc.load_contract(p)


if __name__ == '__main__':
    unittest.main()
