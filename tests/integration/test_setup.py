# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Exercise setup against disposable files and a stand-in for the live KConfig bus."""
import io
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
        self.preflight_error = None
        self.defaults = {}
        self.immutable = set()

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
        elif command == 'swipekb-preflight':
            if self.preflight_error:
                raise subprocess.CalledProcessError(1, args, stderr=self.preflight_error)
            assert args[1].endswith('/qml/SwipeKeyboard/libswipekeyboardplugin.so')
            assert kwargs['timeout'] == 15
        elif command == 'swipekb-config':
            value = json.dumps({key: {'raw': self.settings.get(key),
                                     'effective': self.settings.get(key, self.defaults.get(key)),
                                     'immutable': key in self.immutable}
                                for key in ('InputMethod', 'VirtualKeyboardMode')})
        elif command == 'kreadconfig6':
            key = args[args.index('--key') + 1]
            value = self.settings.get(key, self.defaults.get(key, args[args.index('--default') + 1]))
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
            elif 'set-property' in args:
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

    def test_activation_discloses_multilingual_prediction_and_native_privacy_boundary(self):
        import contextlib
        import io
        output=io.StringIO()
        with contextlib.redirect_stdout(output), mock.patch('builtins.input',return_value='n'):
            accepted=self.module.confirm_activation(self.controller)
        text=output.getvalue()
        self.assertFalse(accepted)
        for language in ('English','Spanish','French','German'):
            self.assertIn(language,text)
        self.assertIn('current-word completions',text)
        self.assertIn('words this keyboard confirmed in the current field',text)
        self.assertIn('raw application completion preference',text)
        self.assertIn('Sensitive and restricted fields remain blocked',text)
        self.assertIn('VirtualKeyboardMode',text)
        self.assertIn('preserved',text)
        self.assertFalse(self.controller.state_file.exists())
        self.assertEqual(self.commands.settings['InputMethod'],'/old/keyboard.desktop')

    def test_activation_consent_discloses_multilingual_prediction_and_native_limits(self):
        import contextlib
        import io
        output=io.StringIO()
        with contextlib.redirect_stdout(output), mock.patch('builtins.input',return_value='n'):
            accepted=self.module.confirm_activation(self.controller)
        text=output.getvalue()
        self.assertFalse(accepted)
        for language in ('English','Spanish','French','German'):
            self.assertIn(language,text)
        self.assertIn('current-word completions',text)
        self.assertIn('words this keyboard confirmed in the current field',text)
        self.assertIn('raw application completion preference',text)
        self.assertIn('Sensitive and restricted fields remain blocked',text)
        self.assertIn('VirtualKeyboardMode',text)
        self.assertIn('preserved',text)
        self.assertFalse(self.controller.state_file.exists())
        self.assertEqual(self.commands.settings['InputMethod'],'/old/keyboard.desktop')

    def test_native_arm_passes_same_compatibility_and_load_gates_as_x86(self):
        for machine in ('x86_64', 'aarch64'):
            with self.subTest(machine=machine), mock.patch.object(self.module.platform, 'machine', return_value=machine):
                self.controller.check()
                self.assertTrue(any(Path(call[0]).name == 'swipekb-preflight' for call in self.commands.calls))
                self.commands.preflight_error = 'native symbol unavailable'
                with self.assertRaisesRegex(self.module.SetupError, 'Native plugin load failed'):
                    self.controller.check()
                self.commands.preflight_error = None
        with mock.patch.object(self.module.platform, 'machine', return_value='riscv64'):
            with self.assertRaisesRegex(self.module.SetupError, 'x86_64 or aarch64'):
                self.controller.check()
        self.assertFalse(self.controller.data.exists(), 'Read-only checks never activate the keyboard')

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

    def test_profile_queries_distinguish_missing_failed_and_verified_utility_read_only(self):
        self.assertEqual(self.controller.profile_checks()['status'], 'utility-missing')
        utility = self.root/'integration/swipekb-profilecheck'
        utility.write_text('fixture')
        result = {'scope': 'queried-installation-capability', 'engineReady': 'unknown',
                  'activeFieldPolicy': 'unknown', 'swipeCapable': False,
                  'unavailableReason': 'Language pack manifest missing or invalid'}
        calls = []
        def query(args, **kwargs):
            calls.append(args)
            self.assertFalse(kwargs['check'])
            return subprocess.CompletedProcess(args, 1, json.dumps(result))
        self.controller.run = query
        report = self.controller.profile_checks()
        self.assertEqual(report['status'], 'queried')
        self.assertEqual(len(report['profiles']), 5)
        self.assertTrue(all(not p['swipeCapable'] for p in report['profiles']))
        self.assertEqual(len(calls), 5)
        self.assertFalse(self.controller.data.exists())
        self.controller.run = lambda *a, **k: subprocess.CompletedProcess(a, 2, 'bad output /private')
        self.assertEqual(self.controller.profile_checks()['status'], 'utility-failed')
        self.assertFalse(self.controller.data.exists())

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
        self.controller.enable(always_available=True)
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

    def test_check_reports_versions_and_selection_without_private_paths(self):
        (self.root / 'runtime.json').write_text(json.dumps({'qt': '6.12', 'package': '0.1.0-2'}))
        with mock.patch.object(self.module.platform, 'freedesktop_os_release', return_value={'PRETTY_NAME': 'Test Linux 44'}):
            report = self.controller.check()
        self.assertEqual(report['Distribution'], 'Test Linux 44')
        self.assertEqual(report['Package'], '0.1.0-2')
        self.assertEqual(report['Plasma Keyboard'], 'plasma-keyboard 6.7.5')
        self.assertEqual(report['Qt installed / build'], '6.12.0 / 6.12')
        self.assertEqual(report['Session'], 'KDE Plasma Wayland')
        self.assertEqual(report['Selected input method'], 'Other / stock')
        self.assertEqual(report['Setup record'], 'Absent')
        self.assertNotIn(str(self.base), str(report))
        self.assertFalse((self.base / 'data').exists())
        self.assertFalse(any(Path(c[0]).name in ('kwriteconfig6', 'busctl') for c in self.commands.calls))
        self.controller.enable(always_available=True)
        report = self.controller.check()
        self.assertEqual(report['Selected input method'], 'Swipe Keyboard')
        self.assertEqual(report['Setup record'], 'Present')

    def notification_failure(self, count, later_choice=None):
        original = self.controller.run
        remaining = count
        def runner(args, **kwargs):
            nonlocal remaining
            if args[:3] == ['busctl', '--user', 'emit'] and remaining:
                remaining -= 1
                if later_choice:
                    self.commands.settings.update(later_choice)
                    self.commands.live_mode = 2
                raise subprocess.CalledProcessError(1, args, stderr='notification failed')
            return original(args, **kwargs)
        self.controller.run = runner

    def test_notification_failure_rolls_back_enable(self):
        previous = dict(self.commands.settings)
        self.notification_failure(1)
        with self.assertRaisesRegex(self.module.SetupError, 'busctl failed'):
            self.controller.enable(always_available=True)
        self.assertEqual(self.commands.settings, previous)
        self.assertEqual(self.commands.live_mode, 0)
        self.assertFalse(self.controller.state_file.exists())

    def test_notification_failure_retains_journal_until_disable_retry(self):
        self.notification_failure(2)
        with self.assertRaises(self.module.SetupError):
            self.controller.enable(always_available=True)
        self.assertTrue(self.controller.state_file.exists())
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')
        self.controller.disable()
        self.assertFalse(self.controller.state_file.exists())

    def test_disable_notification_failure_preserves_later_choices_and_retry(self):
        self.controller.enable(always_available=True)
        later = {'InputMethod': '/later/keyboard.desktop', 'VirtualKeyboardMode': '2'}
        self.commands.settings.update(later)
        self.commands.live_mode = 2
        self.notification_failure(1)
        with self.assertRaises(self.module.SetupError):
            self.controller.disable()
        self.assertTrue(self.controller.state_file.exists())
        self.assertEqual(self.commands.settings, later)
        self.controller.disable()
        self.assertEqual(self.commands.settings, later)
        self.assertEqual(self.commands.live_mode, 2)
        self.assertFalse(self.controller.state_file.exists())

    def test_enable_notification_failure_rollback_preserves_concurrent_choice(self):
        later = {'InputMethod': '/later/keyboard.desktop', 'VirtualKeyboardMode': '2'}
        self.notification_failure(1, later)
        with self.assertRaises(self.module.SetupError):
            self.controller.enable(always_available=True)
        self.assertEqual(self.commands.settings, later)
        self.assertEqual(self.commands.live_mode, 2)
        self.assertFalse(self.controller.state_file.exists())

    def test_disable_notification_failure_after_restore_retains_journal(self):
        previous = dict(self.commands.settings)
        self.controller.enable(always_available=True)
        self.notification_failure(1)
        with self.assertRaises(self.module.SetupError):
            self.controller.disable()
        self.assertEqual(self.commands.settings, previous)
        self.assertTrue(self.controller.state_file.exists())
        self.controller.disable()
        self.assertEqual(self.commands.settings, previous)
        self.assertFalse(self.controller.state_file.exists())

    def test_check_validates_every_present_target_before_any_activation_write(self):
        for locale in ('en_GB', 'es_ES', 'fr_FR', 'de_DE'):
            with self.subTest(locale=locale):
                folder = self.stock / locale
                folder.mkdir()
                (folder / 'main.qml').write_text('unknown target layout')
                try:
                    with self.assertRaises(self.module.SetupError):
                        self.controller.check()
                    self.assertFalse((self.base / 'data').exists())
                    self.assertFalse(any(Path(c[0]).name in ('kwriteconfig6', 'busctl') for c in self.commands.calls))
                finally:
                    (folder / 'main.qml').unlink()
                    folder.rmdir()

    def test_launch_hooks_language_layouts_without_an_extra_language_gate(self):
        for locale, glyph in (('en_GB', '£'), ('es_ES', 'ñ'), ('fr_FR', 'a'), ('de_DE', 'z')):
            (self.stock / locale).mkdir()
            (self.stock / locale / 'main.qml').write_text(STOCK.replace('text: "q"', f'text: "{glyph}"'))
        env = self.controller.launch_environment()
        for locale, glyph in (('en_GB', '£'), ('es_ES', 'ñ'), ('fr_FR', 'a'), ('de_DE', 'z')):
            result = (self.controller.layouts / locale / 'main.qml').read_text()
            self.assertIn('SwipeInputMethod {}', result)
            self.assertIn(f'text: "{glyph}"', result)
        self.assertEqual(env['SWIPEKB_MODEL_DATA'], str(self.root))
        self.assertNotIn('SWIPEKB_LANGUAGE_PACKS', {k: v for k, v in env.items() if k not in self.env})
        self.assertFalse(any(Path(c[0]).name in ('kwriteconfig6', 'busctl') for c in self.commands.calls))
        self.assertFalse(self.controller.state_file.exists())

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
            self.controller.enable(always_available=True)
        self.commands.plasma_version = 'plasma-keyboard 6.7.5'
        self.commands.qt_version = '6.11.0'
        with self.assertRaises(self.module.SetupError):
            self.controller.enable(always_available=True)
        self.assertFalse((self.base / 'data').exists())
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_newer_same_major_qt_requires_successful_native_preflight(self):
        (self.root / 'runtime.json').write_text(json.dumps({'qt': '6.11'}))
        self.controller.check()
        self.assertTrue(any(Path(c[0]).name == 'swipekb-preflight' for c in self.commands.calls))
        self.assertFalse((self.base / 'data').exists())

    def test_unresolved_abi_refuses_read_only_before_activation(self):
        for error in ('libmissing.so: cannot open shared object file',
                      'GLIBCXX_3.4.32 not found', 'undefined symbol: a_new_KF_function'):
            with self.subTest(error=error):
                self.commands.preflight_error = error
                with self.assertRaisesRegex(self.module.SetupError, 'Native plugin load failed'):
                    self.controller.enable(always_available=True)
                self.assertFalse((self.base / 'data').exists())
                self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_missing_preflight_and_loader_timeout_fail_without_writes(self):
        helper = self.root / 'integration/swipekb-preflight'
        helper.unlink()
        with self.assertRaisesRegex(self.module.SetupError, 'Package files are missing'):
            self.controller.enable(always_available=True)
        with mock.patch.object(self.controller, 'run', side_effect=subprocess.TimeoutExpired('loader', 15, stderr=b'timed out')):
            with self.assertRaisesRegex(self.module.SetupError, 'Native plugin load failed'):
                self.controller.preflight()
        self.assertFalse((self.base / 'data').exists())
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_different_major_and_malformed_qt_are_rejected(self):
        for qt in ('7.0.0', '5.15.0', 'garbled'):
            self.commands.qt_version = qt
            with self.subTest(qt=qt), self.assertRaises(self.module.SetupError):
                self.controller.enable(always_available=True)
        self.assertFalse((self.base / 'data').exists())

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
            self.controller.enable(always_available=True)
        self.assertFalse((self.base / 'data').exists())
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_missing_payload_refuses_before_settings_change(self):
        (self.root / self.module.REQUIRED_ASSETS[0]).unlink()
        with self.assertRaises(self.module.SetupError):
            self.controller.enable(always_available=True)
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_existing_custom_layout_is_never_overwritten(self):
        custom = self.base / 'data/plasma/keyboard/layouts'
        custom.mkdir(parents=True)
        (custom / 'mine').write_text('leave alone')
        with self.assertRaises(self.module.SetupError):
            self.controller.enable(always_available=True)
        self.assertEqual((custom / 'mine').read_text(), 'leave alone')
        self.assertFalse((self.base / 'data/swipe-keyboard').exists())

    def test_inherited_defaults_restore_raw_absence_across_session_switch(self):
        self.commands.settings.clear()
        self.commands.defaults = {'InputMethod': '/mobile/stock.desktop', 'VirtualKeyboardMode': '2'}
        self.controller.enable(always_available=True)
        self.commands.defaults = {'InputMethod': '/desktop/stock.desktop', 'VirtualKeyboardMode': '0'}
        self.controller.disable()
        self.assertEqual(self.commands.settings, {})

    def test_explicit_empty_and_sentinel_values_round_trip(self):
        for value in ('', self.module.UNSET, '/mobile/stock.desktop'):
            with self.subTest(value=value):
                self.commands.settings = {'InputMethod': value, 'VirtualKeyboardMode': ''}
                self.commands.defaults = {'InputMethod': '/mobile/stock.desktop'}
                self.controller.enable(always_available=True)
                self.controller.disable()
                self.assertEqual(self.commands.settings, {'InputMethod': value, 'VirtualKeyboardMode': ''})

    def test_immutable_keys_refuse_before_any_mutation(self):
        for key in self.controller.wanted:
            with self.subTest(key=key):
                self.commands.immutable = {key}
                with self.assertRaisesRegex(self.module.SetupError, 'immutable'):
                    self.controller.enable(always_available=True)
                self.assertFalse(self.controller.state_file.exists())
                self.assertFalse(any('set-property' in call or Path(call[0]).name == 'kwriteconfig6'
                                     for call in self.commands.calls))

    def test_later_removal_is_preserved_even_when_effective_default_is_ours(self):
        self.controller.enable(always_available=True)
        self.commands.settings.clear()
        self.commands.defaults = dict(self.controller.wanted)
        self.controller.disable()
        self.assertEqual(self.commands.settings, {})

    def test_later_immutability_blocks_disable_without_live_mutation(self):
        self.controller.enable(always_available=True)
        self.commands.immutable = {'VirtualKeyboardMode'}
        calls = len(self.commands.calls)
        with self.assertRaisesRegex(self.module.SetupError, 'immutable'):
            self.controller.disable()
        self.assertTrue(self.controller.state_file.exists())
        self.assertFalse(any('set-property' in call for call in self.commands.calls[calls:]))

    def test_failed_activation_with_inherited_defaults_restores_absence(self):
        self.commands.settings.clear()
        self.commands.defaults = {'InputMethod': '/mobile/stock.desktop'}
        self.commands.fail_key = 'VirtualKeyboardMode'
        with self.assertRaises(self.module.SetupError):
            self.controller.enable(always_available=True)
        self.assertEqual(self.commands.settings, {})
        self.assertFalse(self.controller.state_file.exists())

    def legacy_record(self, previous, pending=None):
        self.controller.data.mkdir(parents=True, exist_ok=True)
        state = {'previous': previous, 'wanted': self.controller.wanted, 'live_mode': 2}
        if pending is not None:
            state['mode_restore'] = pending
        self.module.atomic_json(self.controller.state_file, state)
        self.commands.settings = dict(self.controller.wanted)
        self.commands.live_mode = 1

    def test_legacy_absence_record_and_interrupted_restore_are_recoverable(self):
        absent = {key: self.module.UNSET for key in self.controller.wanted}
        self.legacy_record(absent, {'before': '1', 'target': self.module.UNSET})
        self.commands.live_mode = 2
        self.commands.settings['VirtualKeyboardMode'] = '2'
        self.controller.disable()
        self.assertEqual(self.commands.settings, {})
        self.assertFalse(self.controller.state_file.exists())

    def test_legacy_ambiguous_value_retains_journal_without_mutation(self):
        for pending in (None, {'before': '1', 'target': '/possibly/inherited'}):
            with self.subTest(pending=pending):
                self.legacy_record({'InputMethod': '/possibly/inherited', 'VirtualKeyboardMode': '0'}, pending)
                before = self.controller.state_file.read_bytes()
                calls = len(self.commands.calls)
                with self.assertRaisesRegex(self.module.SetupError, 'no raw setting provenance'):
                    self.controller.disable()
                self.assertEqual(self.controller.state_file.read_bytes(), before)
                self.assertFalse(any('set-property' in call or Path(call[0]).name == 'kwriteconfig6'
                                     for call in self.commands.calls[calls:]))

    def test_legacy_later_choices_can_be_retired_without_guessing_previous_values(self):
        self.legacy_record({'InputMethod': '/possibly/inherited', 'VirtualKeyboardMode': '0'})
        self.commands.settings = {'InputMethod': '/later/choice', 'VirtualKeyboardMode': '2'}
        self.commands.live_mode = 2
        self.controller.disable()
        self.assertEqual(self.commands.settings, {'InputMethod': '/later/choice', 'VirtualKeyboardMode': '2'})

    def test_retry_preserves_later_removal_after_interrupted_restore(self):
        self.commands.live_mode = 2
        self.controller.enable(always_available=True)
        self.commands.fail_key = 'VirtualKeyboardMode'
        with self.assertRaises(self.module.SetupError):
            self.controller.disable()
        self.commands.settings.clear()
        self.commands.defaults = dict(self.controller.wanted)
        self.controller.disable()
        self.assertEqual(self.commands.settings, {})

    def test_existing_record_repeat_does_not_reclaim_removed_key(self):
        self.controller.enable(always_available=True)
        self.commands.settings.pop('InputMethod')
        self.commands.defaults = dict(self.controller.wanted)
        with self.assertRaisesRegex(self.module.SetupError, 'selection changed'):
            self.controller.enable(always_available=True)
        self.assertNotIn('InputMethod', self.commands.settings)

    def test_unavailable_or_malformed_probe_fails_closed(self):
        original = self.controller.run
        for failure in (FileNotFoundError('probe'), subprocess.CompletedProcess([], 0, '{}', '')):
            def runner(args, **kwargs):
                if Path(args[0]).name == 'swipekb-config':
                    if isinstance(failure, Exception):
                        raise failure
                    return failure
                return original(args, **kwargs)
            self.controller.run = runner
            with self.subTest(failure=failure), self.assertRaises(self.module.SetupError):
                self.controller.enable(always_available=True)
            self.assertFalse(self.controller.state_file.exists())
        self.assertFalse(any('set-property' in call for call in self.commands.calls))

    def test_enable_disable_restores_original_settings(self):
        original = dict(self.commands.settings)
        self.controller.enable(always_available=True)
        self.assertTrue(self.commands.settings['InputMethod'].endswith('/applications/swipe-keyboard.desktop'))
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '1')
        self.controller.enable(always_available=True)
        self.controller.disable()
        self.assertEqual(self.commands.settings, original)
        self.assertFalse(self.controller.state_file.exists())

    def test_enable_applies_live_visibility_and_disable_restores_it(self):
        self.commands.live_mode = 2
        self.controller.enable(always_available=True)
        self.assertEqual(self.commands.live_mode, 1)
        self.controller.disable()
        self.assertEqual(self.commands.live_mode, 2)
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '0')

    def test_interrupted_mode_restore_can_be_retried(self):
        self.commands.live_mode = 2
        self.controller.enable(always_available=True)
        self.commands.fail_key = 'VirtualKeyboardMode'
        with self.assertRaises(self.module.SetupError):
            self.controller.disable()
        self.assertTrue(self.controller.state_file.exists())
        self.controller.disable()
        self.assertEqual(self.commands.live_mode, 2)
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '0')
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_later_live_visibility_change_is_preserved(self):
        self.controller.enable(always_available=True)
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

    def test_plasma_keyboard_6_9_or_newer_steps_aside_to_the_plain_keyboard(self):
        # WHY: the packages no longer cap plasma-keyboard, so a Plasma 6.9 upgrade installs with this add-on present. From 6.9
        # KDE's keyboard has its own input engine, so launch must start it unchanged (no overlay, no plugin paths).
        for version in ('plasma-keyboard 6.9.0', 'plasma-keyboard 6.9.80', 'plasma-keyboard 6.10.1', 'plasma-keyboard 7.0.0'):
            with self.subTest(version=version):
                self.commands.plasma_version = version
                with self.assertRaises(self.module.SetupError) as raised:
                    self.controller.launch_environment()
                self.assertIn('plain KDE keyboard', str(raised.exception))
                self.assertFalse((self.base / 'data').exists())   # nothing built, nothing written
                with mock.patch.object(self.module, 'Controller') as factory, \
                     mock.patch.object(self.module.sys, 'argv', ['swipe-keyboard', 'launch']), \
                     mock.patch.object(self.module.os, 'execvpe', side_effect=SystemExit(0)) as execute, \
                     mock.patch('sys.stderr', new_callable=io.StringIO) as err:
                    factory.return_value.launch_environment.side_effect = raised.exception
                    with self.assertRaises(SystemExit):
                        self.module.main()
                execute.assert_called_once_with('plasma-keyboard', ['plasma-keyboard'], dict(os.environ))   # unmodified environment
                self.assertIn('Starting the unmodified stock keyboard instead', err.getvalue())
        self.commands.plasma_version = 'plasma-keyboard 6.8.9'
        self.controller.check()   # the supported range still passes
        self.commands.plasma_version = 'plasma-keyboard 6.7.0'
        self.controller.check()

    def test_disable_still_works_on_a_newer_plasma_keyboard(self):
        self.controller.enable(always_available=True)
        self.commands.plasma_version = 'plasma-keyboard 6.9.0'
        self.controller.disable()
        self.assertEqual(self.commands.settings['InputMethod'], '/old/keyboard.desktop')

    def test_fedora_dbus_command_name_is_supported(self):
        self.commands.fedora_commands = True
        self.controller.enable(always_available=True)
        self.controller.disable()
        self.assertTrue(any(Path(c[0]).name == 'qdbus-qt6' for c in self.commands.calls))

    def test_unset_settings_return_to_unset(self):
        self.commands.settings.clear()
        self.controller.enable(always_available=True)
        self.controller.disable()
        self.assertEqual(self.commands.settings, {})

    def test_disable_preserves_later_user_changes_and_private_data(self):
        self.controller.enable(always_available=True)
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
            self.controller.enable(always_available=True)
        self.assertEqual(self.commands.settings, original)
        self.assertFalse(self.controller.state_file.exists())

    def test_disable_does_not_need_installed_assets_or_supported_session(self):
        original = dict(self.commands.settings)
        self.controller.enable(always_available=True)
        (self.root / 'runtime.json').unlink()
        self.controller.env['XDG_SESSION_TYPE'] = 'x11'
        self.controller.disable()
        self.assertEqual(self.commands.settings, original)

    def test_launcher_separates_models_from_private_recordings(self):
        self.controller.enable(always_available=True)
        env = self.controller.launch_environment()
        self.assertEqual(env['SWIPEKB_MODEL_DATA'], str(self.root))
        self.assertEqual(env['SWIPEKB_DATA'], str(self.base / 'data/swipe-keyboard'))
        self.assertTrue(env['XDG_DATA_DIRS'].startswith(str(self.base / 'data/swipe-keyboard/overlay') + ':'))
        self.assertTrue(env['QML_IMPORT_PATH'].startswith(str(self.root / 'qml')))
        self.assertFalse((self.base / 'data/swipe-keyboard/record').exists())

    def test_launch_preserves_touch_samples_only_in_child_environment(self):
        before = dict(os.environ)
        self.controller.enable()
        settings = dict(self.commands.settings)
        self.controller.env['QT_WAYLAND_RECONNECT'] = '1'  # Plasma session default
        env = self.controller.launch_environment()
        self.assertNotIn('QT_WAYLAND_RECONNECT', env)
        self.assertEqual(self.controller.env.pop('QT_WAYLAND_RECONNECT'), '1')
        self.assertEqual(env.get('QML_NO_TOUCH_COMPRESSION'), '1')
        self.assertEqual(env.get('QT_WAYLAND_FRAME_CALLBACK_TIMEOUT'), '5000')
        self.assertNotIn('QT_WAYLAND_FRAME_CALLBACK_TIMEOUT', self.controller.env)
        self.assertEqual(env.get('SWIPEKB_NATIVE_PREDICTION'), 'owned-history-v1')
        self.assertNotIn('SWIPEKB_NATIVE_PREDICTION', self.controller.env)
        self.assertEqual(dict(os.environ), before)
        self.assertNotIn('QML_NO_TOUCH_COMPRESSION', self.controller.env)
        self.assertEqual(self.commands.settings, settings)
        self.assertEqual(self.commands.live_mode, 0)

    def test_launch_regenerates_layout_after_stock_update(self):
        self.controller.enable(always_available=True)
        (self.stock / 'fallback/main.qml').write_text(STOCK.replace('text: "q"', 'text: "Q"'))
        self.controller.launch_environment()
        self.assertIn('text: "Q"', (self.base / 'data/swipe-keyboard/overlay/plasma/keyboard/layouts/en_US/main.qml').read_text())

    def test_enable_does_not_replace_changed_keyboard_on_repeat(self):
        self.controller.enable(always_available=True)
        self.commands.settings['InputMethod'] = '/user/new-choice.desktop'
        with self.assertRaises(self.module.SetupError):
            self.controller.enable(always_available=True)
        self.assertEqual(self.commands.settings['InputMethod'], '/user/new-choice.desktop')


if __name__ == '__main__':
    unittest.main()
