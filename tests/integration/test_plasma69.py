"""The launcher picks the right front end for the installed Plasma Keyboard, and the 6.9 package generator.

Everything runs against disposable folders: fake KDE packages in both of KDE's formats (master's "Files" map and
MR !179's single "File"), a stand-in for the commands the launcher runs, and no real keyboard or KWin setting.
"""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest import mock

import test_setup
from test_layout import load_module

MASTER_EN = {
    'KPlugin': {'Id': 'org.kde.plasma.keyboard.en', 'Name': 'English'},
    'X-Plasma-Keyboard': {
        'KeyboardLayouts': [
            {'Files': {'main': 'MobileQwerty.qml', 'symbols': 'shared/Symbols.qml'}, 'Id': 'mobile-qwerty', 'Name': 'QWERTY'},
            {'Files': {'main': 'DesktopQwerty.qml', 'symbols': 'shared/Symbols.qml'}, 'Id': 'desktop-qwerty', 'Name': 'QWERTY (Desktop)'},
        ],
        'Locales': ['en_US', 'en_GB'],
    },
}
MR179_DE = {
    'KPlugin': {'Id': 'org.kde.plasma.keyboard.de_DE', 'Name': 'German'},
    'X-Plasma-Keyboard': {'KeyboardLayouts': [{'File': 'Qwertz.qml', 'Id': 'mobile-qwertz', 'Name': 'QWERTZ'}],
                          'Locales': ['de_DE']},
}
RUSSIAN = {
    'KPlugin': {'Id': 'org.kde.plasma.keyboard.ru_RU', 'Name': 'Russian'},
    'X-Plasma-Keyboard': {'KeyboardLayouts': [{'File': 'Jcuken.qml', 'Id': 'mobile-jcuken'}], 'Locales': ['ru_RU']},
}


def kde_package(root, metadata, files):
    folder = root / 'plasma/keyboard/keyboardpackages' / metadata['KPlugin']['Id']
    (folder / 'contents/layouts/shared').mkdir(parents=True, exist_ok=True)
    (folder / 'metadata.json').write_text(json.dumps(metadata))
    for name in files:
        (folder / 'contents/layouts' / name).write_text('import QtQuick\nItem {}\n')
    return folder


def tree_digest(folder):
    digest = hashlib.sha256()
    for path in sorted(folder.rglob('*')):
        if path.is_file():
            digest.update(str(path.relative_to(folder)).encode() + path.read_bytes())
    return digest.hexdigest()


class FrontEndSelection(unittest.TestCase):
    """Reuses the launcher test fixture (fake commands, fake settings) and adds Plasma 6.9's packages."""

    def setUp(self):
        # WHY the fixture only, not subclassing: the launcher's own tests must not run twice here.
        test_setup.SetupTests.setUp(self)
        self.plasma69 = load_module('plasma69')
        self.system = self.base / 'system-data'
        self.en = kde_package(self.system, MASTER_EN, ['MobileQwerty.qml', 'DesktopQwerty.qml', 'shared/Symbols.qml'])
        self.de = kde_package(self.system, MR179_DE, ['Qwertz.qml'])
        self.ru = kde_package(self.system, RUSSIAN, ['Jcuken.qml'])
        for name in self.module.REQUIRED_ASSETS_69:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture')
        self.commands.plasma_version = 'plasma-keyboard 6.9.0'

    def launch(self):
        """Runs `swipe-keyboard launch` and returns the environment it started plasma-keyboard with."""
        with mock.patch.object(self.module, 'Controller', return_value=self.controller), \
             mock.patch.object(self.module.sys, 'argv', ['swipe-keyboard', 'launch']), \
             mock.patch.object(self.module.os, 'execvpe', side_effect=SystemExit(0)) as execute, \
             mock.patch('sys.stderr', new_callable=io.StringIO) as err:
            with self.assertRaises(SystemExit):
                self.module.main()
        execute.assert_called_once()
        self.stderr = err.getvalue()
        return execute.call_args[0][2]

    # --- which front end ---------------------------------------------------------------------------------------

    def test_each_version_gets_the_matching_front_end(self):
        cases = {
            'plasma-keyboard 6.7.5': 'legacy',
            'plasma-keyboard 6.8.1': 'legacy',
            'plasma-keyboard 6.8.9': 'legacy',
            'plasma-keyboard 6.8.80': 'plasma69',   # 6.9 development series
            'plasma-keyboard 6.8.90': 'plasma69',   # 6.9 beta
            'plasma-keyboard 6.9.0': 'plasma69',
            'plasma-keyboard 6.9.5': 'plasma69',
            'plasma-keyboard 6.10.0': 'plasma69',
        }
        for version, expected in cases.items():
            with self.subTest(version=version):
                self.commands.plasma_version = version
                env = self.launch()
                self.assertEqual(self.controller.front_end, expected)
                if expected == 'plasma69':
                    self.assertIn('org.kde.plasma.keyboard.en', env['SWIPEKB_PLASMA69_PACKAGES'])
                    self.assertNotIn('SWIPEKB_NATIVE_PREDICTION', env)
                    self.assertTrue(self.commands.preflighted.endswith('SwipeKeyboard69/libswipekeyboard69plugin.so'))
                else:
                    self.assertEqual(env['SWIPEKB_NATIVE_PREDICTION'], 'owned-history-v1')
                    self.assertNotIn('SWIPEKB_PLASMA69_PACKAGES', env)
                self.assertTrue(env['XDG_DATA_DIRS'].startswith(str(self.controller.data / 'overlay') + ':'))
                self.assertIn(str(self.root / 'qml'), env['QML_IMPORT_PATH'])

    def test_unknown_or_mismatched_systems_start_the_plain_keyboard(self):
        cases = [
            ('plasma-keyboard 7.0.0', None),                 # new major: new Qt major, our modules cannot load
            ('plasma-keyboard 7.1.2', None),
            ('plasma-keyboard 6.9.0', 'no-packages'),         # 6.9 binary but no 6.9 keyboard packages
            ('plasma-keyboard 6.8.5', 'no-legacy'),           # 6.8 binary but no Qt Virtual Keyboard layouts
            ('plasma-keyboard 6.6.0', None),
            ('plasma-keyboard (garbled)', 'both'),            # unknown version and both kinds of files
        ]
        for version, damage in cases:
            with self.subTest(version=version, damage=damage):
                self.setUp()
                self.commands.plasma_version = version
                if damage == 'no-packages':
                    import shutil
                    shutil.rmtree(self.system / 'plasma')
                if damage == 'no-legacy':
                    import shutil
                    shutil.rmtree(self.stock / 'en_US')
                env = self.launch()
                self.assertEqual(env, dict(os.environ))   # KDE's keyboard, unmodified environment
                self.assertIn('Starting the unmodified stock keyboard instead', self.stderr)
                self.assertFalse(self.controller.packages.exists())

    def test_unknown_version_is_decided_by_the_files_alone(self):
        import shutil
        shutil.rmtree(self.stock / 'en_US')
        self.commands.plasma_version = 'plasma-keyboard (development build)'
        env = self.launch()
        self.assertEqual(self.controller.front_end, 'plasma69')
        self.assertIn('SWIPEKB_PLASMA69_PACKAGES', env)

    def test_six_nine_never_needs_futo_or_qt_virtual_keyboard_files(self):
        # WHY: FUTO is being removed and Qt Virtual Keyboard is not part of Plasma 6.9.
        for name in self.module.REQUIRED_ASSETS:
            if name not in self.module.REQUIRED_ASSETS_69:
                (self.root / name).unlink()
        env = self.launch()
        self.assertEqual(self.controller.front_end, 'plasma69')
        self.assertIn('SWIPEKB_PLASMA69_PACKAGES', env)

    def test_broken_six_nine_module_falls_back_to_the_plain_keyboard(self):
        self.commands.preflight_error = 'libswipekeyboard69.so: undefined symbol: _ZN3Foo3barEv'
        env = self.launch()
        self.assertEqual(env, dict(os.environ))
        self.assertIn('Native plugin load failed', self.stderr)
        (self.root / 'qml/SwipeKeyboard69/qmldir').unlink()
        self.commands.preflight_error = None
        env = self.launch()
        self.assertEqual(env, dict(os.environ))
        self.assertIn('Package files are missing', self.stderr)

    def test_unknown_package_format_falls_back_to_the_plain_keyboard(self):
        for folder in (self.en, self.de):
            metadata = json.loads((folder / 'metadata.json').read_text())
            for layout in metadata['X-Plasma-Keyboard']['KeyboardLayouts']:
                layout.pop('Files', None)
                layout.pop('File', None)
                layout['Definition'] = 'Something.qml'   # a future format we do not know
            (folder / 'metadata.json').write_text(json.dumps(metadata))
        env = self.launch()
        self.assertEqual(env, dict(os.environ))
        self.assertIn('No Plasma Keyboard package', self.stderr)

    def test_check_and_enable_on_six_nine(self):
        report = self.controller.check()
        self.assertIn('Plasma 6.9', report['Front end'])
        self.assertFalse((self.base / 'data').exists())   # check writes nothing
        self.controller.enable()
        self.assertEqual(self.commands.settings['InputMethod'], self.controller.wanted['InputMethod'])
        self.assertTrue(self.controller.packages.is_dir())
        self.controller.disable()
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')
        self.assertFalse(self.controller.packages.exists())

    # --- the generated packages -----------------------------------------------------------------------------------

    def test_generated_packages_wrap_both_formats_and_leave_kde_untouched(self):
        before = {f: tree_digest(f) for f in (self.en, self.de, self.ru)}
        env = self.launch()
        self.assertEqual({f: tree_digest(f) for f in (self.en, self.de, self.ru)}, before)
        self.assertEqual(env['SWIPEKB_PLASMA69_PACKAGES'], 'org.kde.plasma.keyboard.en,org.kde.plasma.keyboard.de_DE')
        out = self.controller.packages
        en = json.loads((out / self.en.name / 'metadata.json').read_text())
        self.assertEqual(en['KPlugin']['Id'], 'org.kde.plasma.keyboard.en')   # same id: saved choices keep working
        mains = [layout['Files']['main'] for layout in en['X-Plasma-Keyboard']['KeyboardLayouts']]
        self.assertEqual(mains, ['SwipeKeyboardMobileQwerty.qml', 'SwipeKeyboardDesktopQwerty.qml'])
        self.assertEqual(en['X-Plasma-Keyboard']['KeyboardLayouts'][0]['Files']['symbols'], 'shared/Symbols.qml')
        wrapper = (out / self.en.name / 'contents/layouts/SwipeKeyboardMobileQwerty.qml').read_text()
        self.assertIn('MobileQwerty {', wrapper)
        self.assertIn('Qt.createComponent("SwipeKeyboard69", "SwipeAttach")', wrapper)
        self.assertNotIn('import SwipeKeyboard69', wrapper)   # a missing module must never break KDE's layout
        de = json.loads((out / self.de.name / 'metadata.json').read_text())
        self.assertEqual(de['X-Plasma-Keyboard']['KeyboardLayouts'][0]['File'], 'SwipeKeyboardQwertz.qml')
        self.assertTrue((out / self.de.name / 'contents/layouts/Qwertz.qml').is_file())   # KDE's file, copied as is
        self.assertFalse((out / self.ru.name).exists())   # languages we do not support stay KDE's only

    def test_packages_are_rebuilt_from_kde_at_every_launch(self):
        self.launch()
        (self.en / 'contents/layouts/MobileQwerty.qml').write_text('import QtQuick\nItem { objectName: "updated" }\n')
        self.launch()
        copied = self.controller.packages / self.en.name / 'contents/layouts/MobileQwerty.qml'
        self.assertIn('updated', copied.read_text())
        self.assertFalse(list(self.controller.packages.parent.glob('*.previous')))

    def test_our_own_overlay_is_never_read_as_kde_input(self):
        # WHY: the overlay is first in XDG_DATA_DIRS for the keyboard; reading it back would wrap a wrapper.
        self.launch()
        self.controller.env['SWIPEKB_SYSTEM_DATA_DIRS'] = str(self.controller.data / 'overlay') + ':' + str(self.system)
        self.launch()
        metadata = json.loads((self.controller.packages / self.en.name / 'metadata.json').read_text())
        self.assertEqual(metadata['X-SwipeKeyboard']['GeneratedFrom'], str(self.en))


if __name__ == '__main__':
    unittest.main()
