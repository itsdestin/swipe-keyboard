"""Exercise setup against disposable files and a stand-in for the live KConfig bus."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from test_layout import load_module, STOCK


class Commands:
    def __init__(self):
        self.settings = {'InputMethod': '/old/keyboard.desktop', 'VirtualKeyboardMode': '0'}
        self.plasma_version = 'plasma-keyboard 6.7.5'
        self.qt_version = '6.12.0'
        self.calls = []
        self.fail_key = None
        self.fedora_commands = False
        self.live_mode = 0
        self.arch_qtpaths = False

    def __call__(self, args, **kwargs):
        self.calls.append(args)
        command = Path(args[0]).name
        if self.fedora_commands and command == 'qdbus6':
            raise FileNotFoundError('Fedora calls it qdbus-qt6')
        value = ''
        if command == 'plasma-keyboard':
            value = self.plasma_version
            assert kwargs['env']['QT_QPA_PLATFORM'] == 'offscreen'
        elif command in ('qtpaths6', 'qtpaths-qt6'):
            if self.arch_qtpaths and args[0] != '/usr/lib/qt6/bin/qtpaths6':
                raise FileNotFoundError('Arch keeps qtpaths6 outside the default PATH')
            value = self.qt_version
        elif command == 'kreadconfig6':
            key = args[args.index('--key') + 1]
            value = self.settings.get(key, args[args.index('--default') + 1])
        elif command == 'kwriteconfig6':
            key = args[args.index('--key') + 1]
            if self.fail_key == key:
                self.fail_key = None
                raise subprocess.CalledProcessError(1, args, stderr='simulated write failure')
            if '--delete' in args:
                self.settings.pop(key, None)
            else:
                self.settings[key] = args[-1]
        elif command == 'busctl':
            if 'get-property' in args:
                value = 'i ' + str(self.live_mode)
            else:
                self.live_mode = int(args[-1])
                self.settings['VirtualKeyboardMode'] = str(self.live_mode)
        elif command not in ('qdbus6', 'qdbus-qt6'):
            raise AssertionError(args)
        return subprocess.CompletedProcess(args, 0, value + '\n', '')


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.module = load_module('setup')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'usr/share/swipe-keyboard'
        self.root.mkdir(parents=True)
        (self.root / 'runtime.json').write_text(json.dumps({'qt': '6.12'}))
        for name in self.module.REQUIRED_ASSETS:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture')
        self.stock = self.base / 'stock'
        (self.stock / 'fallback').mkdir(parents=True)
        (self.stock / 'fallback/main.qml').write_text(STOCK)
        (self.stock / 'en_US').mkdir()
        # Qt/Plasma uses an empty marker file, not a text destination.
        (self.stock / 'en_US/main.fallback').write_text('')
        self.env = {**os.environ, 'HOME': str(self.base / 'home'),
                    'XDG_DATA_HOME': str(self.base / 'data'),
                    'XDG_CONFIG_HOME': str(self.base / 'config'),
                    'XDG_SESSION_TYPE': 'wayland', 'XDG_CURRENT_DESKTOP': 'KDE',
                    'SWIPEKB_SYSTEM_LAYOUTS': str(self.stock)}
        self.commands = Commands()
        self.controller = self.module.Controller(self.root, self.env, self.commands)

    def hold_setup_lock(self, release_after=None):
        self.controller.data.mkdir(parents=True, exist_ok=True)
        # WHY a separate process: exercise real flock contention, not a mocked lock result.
        script = ('import fcntl, sys, time\n'
                  'with open(sys.argv[1], "a") as lock:\n'
                  '    fcntl.flock(lock, fcntl.LOCK_EX)\n'
                  '    print("locked", flush=True)\n'
                  '    if sys.argv[2] == "None": sys.stdin.read()\n'
                  '    else: time.sleep(float(sys.argv[2]))\n')
        process = subprocess.Popen(
            [sys.executable, '-c', script, str(self.controller.data / '.setup.lock'), str(release_after)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(process.communicate, b'', timeout=5)
        self.assertEqual(process.stdout.readline(), b'locked\n')
        return process

    def test_launcher_waits_for_setup_transaction(self):
        holder = self.hold_setup_lock(release_after=0.1)
        env = self.controller.launch_environment()
        self.assertEqual(holder.wait(timeout=5), 0)
        self.assertEqual(env['SWIPEKB_MODEL_DATA'], str(self.root))
        self.assertEqual(env['SWIPEKB_DATA'], str(self.controller.data))
        self.assertEqual(env['QML_IMPORT_PATH'].split(':')[0], str(self.root / 'qml'))
        self.assertEqual(env['XDG_DATA_DIRS'].split(':')[0], str(self.controller.data / 'overlay'))
        self.assertTrue((self.controller.layouts / 'en_US/main.qml').is_file())

    def test_concurrent_enable_and_disable_remain_fail_fast(self):
        self.controller.enable()
        original = dict(self.commands.settings)
        self.hold_setup_lock()
        with mock.patch.object(self.module, 'time', create=True) as clock:
            for action in (self.controller.enable, self.controller.disable):
                with self.subTest(action=action.__name__):
                    with self.assertRaisesRegex(self.module.SetupError, 'Another keyboard setup'):
                        action()
            clock.sleep.assert_not_called()
        self.assertEqual(self.commands.settings, original)
        self.assertTrue(self.controller.state_file.exists())

    def test_launcher_lock_timeout_falls_back_to_stock(self):
        self.hold_setup_lock()
        with mock.patch.object(self.module, 'time', create=True) as clock, \
             mock.patch.object(self.module, 'Controller', return_value=self.controller), \
             mock.patch.object(self.module.sys, 'argv', ['swipe-keyboard', 'launch']), \
             mock.patch.object(self.module.os, 'geteuid', return_value=1000), \
             mock.patch.object(self.module.os, 'execvpe', side_effect=SystemExit(0)) as execute:
            # WHY a virtual monotonic clock: prove the five-second bound without delaying tests.
            clock.monotonic.side_effect = [10, 10, 15]
            with self.assertRaises(SystemExit):
                self.module.main()
            clock.sleep.assert_called_once_with(0.05)
            execute.assert_called_once_with('plasma-keyboard', ['plasma-keyboard'], dict(os.environ))
        self.assertFalse(self.controller.layouts.exists())

    def test_check_is_read_only(self):
        self.controller.check()
        self.assertFalse((self.base / 'data').exists())
        self.assertFalse(any(Path(c[0]).name == 'kwriteconfig6' for c in self.commands.calls))

    def test_unsupported_session_or_versions_make_no_changes(self):
        cases = [('XDG_SESSION_TYPE', 'x11'), ('XDG_CURRENT_DESKTOP', 'GNOME')]
        for key, value in cases:
            with self.subTest(key=key):
                controller = self.module.Controller(self.root, {**self.env, key: value}, self.commands)
                with self.assertRaises(self.module.SetupError):
                    controller.enable()
        self.commands.plasma_version = 'plasma-keyboard 6.9.0'
        with self.assertRaises(self.module.SetupError):
            self.controller.enable()
        self.commands.plasma_version = 'plasma-keyboard 6.7.5'
        self.commands.qt_version = '6.11.0'
        with self.assertRaises(self.module.SetupError):
            self.controller.enable()
        self.assertFalse((self.base / 'data').exists())
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_arch_qt_version_tool_outside_default_path(self):
        self.commands.arch_qtpaths = True
        self.controller.check()
        self.assertTrue(any(c[0] == '/usr/lib/qt6/bin/qtpaths6' for c in self.commands.calls))
        self.assertFalse((self.base / 'data').exists())

    def test_current_arch_plasma_series_is_a_candidate(self):
        self.commands.plasma_version = 'plasma-keyboard 6.8.0'
        self.controller.check()
        self.assertFalse((self.base / 'data').exists())

    def test_explicit_qt_layout_override_refuses_before_changes(self):
        self.controller.env['PLASMA_KEYBOARD_USE_QT_LAYOUTS'] = '1'
        with self.assertRaises(self.module.SetupError):
            self.controller.enable()
        self.assertFalse((self.base / 'data').exists())
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_missing_payload_refuses_before_settings_change(self):
        (self.root / self.module.REQUIRED_ASSETS[0]).unlink()
        with self.assertRaises(self.module.SetupError):
            self.controller.enable()
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_existing_custom_layout_is_never_overwritten(self):
        custom = self.base / 'data/plasma/keyboard/layouts'
        custom.mkdir(parents=True)
        (custom / 'mine').write_text('leave alone')
        with self.assertRaises(self.module.SetupError):
            self.controller.enable()
        self.assertEqual((custom / 'mine').read_text(), 'leave alone')
        self.assertFalse((self.base / 'data/swipe-keyboard').exists())

    def test_enable_disable_restores_original_settings(self):
        original = dict(self.commands.settings)
        self.controller.enable()
        self.assertTrue(self.commands.settings['InputMethod'].endswith('/applications/swipe-keyboard.desktop'))
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '1')
        self.controller.enable()
        self.controller.disable()
        self.assertEqual(self.commands.settings, original)
        self.assertFalse(self.controller.state_file.exists())

    def test_enable_applies_live_visibility_and_disable_restores_it(self):
        self.commands.live_mode = 2
        self.controller.enable()
        self.assertEqual(self.commands.live_mode, 1)
        self.controller.disable()
        self.assertEqual(self.commands.live_mode, 2)
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '0')

    def test_interrupted_mode_restore_can_be_retried(self):
        self.commands.live_mode = 2
        self.controller.enable()
        self.commands.fail_key = 'VirtualKeyboardMode'
        with self.assertRaises(self.module.SetupError):
            self.controller.disable()
        self.assertTrue(self.controller.state_file.exists())
        self.controller.disable()
        self.assertEqual(self.commands.live_mode, 2)
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '0')
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_later_live_visibility_change_is_preserved(self):
        self.controller.enable()
        self.commands.live_mode = 2
        self.commands.settings['VirtualKeyboardMode'] = '2'
        self.controller.disable()
        self.assertEqual(self.commands.live_mode, 2)
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '2')

    def test_filesystem_failure_in_launcher_falls_back_to_stock(self):
        with mock.patch.object(self.module, 'Controller') as factory, \
             mock.patch.object(self.module.sys, 'argv', ['swipe-keyboard', 'launch']), \
             mock.patch.object(self.module.os, 'execvpe', side_effect=SystemExit(0)) as execute:
            factory.return_value.launch_environment.side_effect = PermissionError('read-only state folder')
            with self.assertRaises(SystemExit):
                self.module.main()
            execute.assert_called_once_with('plasma-keyboard', ['plasma-keyboard'], dict(os.environ))

    def test_fedora_dbus_command_name_is_supported(self):
        self.commands.fedora_commands = True
        self.controller.enable()
        self.controller.disable()
        self.assertTrue(any(Path(c[0]).name == 'qdbus-qt6' for c in self.commands.calls))

    def test_unset_settings_return_to_unset(self):
        self.commands.settings.clear()
        self.controller.enable()
        self.controller.disable()
        self.assertEqual(self.commands.settings, {})

    def test_disable_preserves_later_user_changes_and_private_data(self):
        self.controller.enable()
        data = self.base / 'data/swipe-keyboard'
        (data / 'record').write_text('user opted in')
        self.commands.settings['InputMethod'] = '/user/new-choice.desktop'
        self.commands.settings['VirtualKeyboardMode'] = '2'
        self.controller.disable()
        self.assertEqual(self.commands.settings['InputMethod'], '/user/new-choice.desktop')
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '2')
        self.assertTrue((data / 'record').exists())

    def test_failed_enable_rolls_back_config(self):
        original = dict(self.commands.settings)
        self.commands.fail_key = 'VirtualKeyboardMode'
        with self.assertRaises(self.module.SetupError):
            self.controller.enable()
        self.assertEqual(self.commands.settings, original)
        self.assertFalse(self.controller.state_file.exists())

    def test_disable_does_not_need_installed_assets_or_supported_session(self):
        original = dict(self.commands.settings)
        self.controller.enable()
        (self.root / 'runtime.json').unlink()
        self.controller.env['XDG_SESSION_TYPE'] = 'x11'
        self.controller.disable()
        self.assertEqual(self.commands.settings, original)

    def test_launcher_separates_models_from_private_recordings(self):
        self.controller.enable()
        env = self.controller.launch_environment()
        self.assertEqual(env['SWIPEKB_MODEL_DATA'], str(self.root))
        self.assertEqual(env['SWIPEKB_DATA'], str(self.base / 'data/swipe-keyboard'))
        self.assertTrue(env['XDG_DATA_DIRS'].startswith(str(self.base / 'data/swipe-keyboard/overlay') + ':'))
        self.assertTrue(env['QML_IMPORT_PATH'].startswith(str(self.root / 'qml')))
        self.assertFalse((self.base / 'data/swipe-keyboard/record').exists())

    def test_launch_regenerates_layout_after_stock_update(self):
        self.controller.enable()
        (self.stock / 'fallback/main.qml').write_text(STOCK.replace('text: "q"', 'text: "Q"'))
        self.controller.launch_environment()
        self.assertIn('text: "Q"', (self.base / 'data/swipe-keyboard/overlay/plasma/keyboard/layouts/en_US/main.qml').read_text())

    def test_enable_does_not_replace_changed_keyboard_on_repeat(self):
        self.controller.enable()
        self.commands.settings['InputMethod'] = '/user/new-choice.desktop'
        with self.assertRaises(self.module.SetupError):
            self.controller.enable()
        self.assertEqual(self.commands.settings['InputMethod'], '/user/new-choice.desktop')


if __name__ == '__main__':
    unittest.main()
