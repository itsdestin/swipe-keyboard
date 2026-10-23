"""Packaging contracts for Gaming Mode (plan docs/active/plans/2026-10-22-gaming-mode.md, Sections 4, 5, 9).

These read committed files only: no Flatpak, systemd or network is needed, so they run in every package job.
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT / 'packaging/flatpak'
MAIN_ID = 'org.itsdestin.SwipeKeyboard'
HELPER_ID = 'org.itsdestin.SwipeKeyboard.GamingHelper'
SHARED_DIR = '--filesystem=xdg-run/swipekb:create'
# WHY (found in the SteamOS VM, M8): Flatpak points XDG_STATE_HOME at the app's private folder, so without this the
# status file and the installer's outside-access choice live in two different places and never meet.
STATE_DIR = '--filesystem=~/.local/state/swipekb-gaming:create'


def load(name):
    return json.loads((BUNDLE / name).read_text())


class MainFlatpakGamingTests(unittest.TestCase):
    def setUp(self):
        self.main = load(MAIN_ID + '.json')

    def test_main_app_shares_only_the_socket_directory_and_has_no_network(self):
        # WHY: the engine daemon lives in the main app, which must never reach the network (plan Section 9).
        # Its socket must sit in the directory shared with the helper app, or the helper cannot find it.
        args = self.main['finish-args']
        self.assertIn(SHARED_DIR, args)
        for arg in args:
            self.assertNotIn('network', arg)
            self.assertNotIn('--share=', arg)
            self.assertNotIn('--device', arg)
            self.assertNotIn('--filesystem=home', arg)
            self.assertNotIn('--filesystem=host', arg)
        self.assertEqual([a for a in args if a.startswith('--filesystem=')], [SHARED_DIR])

    def test_daemon_is_built_installed_and_elf_checked(self):
        script = (BUNDLE / 'build-swipe.sh').read_text()
        self.assertIn('-DSWIPEKB_INSTALL_GAMINGD=ON', script)
        # WHY: the ELF/Qt-ABI check list is the only gate on installed bytes in the Flatpak build.
        elf_line = next(l for l in script.splitlines() if 'package-check.py --elf' in l)
        self.assertIn('/app/bin/swipekb-gamingd', elf_line)


class HelperFlatpakTests(unittest.TestCase):
    def setUp(self):
        self.helper = load(HELPER_ID + '.json')

    def test_identity_matches_main_runtime_so_one_runtime_is_installed(self):
        main = load(MAIN_ID + '.json')
        self.assertEqual(self.helper['id'], HELPER_ID)
        for key in ('runtime', 'runtime-version', 'sdk'):
            self.assertEqual(self.helper[key], main[key])
        self.assertEqual(self.helper['command'], 'swipekb-helper')

    def test_network_is_the_only_extra_permission(self):
        # WHY: --share=network is the only way a sandbox reaches the host's loopback port to Steam (plan
        # Section 9) and Flatpak cannot narrow it to localhost. Everything else the helper gets is
        # the shared socket directory it needs to reach the engine. No display, no home, no devices.
        args = self.helper['finish-args']
        granted = {a for a in args if not a.startswith(('--nofilesystem=', '--no-talk-name='))}
        self.assertEqual(granted, {'--share=network', SHARED_DIR, STATE_DIR})
        # WHY subtract: the KDE runtime grants styling config and bus names by default; the helper uses none.
        main = load(MAIN_ID + '.json')['finish-args']
        for arg in args:
            if arg.startswith(('--nofilesystem=', '--no-talk-name=')):
                self.assertIn(arg, main)

    def test_helper_is_built_from_gaming_helper_with_a_clear_failure_when_absent(self):
        module = self.helper['modules'][0]
        self.assertEqual(module['buildsystem'], 'simple')
        self.assertIn('sh packaging/flatpak/build-helper.sh', module['build-commands'])
        gaming = next(s for s in module['sources'] if s.get('path') == '../../gaming')
        self.assertEqual(gaming['type'], 'dir')
        self.assertEqual(gaming['dest'], 'gaming')
        # WHY: the engine daemon source must not be copied into the helper bundle.
        self.assertIn('daemon', gaming['skip'])
        # WHY no archives/urls: the helper is stdlib-only Python, so nothing is downloaded at build time.
        for source in module['sources']:
            self.assertNotIn('url', source)

    def run_build_helper(self, files):
        script = BUNDLE / 'build-helper.sh'
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory) / 'work'
            prefix = Path(directory) / 'app'
            for name, text in files.items():
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            work.mkdir(exist_ok=True)
            result = subprocess.run(['sh', str(script)], cwd=work, capture_output=True, text=True,
                                    env={'PATH': os.environ['PATH'], 'SWIPEKB_HELPER_PREFIX': str(prefix)})
            installed = sorted(str(p.relative_to(prefix)) for p in prefix.rglob('*')) if prefix.exists() else []
            wrapper = prefix / 'bin/swipekb-helper'
            return result, installed, wrapper.read_text() if wrapper.exists() else None, \
                os.access(wrapper, os.X_OK) if wrapper.exists() else False

    def test_build_fails_clearly_and_installs_nothing_when_helper_source_is_absent(self):
        result, installed, _, _ = self.run_build_helper({'gaming/contract/gaming-contract.json': '{}'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('gaming/helper/swipekb_helper/__main__.py', result.stderr)
        self.assertEqual(installed, [])

    def test_build_installs_package_contract_and_wrapper(self):
        result, installed, wrapper, executable = self.run_build_helper({
            'gaming/helper/swipekb_helper/__main__.py': 'print(1)\n',
            'gaming/helper/swipekb_helper/__init__.py': '',
            'gaming/contract/gaming-contract.json': '{}'})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('lib/swipekb-helper/swipekb_helper/__main__.py', installed)
        self.assertIn('share/swipekb-helper/gaming-contract.json', installed)
        self.assertTrue(executable)
        # WHY exact PYTHONPATH and no inherited one: the helper talks to Steam's debug port, so it must not
        # import code from a path the caller's environment supplies.
        self.assertIn('/app/lib/swipekb-helper', wrapper)
        self.assertNotIn('${PYTHONPATH', wrapper)
        self.assertIn('python3 -I', wrapper)
        # WHY: the status file must land where the installer and the Desktop settings page look, not in the sandbox's private folder.
        self.assertIn('SWIPEKB_STATE_DIR="$HOME/.local/state/swipekb-gaming"', wrapper)

    def test_wrapper_runs_the_helper_in_isolated_mode_importing_nothing_from_the_caller(self):
        """Review fix 8: `python3 -s -m` still puts the current folder (and PYTHONPATH) on the import path; -I does not."""
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            work, prefix, hostile, cwd = base / 'work', base / 'app', base / 'hostile', base / 'cwd'
            for d in (hostile, cwd):
                d.mkdir()
                (d / 'json.py').write_text('print("PLANTED")\n')
            pkg = work / 'gaming/helper/swipekb_helper'
            pkg.mkdir(parents=True)
            (pkg / '__init__.py').write_text('')
            # the stand-in helper reports where the standard library's json came from and which package it was started as
            (pkg / '__main__.py').write_text('import json, sys\nprint(json.__file__)\nprint(__package__)\n')
            (work / 'gaming/contract').mkdir(parents=True)
            (work / 'gaming/contract/gaming-contract.json').write_text('{}')
            built = subprocess.run(['sh', str(BUNDLE / 'build-helper.sh')], cwd=work, capture_output=True, text=True,
                                   env={'PATH': os.environ['PATH'], 'SWIPEKB_HELPER_PREFIX': str(prefix)})
            self.assertEqual(built.returncode, 0, built.stderr)
            wrapper = prefix / 'bin/swipekb-helper'
            # inside Flatpak the app lives at /app; here the same file is pointed at the temporary copy
            wrapper.write_text(wrapper.read_text().replace('/app/', str(prefix) + '/'))
            run = subprocess.run(['sh', str(wrapper)], cwd=cwd, capture_output=True, text=True,
                                 env={'PATH': os.environ['PATH'], 'HOME': str(base), 'PYTHONPATH': str(hostile)})
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertNotIn('PLANTED', run.stdout)
            lines = run.stdout.split()
            self.assertNotIn(str(cwd), lines[0])
            self.assertNotIn(str(hostile), lines[0])
            self.assertEqual(lines[1], 'swipekb_helper', 'the package still starts as the helper')


class SystemdUnitTests(unittest.TestCase):
    UNITS = ROOT / 'gaming/setup/units'

    def unit(self, name):
        return (self.UNITS / name).read_text()

    def test_units_name_flatpak_app_ids_never_version_paths(self):
        # WHY: a Flatpak upgrade replaces the deployed files; a unit that named a deploy path or commit
        # would silently break on the next update (plan Section 5).
        engine = self.unit('swipekb-engine.service')
        helper = self.unit('swipekb-helper.service')
        self.assertIn('ExecStart=/usr/bin/flatpak run --die-with-parent --user --command=swipekb-gamingd ' + MAIN_ID + '\n', engine)
        self.assertIn('ExecStart=/usr/bin/flatpak run --die-with-parent --user --command=swipekb-helper ' + HELPER_ID + '\n', helper)
        for text in (engine, helper):
            self.assertNotRegex(text, r'/deploy|/var/lib/flatpak|/\.local/share/flatpak|[0-9a-f]{40}|/home/')

    def test_units_need_no_session_environment(self):
        # WHY: M0 measured these units starting about 6.6 s after boot, before any Gaming Mode session
        # existed (no DISPLAY, WAYLAND_DISPLAY or gamescope). They must not wait for or require one.
        for name in ('swipekb-engine.service', 'swipekb-helper.service'):
            text = self.unit(name)
            self.assertIn('WantedBy=default.target', text)
            self.assertNotRegex(text, r'graphical-session|DISPLAY|WAYLAND|DESKTOP_SESSION|XDG_CURRENT')
            self.assertNotRegex(text, r'(?m)^Environment')
            self.assertIn('Restart=always', text)
            self.assertIn('RestartSec=3', text)
            # WHY no start limit but a growing wait (review fix 5): a limit of 30 starts in 300 s was reached after about 90 s of a
            # crash loop and left the unit dead with no explanation. Instead the wait between tries grows from 3 s to a few
            # minutes, which is just as gentle on a handheld battery if the Flatpak was removed without running uninstall.
            self.assertIn('StartLimitIntervalSec=0', text)
            self.assertNotIn('StartLimitBurst', text)
            self.assertRegex(text, r'(?m)^RestartSteps=\d+$')
            self.assertRegex(text, r'(?m)^RestartMaxDelaySec=\d+$')

    def test_units_carry_ownership_and_version_stamp_for_safe_upgrade_and_uninstall(self):
        # WHY: install.sh and uninstall.sh only overwrite or delete a unit that carries this stamp.
        for name in ('swipekb-engine.service', 'swipekb-helper.service'):
            lines = self.unit(name).splitlines()
            self.assertEqual(lines[0], '# swipekb-gaming-managed')
            self.assertEqual(lines[1], '# swipekb-gaming-version: @VERSION@')

    def test_helper_stop_asks_it_to_clean_up_first_and_cannot_fail_the_stop(self):
        # WHY (review fix 8): --die-with-parent turns SIGTERM into an instant kill; the leading "-" means a missing script cannot
        # make the stop itself fail.
        self.assertIn('\nExecStop=-@WATCH_SCRIPT@ stop-helper\n', self.unit('swipekb-helper.service'))

    def test_helper_starts_after_engine_but_does_not_require_it(self):
        helper = self.unit('swipekb-helper.service')
        self.assertIn('After=swipekb-engine.service', helper)
        self.assertIn('Wants=swipekb-engine.service', helper)
        self.assertNotIn('Requires=', helper)


class NativePackagesShipNoGamingTests(unittest.TestCase):
    """Plan Section 4: Arch and Fedora stay exactly as they were. SteamOS is Flatpak-only."""

    def test_cmake_daemon_install_is_off_by_default_and_only_the_flatpak_enables_it(self):
        cmake = (ROOT / 'gaming/daemon/CMakeLists.txt').read_text()
        self.assertRegex(cmake, r'option\(SWIPEKB_INSTALL_GAMINGD [^\n]*OFF\)')
        self.assertRegex(cmake, r'if\(SWIPEKB_INSTALL_GAMINGD\)\s+install\(')
        for recipe in ('packaging/arch/PKGBUILD', 'packaging/fedora/swipe-keyboard.spec', 'scripts/arch-dependencies.py'):
            self.assertNotIn('SWIPEKB_INSTALL_GAMINGD', (ROOT / recipe).read_text(), recipe)
        self.assertIn('-DSWIPEKB_INSTALL_GAMINGD=ON', (BUNDLE / 'build-swipe.sh').read_text())

    def test_native_recipes_name_no_gaming_file(self):
        for recipe in ('packaging/arch/PKGBUILD', 'packaging/fedora/swipe-keyboard.spec'):
            text = (ROOT / recipe).read_text().lower()
            for word in ('gaming', 'swipekb-helper', 'xdg-run', 'cef-enable'):
                self.assertNotIn(word, text, f'{recipe} mentions {word}')

    def load_package_check(self):
        spec = importlib.util.spec_from_file_location('package_check_gaming', ROOT / 'scripts/package-check.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_package_check_rejects_gaming_files_in_a_native_package(self):
        # WHY a behavioural guard on real package bytes: the recipe text checks above can pass while a
        # build option or glob still pulls a gaming file into the package. package-check runs on the
        # staged Arch and Fedora packages in CI.
        checker = self.load_package_check()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'usr/bin').mkdir(parents=True)
            (root / 'usr/bin/swipe-keyboard').write_text('x')
            checker.forbid_gaming_payload(root)  # clean package passes
            for bad in ('usr/bin/swipekb-gamingd', 'usr/lib/systemd/user/swipekb-engine.service',
                        'usr/lib/systemd/user/swipekb-helper.service', 'usr/share/swipe-keyboard/gaming/x',
                        'usr/share/swipekb-helper/gaming-contract.json', 'usr/bin/swipekb-helper'):
                path = root / bad
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('x')
                with self.assertRaisesRegex(ValueError, 'Gaming Mode'):
                    checker.forbid_gaming_payload(root)
                path.unlink()

    def test_package_check_runs_the_guard_on_every_native_package(self):
        source = (ROOT / 'scripts/package-check.py').read_text()
        body = source[source.index('def check(root)'):]
        self.assertIn('forbid_gaming_payload(root)', body)


if __name__ == '__main__':
    unittest.main()
