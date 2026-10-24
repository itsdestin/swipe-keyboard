# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Native KConfig semantics, only private HOME/XDG trees; never a desktop bus."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
# Observed Fedora Mobile raw baseline, including its ordinary nested Tiling group.
BASELINE = '''[Desktops]
Id_1=96de767c-1b20-4035-90ea-bb034e924ffd
Number=1
Rows=1

[Tiling][96de767c-1b20-4035-90ea-bb034e924ffd][fb6e0f7c-a59b-403c-b96e-9696075ac464]
padding=4
tiles={"layoutDirection":"horizontal","tiles":[{"width":0.25},{"width":0.5},{"width":0.25}]}

[Xwayland]
Scale=1
'''



class NativeConfig(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build_temp.cleanup)
        cls.helper = os.environ.get('SWIPEKB_TEST_CONFIG')
        if not cls.helper:
            build = Path(cls.build_temp.name) / 'build'
            subprocess.run(['cmake', '-S', str(ROOT / 'integration'), '-B', str(build)],
                           check=True, capture_output=True)
            subprocess.run(['cmake', '--build', str(build), '--parallel', '2'],
                           check=True, capture_output=True)
            cls.helper = str(build / 'swipekb-config')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.user = self.base / 'user'
        self.high = self.base / 'mobile'
        self.low = self.base / 'system'
        for path in (self.user, self.high, self.low):
            path.mkdir()
        self.env = {key: value for key, value in os.environ.items()
                    if key not in ('DBUS_SESSION_BUS_ADDRESS', 'DISPLAY', 'WAYLAND_DISPLAY')}
        self.env.update(HOME=str(self.base), XDG_CONFIG_HOME=str(self.user),
                        XDG_CONFIG_DIRS=f'{self.high}:{self.low}', QT_QPA_PLATFORM='offscreen')

    def probe(self):
        result = subprocess.run([self.helper], env=self.env, text=True, capture_output=True, check=True)
        return json.loads(result.stdout)

    def test_unsupported_user_provenance_is_refused_by_real_probe(self):
        cases = ('[Wayland]\nInputMethod[$d]\n',
                 '[Wayland]\nVirtualKeyboardMode[$d]=\n', '[Wayland][$d]\n',
                 '[Wayland]\nInputMethod[$e]=$HOME/example.desktop\n',
                 '[Wayland]\nVirtualKeyboardMode[$e]=$MODE\n',
                 '[Wayland]\nInputMethod[de]=localized.desktop\n',
                 '[Wayland]\nInputMethod[en_US][$e]=$HOME/example.desktop\n',
                 '[Wayland]\nInputMethod[$de]=$HOME/example.desktop\n',
                 '[Wayland]\nInputMethod[$i][$d]=\n',
                 '[Wayland] [$d]\n', r'[Way\x6cand]' + '\nInputMethod=value\n',
                 '[Wayland]\nInputMethod[future-flag]=value\n',
                 '[Wayland]\nInputMethod\\x5b$e\\x5d=value\n')
        (self.high / 'kwinrc').write_text('[Wayland]\nInputMethod=inherited\nVirtualKeyboardMode=2\n')
        for content in cases:
            with self.subTest(content=content):
                (self.user / 'kwinrc').write_text(content)
                result = subprocess.run([self.helper], env=self.env, text=True, capture_output=True)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn('Unsupported user KConfig syntax', result.stderr)

    def test_native_kconfig_demonstrates_deletion_and_expansion_provenance(self):
        (self.high / 'kwinrc').write_text('[Wayland]\nInputMethod=inherited\n')
        path = self.user / 'kwinrc'
        def native_read():
            return subprocess.run(['kreadconfig6', '--file', 'kwinrc', '--group', 'Wayland',
                                   '--key', 'InputMethod', '--default', 'ABSENT'],
                                  env=self.env, check=True, text=True, capture_output=True).stdout.rstrip('\n')
        self.assertEqual(native_read(), 'inherited')
        path.write_text('[Wayland]\nInputMethod[$d]\n')
        self.assertEqual(native_read(), 'ABSENT')
        path.write_text('[Wayland]\nInputMethod[$e]=$HOME/example.desktop\n')
        self.assertEqual(native_read(), str(self.base / 'example.desktop'))
        path.write_text('[Wayland]\nInputMethod=literal[$d]\\s[$e]\n')
        self.assertEqual(self.probe()['InputMethod']['raw'], 'literal[$d] [$e]')

    def test_controller_refuses_unsupported_initial_and_later_user_state(self):
        import shutil
        from test_setup import SetupTests
        for content in ('[Wayland]\nInputMethod[$d]\n', '[Wayland][$d]\n',
                        '[Wayland]\nInputMethod[$e]=$HOME/example.desktop\n',
                        '[Wayland]\nInputMethod[de]=localized.desktop\n'):
            for later in (False, True):
                with self.subTest(content=content, later=later):
                    fixture = SetupTests('test_enable_disable_restores_original_settings')
                    fixture.setUp()
                    try:
                        controller = fixture.controller
                        for key in ('DBUS_SESSION_BUS_ADDRESS', 'DISPLAY', 'WAYLAND_DISPLAY'):
                            controller.env.pop(key, None)
                        controller.env['XDG_CONFIG_DIRS'] = str(self.high)
                        config = Path(controller.env['XDG_CONFIG_HOME']) / 'kwinrc'
                        config.parent.mkdir(parents=True)
                        if later:
                            controller.enable()  # fake activation, then switch to real probe
                        shutil.copy2(self.helper, controller.root / 'integration/swipekb-config')
                        original = controller.run
                        def runner(args, **kwargs):
                            if Path(args[0]).name == 'swipekb-config':
                                return subprocess.run(args, **kwargs)
                            return original(args, **kwargs)
                        controller.run = runner
                        config.write_text(content)
                        before = config.read_bytes()
                        journal = controller.state_file.read_bytes() if later else None
                        calls = len(fixture.commands.calls)
                        with self.assertRaisesRegex(fixture.module.SetupError, 'Unsupported user KConfig syntax'):
                            (controller.disable if later else controller.enable)()
                        self.assertEqual(config.read_bytes(), before)
                        self.assertEqual(controller.state_file.read_bytes() if later else None, journal)
                        self.assertFalse(any('set-property' in call or Path(call[0]).name == 'kwriteconfig6'
                                             for call in fixture.commands.calls[calls:]))
                    finally:
                        fixture.doCleanups()

    def test_empty_nested_groups_preserve_real_wayland_values(self):
        # WHY: KWin legitimately writes [Tiling][output-id][]. These nested
        # groups must not reject setup or alias the root Wayland ownership view.
        path = self.user / 'kwinrc'
        for header in ('[Tiling][synthetic-output][]', '[Wayland][]', '[Wayland][Nested][]'):
            with self.subTest(header=header):
                path.write_text('[Wayland]\nInputMethod=plain.desktop\nVirtualKeyboardMode=1\n'
                                + header + '\nInputMethod[$e]=$HOME/unrelated.desktop\n')
                before = path.read_bytes()
                result = subprocess.run([self.helper], env=self.env, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                state = json.loads(result.stdout)
                self.assertEqual(state['InputMethod'],
                                 {'raw': 'plain.desktop', 'effective': 'plain.desktop', 'immutable': False})
                self.assertEqual(state['VirtualKeyboardMode']['raw'], '1')
                self.assertEqual(path.read_bytes(), before)
                path.write_text(path.read_text() + '[Wayland]\nInputMethod[$e]=$HOME/relevant.desktop\n')
                refused = subprocess.run([self.helper], env=self.env, text=True, capture_output=True)
                self.assertEqual(refused.returncode, 2)

    def test_nested_unrelated_groups_do_not_hide_later_relevant_flags(self):
        path = self.user / 'kwinrc'
        unrelated = BASELINE + '[Wayland][Nested]\nInputMethod[$e]=$HOME/unrelated\n'
        path.write_text(unrelated)
        before = path.read_bytes()
        self.assertIsNone(self.probe()['InputMethod']['raw'])
        self.assertEqual(path.read_bytes(), before)
        for entry in ('InputMethod[$d]', 'InputMethod[$e]=$HOME/a', 'InputMethod[de]=a'):
            path.write_text(unrelated + '[Wayland]\n' + entry + '\n')
            result = subprocess.run([self.helper], env=self.env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 2)

    def test_raw_absent_and_effective_precedence_session_switch(self):
        (self.low / 'kwinrc').write_text('[Wayland]\nInputMethod=system\n')
        (self.high / 'kwinrc').write_text('[Wayland]\nInputMethod=mobile\nVirtualKeyboardEnabled[$i]=true\n')
        self.assertEqual(self.probe()['InputMethod'], {'raw': None, 'effective': 'mobile', 'immutable': False})
        self.assertFalse(self.probe()['VirtualKeyboardMode']['immutable'])
        self.env['XDG_CONFIG_DIRS'] = str(self.low)
        self.assertEqual(self.probe()['InputMethod']['effective'], 'system')

    def test_native_probe_installs_as_executable(self):
        # WHY a plain source copy: archive users have no Git history or parent checkout.
        import shutil
        source = self.base / 'source'
        source.mkdir()
        for name in ('CMakeLists.txt', 'config.cpp'):
            shutil.copy2(ROOT / 'integration' / name, source / name)
        build = self.base / 'build'
        subprocess.run(['cmake', '-S', str(source), '-B', str(build)],
                       check=True, capture_output=True)
        subprocess.run(['cmake', '--build', str(build), '--parallel', '2'], check=True, capture_output=True)
        subprocess.run(['cmake', '--install', str(build), '--prefix', str(self.base / 'stage')],
                       check=True, capture_output=True)
        installed = self.base / 'stage/share/swipe-keyboard/integration/swipekb-config'
        self.assertTrue(os.access(installed, os.X_OK))

    def test_unreadable_or_invalid_source_is_not_reported_as_absent(self):
        for location in (self.user, self.high, self.low):
            with self.subTest(location=location):
                path = location / 'kwinrc'
                path.symlink_to(location / 'missing-target')
                result = subprocess.run([self.helper], env=self.env, text=True, capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn('ownership cannot be established', result.stderr)
                path.unlink()

    def test_globals_immutability_and_user_xdg_precedence(self):
        (self.low / 'kdeglobals').write_text('[Wayland]\nInputMethod[$i]=global-lock\n')
        (self.user / 'kwinrc').write_text('[Wayland]\nInputMethod=user\n')
        self.assertTrue(self.probe()['InputMethod']['immutable'])
        (self.low / 'kdeglobals').unlink()
        self.assertEqual(self.probe()['InputMethod']['effective'], 'user')

    def test_explicit_empty_whitespace_and_sentinel(self):
        for value, encoded in [('', ''), ('  value  ', '\\s\\svalue\\s\\s'),
                               ('__SWIPEKB_UNSET__', '__SWIPEKB_UNSET__')]:
            (self.user / 'kwinrc').write_text(f'[Wayland]\nInputMethod={encoded}\n')
            self.assertEqual(self.probe()['InputMethod']['raw'], value)

    def test_native_immutable_key_group_and_file_in_user_and_overlays(self):
        for location in (self.user, self.high, self.low):
            for content in ('[Wayland]\nInputMethod[$i]=locked\n',
                            '[Wayland][$i]\nInputMethod=locked\n',
                            '[$i]\n[Wayland]\nInputMethod=locked\n'):
                with self.subTest(location=location, content=content):
                    path = location / 'kwinrc'
                    path.write_text(content)
                    self.assertTrue(self.probe()['InputMethod']['immutable'])
                    path.unlink()

    def test_immutable_lower_overlay_overrides_explicit_user_choice(self):
        (self.low / 'kwinrc').write_text('[Wayland]\nInputMethod[$i]=locked\n')
        (self.user / 'kwinrc').write_text('[Wayland]\nInputMethod=user\n')
        self.assertEqual(self.probe()['InputMethod'], {'raw': 'user', 'effective': 'locked', 'immutable': True})

    def test_controller_round_trip_uses_real_kconfig_without_live_bus(self):
        import shutil
        from test_setup import SetupTests
        for previous in (None, '', '__SWIPEKB_UNSET__', 'mobile'):
            with self.subTest(previous=previous):
                fixture = SetupTests('test_enable_disable_restores_original_settings')
                fixture.setUp()
                try:
                    controller = fixture.controller
                    for key in ('DBUS_SESSION_BUS_ADDRESS', 'DISPLAY', 'WAYLAND_DISPLAY'):
                        controller.env.pop(key, None)
                    controller.env['XDG_CONFIG_DIRS'] = str(self.high)
                    config = Path(controller.env['XDG_CONFIG_HOME']) / 'kwinrc'
                    config.parent.mkdir(parents=True)
                    (self.high / 'kwinrc').write_text('[Wayland]\nInputMethod=mobile\nVirtualKeyboardMode=1\n')
                    config.write_text(BASELINE + (f'\n[Wayland]\nInputMethod={previous}\n' if previous is not None else ''))
                    shutil.copy2(self.helper, controller.root / 'integration/swipekb-config')
                    fake = fixture.commands
                    def runner(args, **kwargs):
                        name = Path(args[0]).name
                        if name == 'swipekb-config':
                            return subprocess.run(args, **kwargs)
                        if name == 'kwriteconfig6':
                            # WHY no --notify in tests: use real file semantics, never contact a host bus.
                            return subprocess.run([arg for arg in args if arg != '--notify'], **kwargs)
                        result = fake(args, **kwargs)
                        if name == 'busctl' and 'set-property' in args:
                            subprocess.run(['kwriteconfig6', '--file', str(config), '--group', 'Wayland',
                                            '--key', 'VirtualKeyboardMode', args[-1]],
                                           env=controller.env, check=True, capture_output=True)
                        return result
                    controller.run = runner
                    controller.enable()
                    # KConfig may reorder groups on write; compare native unrelated values.
                    def unrelated():
                        return [subprocess.run(['kreadconfig6', '--file', str(config), *groups,
                                                '--key', key], env=controller.env, check=True,
                                               capture_output=True, text=True).stdout
                                for groups, key in ((['--group', 'Desktops'], 'Id_1'),
                                    (['--group', 'Tiling', '--group', '96de767c-1b20-4035-90ea-bb034e924ffd',
                                      '--group', 'fb6e0f7c-a59b-403c-b96e-9696075ac464'], 'tiles'),
                                    (['--group', 'Xwayland'], 'Scale'))]
                    enabled_unrelated = unrelated()
                    self.assertIn('horizontal', enabled_unrelated[1])
                    saved = json.loads(controller.state_file.read_text())
                    self.assertEqual(saved['previous']['InputMethod'], previous)
                    self.assertIsNone(saved['previous']['VirtualKeyboardMode'])
                    controller.env['XDG_CONFIG_DIRS'] = str(self.low)
                    controller.disable()
                    state = controller.config_state()
                    self.assertEqual(state['InputMethod']['raw'], previous)
                    self.assertIsNone(state['VirtualKeyboardMode']['raw'])
                    self.assertEqual(unrelated(), enabled_unrelated)
                finally:
                    fixture.doCleanups()

    def test_raw_user_immutable_not_hidden_by_overlay(self):
        (self.low / 'kwinrc').write_text('[Wayland]\nInputMethod[$i]=locked\n')
        (self.user / 'kwinrc').write_text('[Wayland]\nVirtualKeyboardMode[$i]=2\n')
        self.assertTrue(self.probe()['VirtualKeyboardMode']['immutable'])


if __name__ == '__main__':
    unittest.main()
