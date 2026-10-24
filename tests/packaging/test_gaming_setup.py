# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Run gaming/setup/install.sh and uninstall.sh against a throwaway HOME with stand-in `flatpak` and `systemctl`.

WHY stand-ins and a PATH that holds nothing else: the scripts must be provable without touching this computer's
real Flatpak, systemd or Steam. The PATH given to the scripts contains only symlinks to a few basic tools plus the
stand-ins, so a real `flatpak` or `systemctl` can never be reached by accident.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SETUP = ROOT / 'gaming/setup'
MAIN_ID = 'org.itsdestin.SwipeKeyboard'
HELPER_ID = 'org.itsdestin.SwipeKeyboard.GamingHelper'
MARKER = '.cef-enable-remote-debugging'
TOOLS = ['bash', 'sh', 'sed', 'grep', 'sha256sum', 'readlink', 'date', 'mkdir', 'rm', 'rmdir', 'cp', 'cat',
         'dirname', 'basename', 'sort', 'head', 'tr', 'mktemp', 'chmod', 'touch', 'ln', 'tail', 'cut', 'mv',
         'env', 'uname', 'id', 'printf', 'tee', 'find', 'wc', 'cmp', 'awk', 'ls']

# WHY logging and a tiny "installed apps" file: the scripts ask `flatpak info` whether an app is already there.
FLATPAK = '''#!/bin/sh
printf 'flatpak %s\\n' "$*" >> "$FAKE_LOG"
case "$1" in
  info) grep -qx "$3" "$FAKE_APPS" 2>/dev/null; exit $? ;;
  install) [ "$FAKE_FAIL" = install ] && { echo 'fake: install failed' >&2; exit 1; }
           case "$*" in *GamingHelper*|*gaming-helper*) id=org.itsdestin.SwipeKeyboard.GamingHelper;; *) id=org.itsdestin.SwipeKeyboard;; esac
           # WHY: the real Flatpak refuses to install a bundle whose exact version is already here (seen on SteamOS in M8).
           [ "$FAKE_FAIL" = same-version ] && grep -qx "$id" "$FAKE_APPS" 2>/dev/null && { echo "error: Failed to install bundle $id: $id already installed" >&2; exit 1; }
           echo "$id" >> "$FAKE_APPS" ;;
  uninstall) for last; do :; done; grep -vx "$last" "$FAKE_APPS" > "$FAKE_APPS.new" 2>/dev/null; mv "$FAKE_APPS.new" "$FAKE_APPS" ;;
esac
exit 0
'''
# WHY the root marker: calls made through the stand-in sudo are logged as "root-systemctl", so a test can tell the
# few administrator-level commands from the user-level ones.
# The fake SteamOS: `cat` of the socket unit succeeds unless FAKE_NO_SOCKET is set, and restarting that socket makes
# port 8081 listen on every interface (the SteamOS default) or, when our drop-in file exists, on this device only.
SYSTEMCTL = '''#!/bin/sh
if [ -n "$FAKE_ROOT" ]; then printf 'root-systemctl %s\\n' "$*" >> "$FAKE_LOG"; else printf 'systemctl %s\\n' "$*" >> "$FAKE_LOG"; fi
case "$1" in
  cat) [ -z "$FAKE_NO_SOCKET" ]; exit $? ;;
  # WHY stop/start and a failing restart: real SteamOS runs a helper service (steam-web-debug-portforward.service)
  # that systemd keeps for 5 minutes after the last connection. While it runs, `restart` of the socket FAILS
  # ("Socket service ... already active, refusing") and the old wide-open listener stays in use (seen in the M8 VM run).
  stop) case "$*" in *steam-web-debug-portforward.service*) rm -f "$SWIPEKB_PROC_NET/proxy-active";; esac ;;
  start|restart) case "$2" in steam-web-debug-portforward.socket)
      if [ "$1" = restart ] && [ -f "$SWIPEKB_PROC_NET/proxy-active" ]; then echo 'Job failed.' >&2; exit 1; fi
      if [ -f "$SWIPEKB_SYSTEM_UNIT_DIR/steam-web-debug-portforward.socket.d/swipekb-loopback-only.conf" ] && [ -z "$FAKE_STUCK_OPEN" ]; then
        a=0100007F; else a=00000000; fi
      printf 'header\\n   0: %s:1F91 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 1 1 0 100 0 0 10 0\\n' "$a" > "$SWIPEKB_PROC_NET/tcp";; esac ;;
esac
exit 0
'''
SUDO = '''#!/bin/sh
printf 'sudo %s\\n' "$*" >> "$FAKE_LOG"
[ "$FAKE_SUDO" = deny ] && { echo 'sudo: a password is required' >&2; exit 1; }
[ "$1" = -v ] && exit 0
FAKE_ROOT=1 exec "$@"
'''


class Fixture:
    def __init__(self, test, version='0.3.1-1', steam=True, symlink=False):
        self.tmp = tempfile.TemporaryDirectory()
        test.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.home = base / 'home'
        self.home.mkdir()
        self.log = base / 'calls.log'
        self.apps = base / 'apps'
        self.log.write_text('')
        self.apps.write_text('')
        shims, tools = base / 'shims', base / 'tools'
        shims.mkdir(), tools.mkdir()
        for name, body in (('flatpak', FLATPAK), ('systemctl', SYSTEMCTL), ('sudo', SUDO)):
            (shims / name).write_text(body)
            (shims / name).chmod(0o755)
        for name in TOOLS:
            found = shutil.which(name)
            if found:
                (tools / name).symlink_to(found)
        self.shims, self.tools = shims, tools
        self.sysdir = base / 'etc-systemd-system'          # stands in for /etc/systemd/system
        self.sysdir.mkdir()
        self.proc = base / 'proc-net'                       # stands in for /proc/net; starts like SteamOS: 8081 open to all
        self.proc.mkdir()
        (self.proc / 'tcp').write_text('header\n   0: 00000000:1F91 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 1 1 0 100 0 0 10 0\n')
        (self.proc / 'tcp6').write_text('header\n')
        self.steam = self.home / '.local/share/Steam'
        if steam:
            self.steam.mkdir(parents=True)
            if symlink:
                (self.home / '.steam').mkdir()
                (self.home / '.steam/steam').symlink_to(self.steam)
        self.bundle = base / 'download'
        self.make_bundle(version)

    def make_bundle(self, version):
        """Lay out what the SteamOS download contains after extraction (same shape scripts/steamos-bundle.py makes)."""
        if self.__dict__.get('bundle') and self.bundle.exists():
            shutil.rmtree(self.bundle)
        self.bundle.mkdir()
        shutil.copy2(SETUP / 'install.sh', self.bundle)
        shutil.copy2(SETUP / 'uninstall.sh', self.bundle)
        shutil.copy2(SETUP / 'switch-watch.sh', self.bundle)
        shutil.copytree(SETUP / 'units', self.bundle / 'units')
        (self.bundle / 'VERSION').write_text(version + '\n')
        (self.bundle / f'swipe-keyboard-{version}-x86_64.flatpak').write_bytes(b'main bundle ' + version.encode())
        (self.bundle / f'swipe-keyboard-gaming-helper-{version}-x86_64.flatpak').write_bytes(b'helper bundle ' + version.encode())
        self.write_sums()

    def write_sums(self):
        lines = []
        for path in sorted(self.bundle.glob('*.flatpak')):
            lines.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n')
        (self.bundle / 'SHA256SUMS').write_text(''.join(lines))

    @property
    def dropin(self):
        return self.sysdir / 'steam-web-debug-portforward.socket.d/swipekb-loopback-only.conf'

    def run(self, script, *args, stdin='y\n', fail=None, shims=True, extra=None):
        env = {'SWIPEKB_SYSTEM_UNIT_DIR': str(self.sysdir), 'SWIPEKB_PROC_NET': str(self.proc), **(extra or {}), 'HOME': str(self.home), 'PATH': (str(self.shims) + ':' if shims else '') + str(self.tools),
               'LANG': 'C', 'FAKE_LOG': str(self.log), 'FAKE_APPS': str(self.apps), 'FAKE_FAIL': fail or ''}
        return subprocess.run(['bash', str(self.bundle / script), *args], input=stdin, env=env,
                              capture_output=True, text=True, timeout=60)

    def calls(self):
        return self.log.read_text().splitlines()

    def snapshot(self):
        """Every file and symlink under HOME with its content, so 'nothing changed' is checkable."""
        result = {}
        for path in sorted(self.home.rglob('*')):
            if path.is_symlink():
                result[str(path.relative_to(self.home))] = ('link', os.readlink(path))
            elif path.is_file():
                result[str(path.relative_to(self.home))] = ('file', path.read_bytes())
        return result

    def record(self):
        return json.loads((self.home / '.local/share/swipe-keyboard-gaming/install-record.json').read_text())

    @property
    def units(self):
        return self.home / '.config/systemd/user'


class InstallTests(unittest.TestCase):
    def test_dry_run_prints_the_plan_and_changes_nothing(self):
        f = Fixture(self)
        before = f.snapshot()
        result = f.run('install.sh', '--dry-run', stdin='')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(f.calls(), [])  # not even a read-only flatpak call that could differ on a real machine
        self.assertIn('Dry run', result.stdout)
        self.assertIn(str(f.steam / MARKER), result.stdout)
        self.assertIn('swipekb-engine.service', result.stdout)

    def test_consent_text_is_plain_and_complete(self):
        f = Fixture(self)
        out = f.run('install.sh', '--dry-run', stdin='').stdout
        for phrase in ('developer switch', 'other devices', '8081', 'has not approved this', 'Subscriber Agreement',
                       'administrator password', 'Second question', 'Block other devices', 'Leave it open', 'Nothing is sent to the internet', 'Gaming Mode', 'uninstall.sh',
                       'restart Steam'):
            self.assertIn(phrase, out)

    def test_answering_no_or_nothing_changes_nothing(self):
        for answer in ('n\n', '\n', '', 'maybe\n', 'N\n'):
            with self.subTest(answer=answer):
                f = Fixture(self)
                before = f.snapshot()
                result = f.run('install.sh', stdin=answer)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Nothing was changed', result.stdout)
                self.assertEqual(f.snapshot(), before)
                self.assertEqual([c for c in f.calls() if not c.startswith('flatpak info')], [])

    def test_install_creates_marker_units_record_and_runs_user_services(self):
        f = Fixture(self)
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((f.steam / MARKER).is_file())
        engine = (f.units / 'swipekb-engine.service').read_text()
        self.assertEqual(engine.splitlines()[1], '# swipekb-gaming-version: 0.3.1-1')
        self.assertNotIn('@VERSION@', engine)
        self.assertEqual(engine.splitlines()[2], '# swipekb-gaming-unit-revision: 3')   # WHY: bumped when unit files change, so an installed copy is visibly old
        self.assertTrue((f.units / 'swipekb-helper.service').is_file())
        record = f.record()
        self.assertEqual(record['version'], '0.3.1-1')
        self.assertEqual(record['created_markers'], [str(f.steam / MARKER)])
        self.assertEqual(record['existing_markers'], [])
        self.assertEqual(sorted(record['created_units']), ['swipekb-engine.service', 'swipekb-helper.service', 'swipekb-switch-watch.service', 'swipekb-switch-watch.timer'])
        self.assertEqual(record['installed_flatpaks'], [MAIN_ID, HELPER_ID])
        calls = f.calls()
        installs = [c for c in calls if c.startswith('flatpak install')]
        self.assertEqual(len(installs), 2)
        self.assertIn('swipe-keyboard-0.3.1-1-x86_64.flatpak', installs[0])
        self.assertIn('swipe-keyboard-gaming-helper-0.3.1-1-x86_64.flatpak', installs[1])
        self.assertIn('systemctl --user daemon-reload', calls)
        self.assertIn('systemctl --user enable --now swipekb-engine.service swipekb-helper.service swipekb-switch-watch.service swipekb-switch-watch.timer', calls)
        self.assertIn('restart Steam', result.stdout)

    def test_a_marker_that_was_removed_and_is_made_again_is_recorded_as_ours_only(self):
        # WHY (found in the SteamOS VM, round 3): a switch file that existed before (Decky) and was later removed was made
        # again by a re-run, and the record then listed it as both "created" and "existed before".
        f = Fixture(self)
        marker = f.steam / MARKER
        marker.write_text('decky was here')
        f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(f.record()['existing_markers'], [str(marker)])
        marker.unlink()
        f.run('install.sh', '--gaming', '--block-outside')
        record = f.record()
        self.assertEqual(record['created_markers'], [str(marker)])
        self.assertEqual(record['existing_markers'], [])

    def test_the_switch_watcher_is_installed_wired_and_removed_again(self):
        # Owner decision 2026-10-22: if the developer switch file disappears we notify with a recovery option and
        # never silently recreate it. A host-side watcher does the looking (the helper's sandbox cannot see Steam's folder).
        f = Fixture(self)
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        watcher = f.home / '.local/share/swipe-keyboard-gaming/switch-watch.sh'
        self.assertTrue(os.access(watcher, os.X_OK))
        self.assertEqual(watcher.read_bytes(), (SETUP / 'switch-watch.sh').read_bytes())
        service = (f.units / 'swipekb-switch-watch.service').read_text()
        self.assertIn(f'ExecStart={watcher} check', service)
        self.assertNotIn('@', service)
        self.assertIn('OnUnitActiveSec=60s', (f.units / 'swipekb-switch-watch.timer').read_text())
        self.assertIn('Once a minute it only checks', result.stdout)   # said before it is done (consent text)
        # nothing in the installer's own steps can create the switch file again on its own after a first install
        (f.steam / MARKER).unlink()
        self.assertEqual(f.run('uninstall.sh', stdin='y\n').returncode, 0)
        self.assertFalse(watcher.exists())
        self.assertEqual(list(f.units.glob('swipekb-*')), [])

    def test_rerun_after_the_switch_was_turned_off_says_so_and_puts_it_back(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        (f.steam / MARKER).unlink()
        state = f.home / '.local/state/swipekb-gaming'
        (state / 'switch-state').write_text('off\n')
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('had been turned off', result.stdout)
        self.assertIn('Restart Steam once', result.stdout)
        self.assertTrue((f.steam / MARKER).is_file())
        self.assertFalse((state / 'switch-state').exists(), 'a stale "off" must not outlive the fix')

    def test_only_user_scope_commands_are_ever_run(self):
        # WHY: no sudo, no system-wide Flatpak, no system service (plan Section 5).
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        f.run('uninstall.sh', '--yes', '--remove-apps')
        for call in f.calls():
            if call.startswith(('sudo ', 'root-systemctl ')) or call == 'systemctl cat steam-web-debug-portforward.socket':
                continue   # the few administrator steps are pinned by the outside-door tests below
            self.assertIn('--user', call, call)
        for script in ('install.sh', 'uninstall.sh'):
            # WHY comments are skipped: the header says "No sudo"; only executable lines and messages matter.
            text = '\n'.join(l for l in (SETUP / script).read_text().splitlines() if not l.lstrip().startswith('#'))
            for forbidden in ('pkexec', 'doas', 'firewall-cmd', 'nft ', 'iptables', 'steamos-readonly', '/usr/lib/systemd/system'):
                self.assertNotIn(forbidden, text, f'{script} contains {forbidden}')
            # WHY a whitelist: administrator rights are used for exactly these actions on our one drop-in file, nothing else.
            for line in text.splitlines():
                if 'sudo' in line:
                    self.assertRegex(line.strip(), r'^(if ! |\{ )?(command -v sudo|sudo -v|sudo (mkdir -p|rm -f|rmdir|systemctl (daemon-reload|stop|start)|tee)\b)|sudo -v;|&& sudo|\| sudo tee|\{ sudo mkdir', f'{script}: {line}')

    def test_existing_marker_is_left_alone_and_recorded_as_not_ours(self):
        f = Fixture(self)
        marker = f.steam / MARKER
        marker.write_text('decky was here')
        os.utime(marker, (1000, 1000))
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(marker.read_text(), 'decky was here')
        self.assertEqual(marker.stat().st_mtime, 1000)
        record = f.record()
        self.assertEqual(record['created_markers'], [])
        self.assertEqual(record['existing_markers'], [str(marker)])

    def test_running_twice_is_harmless_and_keeps_the_created_flag(self):
        # WHY: on the second run the marker exists because WE made it; the record must not flip to "not ours",
        # or uninstall would leave the developer switch on.
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        first = {k: v for k, v in f.snapshot().items() if k != '.local/share/swipe-keyboard-gaming/install-record.json'}
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(result.returncode, 0, result.stderr)
        second = {k: v for k, v in f.snapshot().items() if k != '.local/share/swipe-keyboard-gaming/install-record.json'}
        self.assertEqual(first, second)
        record = f.record()
        self.assertEqual(record['created_markers'], [str(f.steam / MARKER)])
        self.assertEqual(record['existing_markers'], [])
        self.assertEqual(record['installed_flatpaks'], [MAIN_ID, HELPER_ID])

    def test_steam_folder_reached_two_ways_gets_one_marker(self):
        f = Fixture(self, symlink=True)
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(f.record()['created_markers'], [str(f.steam / MARKER)])

    def test_refuses_when_steam_has_never_run(self):
        f = Fixture(self, steam=False)
        before = f.snapshot()
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Steam', result.stderr)
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(f.calls(), [])

    def test_refuses_tampered_download_before_changing_anything(self):
        f = Fixture(self)
        (f.bundle / 'swipe-keyboard-0.3.1-1-x86_64.flatpak').write_bytes(b'tampered')
        before = f.snapshot()
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('checksum', result.stderr.lower())
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(f.calls(), [])

    def test_refuses_without_flatpak_or_systemctl(self):
        f = Fixture(self)
        result = f.run('install.sh', '--gaming', '--block-outside', shims=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('flatpak', result.stderr)
        self.assertEqual(f.snapshot(), {})

    def test_never_overwrites_a_unit_file_it_did_not_write(self):
        f = Fixture(self)
        f.units.mkdir(parents=True)
        (f.units / 'swipekb-helper.service').write_text('[Service]\nExecStart=/bin/true\n')
        before = f.snapshot()
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('swipekb-helper.service', result.stderr)
        self.assertEqual(f.snapshot(), before)  # refused up front: not even the marker was made

    def test_rerun_with_the_same_version_already_installed_is_not_an_error(self):
        # WHY: found in the real SteamOS VM (M8). `flatpak install --bundle` fails with "already installed" when the
        # bundle is the version that is already here, so every re-run (the documented way to change a choice or
        # upgrade) died before doing anything. That one message means "nothing to do", not "failed".
        f = Fixture(self)
        self.assertEqual(f.run('install.sh', '--gaming', '--block-outside').returncode, 0)
        result = f.run('install.sh', '--gaming', '--leave-open', fail='same-version')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(f.record()['installed_flatpaks'], [MAIN_ID, HELPER_ID])

    def test_a_real_install_failure_is_still_an_error_even_if_the_app_is_already_here(self):
        f = Fixture(self)
        f.run('install.sh', '--desktop-only')
        result = f.run('install.sh', '--desktop-only', fail='install')
        self.assertNotEqual(result.returncode, 0)

    def test_flatpak_failure_leaves_no_marker_or_units(self):
        # WHY: the network-dependent step runs first so a failure cannot leave a half-installed system.
        f = Fixture(self)
        result = f.run('install.sh', '--gaming', '--block-outside', fail='install')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((f.steam / MARKER).exists())
        self.assertFalse(f.units.exists())

    def test_skip_flatpaks_installs_no_apps_and_records_none(self):
        f = Fixture(self)
        result = f.run('install.sh', '--gaming', '--block-outside', '--skip-flatpaks')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([c for c in f.calls() if c.startswith('flatpak install')], [])
        self.assertEqual(f.record()['installed_flatpaks'], [])

    def test_upgrade_rewrites_stamped_units_restarts_and_refuses_downgrade(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        f.make_bundle('0.3.2-1')
        f.log.write_text('')
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((f.units / 'swipekb-engine.service').read_text().splitlines()[1], '# swipekb-gaming-version: 0.3.2-1')
        self.assertIn('systemctl --user restart swipekb-engine.service swipekb-helper.service swipekb-switch-watch.service swipekb-switch-watch.timer', f.calls())
        self.assertEqual(f.record()['version'], '0.3.2-1')
        self.assertEqual(f.record()['created_markers'], [str(f.steam / MARKER)])
        f.make_bundle('0.3.1-1')
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('older', result.stderr)
        self.assertEqual(f.record()['version'], '0.3.2-1')
        self.assertEqual(f.run('install.sh', '--gaming', '--block-outside', '--force').returncode, 0)
        self.assertEqual(f.record()['version'], '0.3.1-1')

def sys_files(f):
    return sorted(str(p.relative_to(f.sysdir)) for p in f.sysdir.rglob('*') if p.is_file())


def door_open(f):
    """Is 8081 listening on every interface in the fake /proc/net/tcp (what SteamOS does by default)?"""
    return '00000000:1F91' in (f.proc / 'tcp').read_text()


class ChoiceTests(unittest.TestCase):
    """The two owner choices (2026-10-22): Gaming Mode on or off, and, only with Gaming Mode, block other devices or leave open."""
    def gaming_state_file(self, f):
        return f.home / '.local/state/swipekb-gaming/outside-access'

    def test_a_first_install_with_no_choice_is_desktop_only_and_asks_for_nothing(self):
        # WHY: pressing Enter, or running with --yes, must never switch on a Valve developer feature or ask for a password.
        for args, stdin in (((), '\ny\n'), (('--yes',), '')):
            with self.subTest(args=args):
                f = Fixture(self)
                result = f.run('install.sh', *args, stdin=stdin)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse((f.steam / MARKER).exists())
                self.assertFalse(f.units.exists())
                self.assertEqual(sys_files(f), [])
                self.assertEqual([c for c in f.calls() if c.startswith(('sudo', 'root-', 'systemctl'))], [])
                installs = [c for c in f.calls() if c.startswith('flatpak install')]
                self.assertEqual(len(installs), 1)
                self.assertIn('swipe-keyboard-0.3.1-1', installs[0])
                self.assertEqual(f.record()['installed_flatpaks'], [MAIN_ID])
                self.assertTrue(door_open(f))   # we changed nothing about the network

    def test_desktop_only_works_even_when_steam_has_never_run(self):
        f = Fixture(self, steam=False)
        result = f.run('install.sh', '--desktop-only')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Gaming Mode is not available', result.stdout)

    def test_gaming_with_block_closes_the_door_before_the_switch_and_records_it(self):
        f = Fixture(self)
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((f.dropin).read_text(), '\n'.join([
            '# swipekb-gaming-managed',
            '# Written by the Swipe Keyboard installer. Makes SteamOS answer Steam debug requests from this Deck only,',
            '# not from other devices on the network. Remove with: bash uninstall.sh (or delete this file, then',
            '# run: systemctl daemon-reload && systemctl stop steam-web-debug-portforward.socket steam-web-debug-portforward.service',
            '# && systemctl start steam-web-debug-portforward.socket).',
            '[Socket]', 'ListenStream=', 'ListenStream=127.0.0.1:8081', '']))
        self.assertFalse(door_open(f))
        record = f.record()
        self.assertEqual(record['created_system_files'], [str(f.dropin)])
        self.assertEqual(record['outside_access'], 'blocked')
        self.assertEqual(self.gaming_state_file(f).read_text().strip(), 'blocked')
        calls = f.calls()
        restart = calls.index('root-systemctl start steam-web-debug-portforward.socket')
        self.assertIn('root-systemctl daemon-reload', calls[:restart])
        # the door is shut before the helper app, the switch file or the services exist
        self.assertLess(restart, next(i for i, c in enumerate(calls) if c.startswith('flatpak install') and 'GamingHelper' not in c and 'gaming-helper' in c))
        self.assertTrue((f.steam / MARKER).is_file())

    def test_the_still_running_forwarding_helper_cannot_keep_the_door_open_or_stop_it_reopening(self):
        # WHY: found in the SteamOS VM (M8). SteamOS keeps a small forwarding service alive for 5 minutes after any
        # connection to port 8081. While it runs, `systemctl restart` of the socket FAILS and the old listener stays,
        # so closing the door looked refused and reopening it left no listener at all. The installer must stop the
        # service and start the socket fresh, in both directions.
        f = Fixture(self)
        (f.proc / 'proxy-active').write_text('1')
        result = f.run('install.sh', '--gaming', '--block-outside')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(door_open(f))
        (f.proc / 'proxy-active').write_text('1')
        result = f.run('install.sh', '--gaming', '--leave-open')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(door_open(f))
        self.assertFalse(f.dropin.exists())
        (f.proc / 'proxy-active').write_text('1')
        f.run('install.sh', '--gaming', '--block-outside')
        (f.proc / 'proxy-active').write_text('1')
        result = f.run('uninstall.sh')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(door_open(f), 'uninstall must put SteamOS back to how it was, listener included')

    def test_gaming_with_leave_open_needs_no_password_and_changes_no_system_file(self):
        f = Fixture(self)
        result = f.run('install.sh', '--gaming', '--leave-open')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([c for c in f.calls() if c.startswith(('sudo', 'root-'))], [])
        self.assertEqual(sys_files(f), [])
        self.assertTrue(door_open(f))
        self.assertEqual(f.record()['outside_access'], 'open')
        self.assertEqual(f.record()['created_system_files'], [])
        self.assertEqual(self.gaming_state_file(f).read_text().strip(), 'open')
        self.assertTrue((f.steam / MARKER).is_file())
        self.assertIn('Other devices on your', result.stdout)   # the plan said so plainly

    def test_no_password_with_block_does_not_silently_enable_gaming_mode(self):
        f = Fixture(self)
        result = f.run('install.sh', '--gaming', '--block-outside', '--yes', extra={'FAKE_SUDO': 'deny'})
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn('NOT closed', result.stderr)
        self.assertIn('passwd', result.stderr)
        self.assertFalse((f.steam / MARKER).exists())
        self.assertFalse(f.units.exists())
        self.assertEqual(sys_files(f), [])
        self.assertEqual([c for c in f.calls() if c.startswith('flatpak install')], [c for c in f.calls() if c.startswith('flatpak install') and 'gaming-helper' not in c])
        self.assertEqual(f.record()['installed_flatpaks'], [MAIN_ID])   # the Desktop keyboard still installed
        self.assertEqual(f.record()['gaming'], 'no')

    def test_declining_the_password_asks_again_and_skip_is_the_default(self):
        for stdin, expect_gaming in (('y\n\n', False), ('y\n2\n', False), ('y\n1\n', True)):
            with self.subTest(stdin=stdin):
                f = Fixture(self)
                result = f.run('install.sh', '--gaming', '--block-outside', stdin=stdin, extra={'FAKE_SUDO': 'deny'})
                self.assertIn('What now?', result.stdout)
                self.assertEqual((f.steam / MARKER).exists(), expect_gaming, result.stdout + result.stderr)
                self.assertEqual(result.returncode, 0 if expect_gaming else 3)
                if expect_gaming:
                    self.assertEqual(f.record()['outside_access'], 'open')
                    self.assertEqual(self.gaming_state_file(f).read_text().strip(), 'open')
                    self.assertEqual(sys_files(f), [])

    def test_a_door_that_stays_open_undoes_the_change_and_leaves_gaming_off(self):
        f = Fixture(self)
        result = f.run('install.sh', '--gaming', '--block-outside', '--yes', extra={'FAKE_STUCK_OPEN': '1'})
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn('still reachable', result.stderr)
        self.assertEqual(sys_files(f), [])
        self.assertFalse((f.steam / MARKER).exists())
        self.assertEqual(f.record()['created_system_files'], [])

    def test_a_drop_in_that_is_not_ours_is_never_touched(self):
        f = Fixture(self)
        f.dropin.parent.mkdir(parents=True)
        f.dropin.write_text('[Socket]\nListenStream=\n')
        result = f.run('install.sh', '--gaming', '--block-outside', '--yes')
        self.assertEqual(result.returncode, 3)
        self.assertEqual(f.dropin.read_text(), '[Socket]\nListenStream=\n')
        self.assertFalse((f.steam / MARKER).exists())

    def test_running_again_with_the_door_already_closed_asks_for_no_password(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        f.log.write_text('')
        result = f.run('install.sh', '--gaming', '--block-outside', extra={'FAKE_SUDO': 'deny'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([c for c in f.calls() if c.startswith(('sudo', 'root-'))], [])
        self.assertEqual(f.record()['created_system_files'], [str(f.dropin)])

    def test_a_system_without_the_socket_and_with_nothing_exposed_needs_no_password(self):
        f = Fixture(self)
        (f.proc / 'tcp').write_text('header\n')
        result = f.run('install.sh', '--gaming', '--block-outside', extra={'FAKE_NO_SOCKET': '1', 'FAKE_SUDO': 'deny'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sys_files(f), [])
        self.assertTrue((f.steam / MARKER).is_file())

    def test_interactive_answers_map_to_the_right_choices(self):
        cases = (('2\n1\ny\n', True, 'blocked'), ('2\n2\ny\n', True, 'open'), ('2\n\ny\n', True, 'blocked'),
                 ('1\ny\n', False, None), ('\ny\n', False, None))
        for stdin, gaming, outside in cases:
            with self.subTest(stdin=stdin):
                f = Fixture(self)
                result = f.run('install.sh', stdin=stdin)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((f.steam / MARKER).exists(), gaming)
                if gaming:
                    self.assertEqual(f.record()['outside_access'], outside)
                    self.assertEqual(door_open(f), outside == 'open')

    def test_the_second_question_only_appears_for_gaming_mode(self):
        f = Fixture(self)
        self.assertNotIn('Second question', f.run('install.sh', '--desktop-only', '--yes').stdout)
        self.assertIn('Second question', Fixture(self).run('install.sh', '--gaming', '--leave-open', '--yes').stdout)

    def test_a_rerun_can_turn_gaming_mode_off_and_keeps_the_desktop_keyboard(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        f.log.write_text('')
        result = f.run('install.sh', '--desktop-only')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((f.steam / MARKER).exists())           # we made it, so it goes
        self.assertFalse(f.units.exists() and any(f.units.iterdir()))
        self.assertEqual(sys_files(f), [])
        self.assertTrue(door_open(f))                          # the block we added is gone again
        removed = [c for c in f.calls() if c.startswith('flatpak uninstall')]
        self.assertEqual(len(removed), 1)
        self.assertTrue(removed[0].endswith(HELPER_ID))        # only the Gaming helper, never the Desktop keyboard
        record = f.record()
        self.assertEqual(record['gaming'], 'no')
        self.assertEqual(record['installed_flatpaks'], [MAIN_ID])
        self.assertEqual(record['created_system_files'], [])
        self.assertFalse(self.gaming_state_file(f).exists())

    def test_turning_gaming_off_leaves_a_marker_that_existed_before(self):
        f = Fixture(self)
        (f.steam / MARKER).write_text('decky')
        f.run('install.sh', '--gaming', '--leave-open')
        f.run('install.sh', '--desktop-only')
        self.assertEqual((f.steam / MARKER).read_text(), 'decky')

    def test_turning_gaming_off_without_the_password_keeps_the_harmless_block(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        result = f.run('install.sh', '--desktop-only', extra={'FAKE_SUDO': 'deny'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(f.dropin.is_file())
        self.assertEqual(f.record()['gaming'], 'no')
        self.assertFalse((f.steam / MARKER).exists())

    def test_the_two_choices_change_independently_on_a_rerun(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        self.assertTrue(f.dropin.is_file())
        result = f.run('install.sh', '--gaming', '--leave-open')       # block -> open: our block comes away
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sys_files(f), [])
        self.assertTrue(door_open(f))
        self.assertEqual(f.record()['outside_access'], 'open')
        self.assertEqual(self.gaming_state_file(f).read_text().strip(), 'open')
        self.assertTrue((f.steam / MARKER).is_file())                  # Gaming Mode stayed on
        result = f.run('install.sh', '--gaming', '--block-outside')    # open -> block
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(f.dropin.is_file())
        self.assertEqual(self.gaming_state_file(f).read_text().strip(), 'blocked')

    def test_yes_alone_keeps_what_you_already_have(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--leave-open')
        result = f.run('install.sh', '--yes')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(f.record()['gaming'], 'yes')
        self.assertEqual(f.record()['outside_access'], 'open')
        self.assertTrue((f.steam / MARKER).is_file())

    def test_dry_run_with_no_choice_shows_both_plans_and_changes_nothing(self):
        f = Fixture(self)
        before = f.snapshot()
        out = f.run('install.sh', '--dry-run', stdin='').stdout
        for phrase in ('If you choose 2', 'administrator password', str(f.dropin), str(f.steam / MARKER), 'Second question'):
            self.assertIn(phrase, out)
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(f.calls(), [])
        self.assertEqual(sys_files(f), [])
        out = Fixture(self).run('install.sh', '--dry-run', '--gaming', '--leave-open', stdin='').stdout
        self.assertIn('leave SteamOS', out)
        self.assertNotIn('write one small file', out)


class UninstallBlockTests(unittest.TestCase):
    def test_uninstall_removes_the_block_it_made_and_restores_everything(self):
        f = Fixture(self)
        before = f.snapshot()
        f.run('install.sh', '--gaming', '--block-outside')
        result = f.run('uninstall.sh', '--yes')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(sys_files(f), [])
        self.assertFalse(f.dropin.parent.exists())      # the folder we made goes too, only because it is empty
        self.assertTrue(door_open(f))

    def test_uninstall_dry_run_names_the_block_and_changes_nothing(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        f.log.write_text('')
        out = f.run('uninstall.sh', '--dry-run', stdin='').stdout
        self.assertIn(str(f.dropin), out)
        self.assertTrue(f.dropin.is_file())
        self.assertEqual(f.calls(), [])

    def test_uninstall_without_the_password_leaves_the_block_and_still_finishes(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        result = f.run('uninstall.sh', '--yes', extra={'FAKE_SUDO': 'deny'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(f.dropin.is_file())
        self.assertIn('left in place', result.stderr)

    def test_uninstall_never_removes_a_drop_in_that_is_not_recorded_as_ours(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--leave-open')     # no block made, nothing recorded
        f.dropin.parent.mkdir(parents=True)
        f.dropin.write_text('# swipekb-gaming-managed\n[Socket]\n')    # looks like ours, but the record does not list it
        f.run('uninstall.sh', '--yes')
        self.assertTrue(f.dropin.is_file())

class RealDownloadTests(unittest.TestCase):
    def test_the_assembled_download_installs_with_its_own_checksums(self):
        # WHY: ties the packaging side (scripts/steamos_bundle.py) to the installer's checksum, file-name and
        # layout expectations, so neither can drift without a failing test.
        spec = importlib.util.spec_from_file_location('steamos_bundle', ROOT / 'scripts/steamos_bundle.py')
        bundle = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bundle)
        f = Fixture(self)
        (f.bundle).rename(f.bundle.with_name('unused'))
        parts = f.bundle.with_name('parts')
        parts.mkdir()
        (parts / 'm').write_bytes(b'main'), (parts / 'h').write_bytes(b'helper')
        archive = bundle.assemble(parts / 'm', parts / 'h', '0.3.1-1', parts)
        with tarfile.open(archive) as tar:
            tar.extractall(parts / 'x', filter='data')
        f.bundle = parts / 'x' / 'swipe-keyboard-steamos-0.3.1-1'
        result = f.run('install.sh', '--gaming', '--block-outside', '--yes')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len([c for c in f.calls() if c.startswith('flatpak install')]), 2)
        (f.bundle / 'units/swipekb-helper.service').write_text('changed')
        self.assertNotEqual(f.run('install.sh', '--gaming', '--block-outside', '--yes').returncode, 0)


class UninstallTests(unittest.TestCase):
    def installed(self, **kwargs):
        f = Fixture(self, **kwargs)
        self.assertEqual(f.run('install.sh', '--gaming', '--block-outside').returncode, 0)
        f.log.write_text('')
        return f

    def test_install_then_uninstall_restores_the_home_folder_exactly(self):
        f = Fixture(self)
        before = f.snapshot()
        f.run('install.sh', '--gaming', '--block-outside')
        (f.home / '.local/state/swipekb-gaming/status.json').write_text('{}')  # what the helper writes
        result = f.run('uninstall.sh', '--yes')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(f.snapshot(), before)
        calls = f.calls()
        self.assertIn('systemctl --user disable --now swipekb-engine.service swipekb-helper.service swipekb-switch-watch.service swipekb-switch-watch.timer', calls)
        self.assertIn('systemctl --user daemon-reload', calls)
        self.assertEqual([c for c in calls if c.startswith('flatpak uninstall')], [])
        self.assertIn('flatpak uninstall --user', result.stdout)  # printed as a hint, not run
        self.assertIn('next restart', result.stdout)

    def test_pre_existing_marker_survives_uninstall(self):
        f = Fixture(self)
        (f.steam / MARKER).write_text('mine')
        f.run('install.sh', '--gaming', '--block-outside')
        result = f.run('uninstall.sh', '--yes')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((f.steam / MARKER).read_text(), 'mine')
        self.assertFalse((f.units / 'swipekb-engine.service').exists())

    def test_foreign_unit_with_same_name_is_not_deleted(self):
        f = self.installed()
        (f.units / 'swipekb-helper.service').write_text('[Service]\nExecStart=/bin/true\n')
        f.run('uninstall.sh', '--yes')
        self.assertTrue((f.units / 'swipekb-helper.service').is_file())
        self.assertFalse((f.units / 'swipekb-engine.service').exists())

    def test_dry_run_and_no_change_nothing(self):
        f = self.installed()
        before = f.snapshot()
        self.assertEqual(f.run('uninstall.sh', '--dry-run', stdin='').returncode, 0)
        self.assertEqual(f.snapshot(), before)
        result = f.run('uninstall.sh', stdin='n\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(f.calls(), [])

    def test_remove_apps_only_removes_apps_this_install_added(self):
        f = Fixture(self)
        f.apps.write_text(MAIN_ID + '\n')  # the keyboard was already there: not ours to remove
        f.run('install.sh', '--gaming', '--block-outside')
        f.log.write_text('')
        f.run('uninstall.sh', '--yes', '--remove-apps')
        removed = [c for c in f.calls() if c.startswith('flatpak uninstall')]
        self.assertEqual(len(removed), 1)
        self.assertTrue(removed[0].endswith(HELPER_ID))

    def test_without_a_record_it_cleans_its_own_units_but_never_the_marker(self):
        f = self.installed()
        shutil.rmtree(f.home / '.local/share/swipe-keyboard-gaming')
        result = f.run('uninstall.sh', '--yes')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((f.steam / MARKER).exists())
        self.assertFalse((f.units / 'swipekb-engine.service').exists())
        self.assertIn('install record', result.stdout)

    def test_a_recorded_file_that_someone_replaced_is_not_removed(self):
        # WHY: the stamp on its first line is what proves the file is still the one this installer wrote.
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        f.dropin.write_text('[Socket]\nListenStream=\nListenStream=192.168.1.5:8081\n')    # edited by hand, stamp gone
        f.run('uninstall.sh', '--yes')
        self.assertTrue(f.dropin.is_file())
        f2 = Fixture(self)
        f2.run('install.sh', '--gaming', '--block-outside')
        f2.dropin.write_text('[Socket]\nListenStream=\n')
        f2.run('install.sh', '--gaming', '--leave-open')
        self.assertTrue(f2.dropin.is_file())


class ReviewFixTests(unittest.TestCase):
    """Fixes from the independent review of the Gaming Mode branch (plan: Pre-merge review fixes)."""

    def gaming(self):
        f = Fixture(self)
        self.assertEqual(f.run('install.sh', '--gaming', '--block-outside').returncode, 0)
        f.log.write_text('')
        return f

    def decky(self, f, how='folder'):
        if how == 'folder':
            (f.home / 'homebrew/services').mkdir(parents=True)
            (f.home / 'homebrew/services/PluginLoader').write_text('')
        else:
            (f.sysdir / 'plugin_loader.service').write_text('[Service]\n')

    def test_uninstall_keeps_the_switch_file_when_decky_loader_was_added_after_us(self):
        for how in ('folder', 'unit'):
            f = self.gaming()
            self.decky(f, how)
            result = f.run('uninstall.sh', '--yes')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((f.steam / MARKER).is_file(), how)
            self.assertIn('Decky Loader', result.stdout)

    def test_uninstall_says_beforehand_that_it_will_keep_the_file_for_decky(self):
        f = self.gaming()
        self.decky(f)
        result = f.run('uninstall.sh', '--dry-run', stdin='')
        self.assertIn('Decky Loader', result.stdout)
        self.assertNotIn('delete the developer-switch file', result.stdout)

    def test_turning_gaming_off_keeps_the_switch_file_for_decky_and_forgets_it_as_ours(self):
        f = self.gaming()
        self.decky(f)
        result = f.run('install.sh', '--desktop-only')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((f.steam / MARKER).is_file())
        self.assertIn('Decky Loader', result.stdout)
        self.assertEqual(f.record()['created_markers'], [])
        self.assertEqual(f.record()['existing_markers'], [str(f.steam / MARKER)])

    def test_without_decky_the_switch_file_we_made_still_goes(self):
        f = self.gaming()
        f.run('uninstall.sh', '--yes')
        self.assertFalse((f.steam / MARKER).exists())

    def test_a_recorded_path_that_is_not_the_switch_file_in_steams_folder_is_never_deleted(self):
        for victim in ('Documents/precious.txt', 'Documents/' + MARKER, '.local/share/Steam/config.vdf'):
            f = self.gaming()
            target = f.home / victim
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('mine')
            rec = f.home / '.local/share/swipe-keyboard-gaming/install-record.json'
            rec.write_text(rec.read_text().replace(str(f.steam / MARKER), str(target)))
            result = f.run('uninstall.sh', '--yes')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(target.read_text(), 'mine', victim)
            self.assertTrue((f.steam / MARKER).is_file(), 'the real file is not ours any more on paper, so it stays too')
            f2 = self.gaming()
            rec = f2.home / '.local/share/swipe-keyboard-gaming/install-record.json'
            t2 = f2.home / victim
            t2.parent.mkdir(parents=True, exist_ok=True)
            t2.write_text('mine')
            rec.write_text(rec.read_text().replace(str(f2.steam / MARKER), str(t2)))
            f2.run('install.sh', '--desktop-only')
            self.assertEqual(t2.read_text(), 'mine', 'turning Gaming Mode off applies the same rule: ' + victim)

    def test_uninstall_does_not_disable_a_unit_that_is_not_ours(self):
        f = self.gaming()
        (f.units / 'swipekb-helper.service').write_text('[Service]\nExecStart=/bin/true\n')
        f.run('uninstall.sh', '--yes')
        disables = [c for c in f.calls() if c.startswith('systemctl --user disable')]
        self.assertEqual(len(disables), 1, disables)
        self.assertNotIn('swipekb-helper.service', disables[0])
        self.assertIn('swipekb-engine.service', disables[0])

    def test_running_the_installer_again_clears_the_helpers_pause_and_restarts_it(self):
        f = self.gaming()
        state = f.home / '.local/state/swipekb-gaming'
        state.mkdir(parents=True, exist_ok=True)
        (state / 'retry.json').write_text('{"fingerprint": "x", "failures": 3, "detail": "d"}')
        f.log.write_text('')
        self.assertEqual(f.run('install.sh', '--gaming', '--block-outside').returncode, 0)
        self.assertFalse((state / 'retry.json').exists())
        self.assertTrue([c for c in f.calls() if c.startswith('systemctl --user restart')], f.calls())

    def test_a_dry_run_leaves_the_pause_alone(self):
        f = self.gaming()
        state = f.home / '.local/state/swipekb-gaming'
        state.mkdir(parents=True, exist_ok=True)
        (state / 'retry.json').write_text('{}')
        f.run('install.sh', '--gaming', '--dry-run', stdin='')
        self.assertTrue((state / 'retry.json').exists())


if __name__ == '__main__':
    unittest.main()
