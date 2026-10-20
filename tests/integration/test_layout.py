"""Language hooks preserve each stock layout and fail closed transactionally on drift."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

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

    def test_builds_only_present_approved_overrides_and_keeps_other_languages(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / 'stock', Path(tmp) / 'overlay'
            (src / 'fallback').mkdir(parents=True)
            (src / 'fallback/main.qml').write_text(STOCK)
            (src / 'en_US').mkdir()
            (src / 'en_US/main.fallback').write_text('')
            (src / 'en_US/symbols.fallback').write_text('')
            (src / 'pt_BR').mkdir()
            (src / 'pt_BR/main.qml').write_text('PORTUGUESE UNCHANGED')
            self.layout.build_overlay(src, dst)
            self.assertIn('SwipeCapture', (dst / 'en_US/main.qml').read_text())
            self.assertEqual((dst / 'fallback/main.qml').read_text(), STOCK)
            self.assertEqual((dst / 'pt_BR/main.qml').read_text(), 'PORTUGUESE UNCHANGED')
            self.assertEqual((dst / 'en_US/symbols.fallback').read_text(), '')
            self.assertEqual((src / 'en_US/main.fallback').read_text(), '')

    def test_hooks_approved_locales_without_changing_physical_stock_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / 'stock', Path(tmp) / 'overlay'
            sources = {}
            for locale, glyph in (('en_US', 'q'), ('en_GB', '£'), ('es_ES', 'ñ'),
                                  ('es_MX', 'á'), ('fr_FR', 'a'), ('fr_CA', 'ç'),
                                  ('de_DE', 'z'), ('de_AT', 'ü'), ('es', 'í')):
                folder = src / locale
                folder.mkdir(parents=True)
                source = STOCK.replace('text: "q"', f'text: "{glyph}"; alternativeKeys: "{glyph}éß"')
                source = source.replace('    Key {', f'    // physical {locale}\n    Key {{')
                (folder / 'main.qml').write_text(source)
                (folder / 'symbols.fallback').write_text('')
                sources[locale] = source
            for locale in ('en_AU', 'pt_BR', 'it_IT'):
                (src / locale).mkdir()
                (src / locale / 'main.qml').write_text('untargeted custom layout')
            before = {p.relative_to(src): p.read_bytes() for p in src.rglob('*') if p.is_file()}
            self.layout.build_overlay(src, dst)
            for locale, source in sources.items():
                with self.subTest(locale=locale):
                    result = (dst / locale / 'main.qml').read_text()
                    self.assertEqual(result, self.layout.patch_english(source))
                    self.assertIn('SwipeInputMethod {}', result)
                    self.assertEqual((dst / locale / 'symbols.fallback').read_text(), '')
                    self.assertNotIn('en_US', result if locale != 'en_US' else '')
                    self.assertNotIn('English-only', result)
            for locale in ('en_AU', 'pt_BR', 'it_IT'):
                self.assertEqual((dst / locale / 'main.qml').read_text(), 'untargeted custom layout')
            self.assertEqual(before, {p.relative_to(src): p.read_bytes() for p in src.rglob('*') if p.is_file()})

    def test_each_empty_fallback_marker_uses_stock_fallback_not_us_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / 'stock', Path(tmp) / 'overlay'
            (src / 'fallback').mkdir(parents=True)
            fallback = STOCK.replace('text: "q"', 'text: "fallback geometry"')
            (src / 'fallback/main.qml').write_text(fallback)
            (src / 'en_US').mkdir()
            (src / 'en_US/main.qml').write_text(STOCK.replace('text: "q"', 'text: "US geometry"'))
            for locale in ('en_GB', 'es_AR', 'fr_BE', 'de_CH'):
                (src / locale).mkdir()
                (src / locale / 'main.fallback').write_text('')
            self.layout.build_overlay(src, dst)
            for locale in ('en_GB', 'es_AR', 'fr_BE', 'de_CH'):
                with self.subTest(locale=locale):
                    self.assertEqual((dst / locale / 'main.qml').read_text(), self.layout.patch_english(fallback))
                    self.assertFalse((dst / locale / 'main.fallback').exists())
                    self.assertEqual((src / locale / 'main.fallback').read_text(), '')
            self.assertEqual((dst / 'fallback/main.qml').read_text(), fallback)
            self.assertFalse((dst / 'fr_FR').exists())  # Never synthesize absent locale directories.

    def test_invalid_target_locale_does_not_publish_any_partial_overlay(self):
        for bad in ('Item {}', STOCK.replace('KeyboardLayout {', 'KeyboardLayout {\n id: custom'),
                    STOCK.replace('SpaceKey {}', 'SpaceKey {}\n Component.onCompleted: {}')):
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as tmp:
                src, dst = Path(tmp) / 'stock', Path(tmp) / 'overlay'
                for locale, source in (('en_US', STOCK), ('es_ES', STOCK), ('fr_FR', bad)):
                    (src / locale).mkdir(parents=True)
                    (src / locale / 'main.qml').write_text(source)
                dst.mkdir()
                (dst / 'precious').write_text('keep')
                with self.assertRaises(ValueError):
                    self.layout.build_overlay(src, dst)
                self.assertEqual(list(dst.iterdir()), [dst / 'precious'])
                self.assertEqual((dst / 'precious').read_text(), 'keep')
                self.assertFalse(dst.with_name('overlay.previous').exists())
                self.assertFalse(list(dst.parent.glob('.layouts-*')))

    def test_nonempty_or_missing_target_fallback_is_never_guessed(self):
        for marker in ('fr_FR', None):
            with self.subTest(marker=marker), tempfile.TemporaryDirectory() as tmp:
                src, dst = Path(tmp) / 'stock', Path(tmp) / 'overlay'
                (src / 'en_US').mkdir(parents=True)
                (src / 'en_US/main.qml').write_text(STOCK)
                (src / 'fr_CA').mkdir()
                (src / 'fr_FR').mkdir()
                (src / 'fr_FR/main.qml').write_text(STOCK)
                if marker is not None:
                    (src / 'fr_CA/main.fallback').write_text(marker)
                with self.assertRaises(ValueError):
                    self.layout.build_overlay(src, dst)
                self.assertFalse(dst.exists())

    def test_rename_failure_rolls_back_complete_previous_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / 'stock', Path(tmp) / 'overlay'
            for locale in ('en_US', 'en_GB', 'es_ES', 'fr_FR', 'de_DE'):
                (src / locale).mkdir(parents=True)
                (src / locale / 'main.qml').write_text(STOCK)
            dst.mkdir()
            (dst / 'previous-content').write_text('keep')
            original = Path.rename
            def fail_publish(path, target):
                if path.name == 'layouts' and Path(target) == dst:
                    raise OSError('simulated staged rename failure')
                return original(path, target)
            with mock.patch.object(Path, 'rename', fail_publish):
                with self.assertRaisesRegex(OSError, 'staged rename'):
                    self.layout.build_overlay(src, dst)
            self.assertEqual(list(dst.iterdir()), [dst / 'previous-content'])
            self.assertEqual((dst / 'previous-content').read_text(), 'keep')
            self.assertFalse(dst.with_name('overlay.previous').exists())

    def test_nested_or_malformed_custom_root_is_not_patched(self):
        for source in ('Item {\n' + STOCK + '\n}', STOCK + '\nItem {}',
                       STOCK.replace('    SpaceKey {}', '    SpaceKey {'),
                       STOCK.replace('    SpaceKey {}', '    SpaceKey {}\n id: custom'),
                       STOCK.replace('    SpaceKey {}', '    SpaceKey {}\n inputMethod: custom')):
            with self.subTest(source=source), self.assertRaises(ValueError):
                self.layout.patch_english(source)

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
