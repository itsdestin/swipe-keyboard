"""English-only integration must preserve stock layouts and fail closed on drift."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
STOCK = '''// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.VirtualKeyboard
KeyboardLayout {
    inputMode: InputEngine.InputMode.Latin
    Key { key: Qt.Key_Q; text: "q" }
    SpaceKey {}
}
'''


def load_module(name):
    path = ROOT / 'integration' / (name + '.py')
    if not path.is_file():
        raise AssertionError(f'Missing standalone integration: {path.name}')
    spec = importlib.util.spec_from_file_location('swipe_' + name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.layout = load_module('layout')

    def test_injects_listener_without_rearranging_keys(self):
        result = self.layout.patch_english(STOCK)
        self.assertIn('Key { key: Qt.Key_Q; text: "q" }', result)
        self.assertIn('SpaceKey {}', result)
        self.assertIn('createInputMethod()', result)
        self.assertIn('SwipeCapture', result)
        self.assertIn('id: swipeLayout', result)
        self.assertTrue(result.startswith('// SPDX-License-Identifier: GPL-3.0-only'))

    def test_refuses_unknown_or_already_customised_layout(self):
        for source in ('Item {}', STOCK.replace('SpaceKey {}', ''),
                       STOCK.replace('KeyboardLayout {', 'KeyboardLayout {\n id: custom'),
                       STOCK.replace('SpaceKey {}', 'SpaceKey {}\n Component.onCompleted: {}')):
            with self.subTest(source=source):
                with self.assertRaises(ValueError):
                    self.layout.patch_english(source)

    def test_builds_only_english_override_and_keeps_other_languages(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / 'stock', Path(tmp) / 'overlay'
            (src / 'fallback').mkdir(parents=True)
            (src / 'fallback/main.qml').write_text(STOCK)
            (src / 'en_US').mkdir()
            (src / 'en_US/main.fallback').write_text('')
            (src / 'en_US/symbols.fallback').write_text('')
            (src / 'de_DE').mkdir()
            (src / 'de_DE/main.qml').write_text('GERMAN UNCHANGED')
            self.layout.build_overlay(src, dst)
            self.assertIn('SwipeCapture', (dst / 'en_US/main.qml').read_text())
            self.assertEqual((dst / 'fallback/main.qml').read_text(), STOCK)
            self.assertEqual((dst / 'de_DE/main.qml').read_text(), 'GERMAN UNCHANGED')
            self.assertEqual((dst / 'en_US/symbols.fallback').read_text(), '')
            self.assertEqual((src / 'en_US/main.fallback').read_text(), '')

    def test_failure_preserves_previous_overlay(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / 'stock', Path(tmp) / 'overlay'
            (src / 'fallback').mkdir(parents=True)
            (src / 'fallback/main.qml').write_text('unknown upstream format')
            (src / 'en_US').mkdir()
            dst.mkdir()
            (dst / 'precious').write_text('keep')
            with self.assertRaises(ValueError):
                self.layout.build_overlay(src, dst)
            self.assertEqual((dst / 'precious').read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()
