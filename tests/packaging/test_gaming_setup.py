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
  info) for id; do :; done
        grep -qx "$id" "$FAKE_APPS" 2>/dev/null || exit 1
        # WHY: install.sh reads the app's origin remote to point it at the update site.
        case "$*" in *--show-origin*) echo "$(echo "$id" | tr -d '.' | tr 'A-Z' 'a-z')-origin";; esac
        # WHY a commit per app: auto-update.sh compares commits before and after to know whether anything changed.
        case "$*" in *--show-commit*) cat "$FAKE_APPS.$id" 2>/dev/null || echo old;; esac
        exit 0 ;;
  # WHY: real Flatpak says "not found" for a remote that does not exist (seen on SteamOS 3.8 for the helper's origin).
  remote-modify) [ "$LC_ALL" = C ] || { echo 'fake: remote-modify must run with LC_ALL=C' >&2; exit 9; }
       case "$*" in *gaminghelper-origin*) [ -n "$FAKE_LOST_ORIGIN" ] && { echo 'error: Remote "gaminghelper-origin" not found in the user installation' >&2; exit 1; };; esac ;;
  update) [ "$FAKE_FAIL" = update ] && exit 1
          if [ -n "$FAKE_NEW" ]; then for id; do case "$id" in org.*) echo new > "$FAKE_APPS.$id";; esac; done; fi ;;
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
  # WHY FAKE_SOCKET_OFF: Bazzite ships the same socket as SteamOS but switched off (seen in the Bazzite VM, card #502).
  is-enabled) if [ -n "$FAKE_SOCKET_OFF" ]; then echo disabled; exit 1; fi; echo enabled ;;
  is-active) [ -z "$FAKE_SOCKET_OFF" ]; exit $? ;;
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
# WHY a logging stand-in: the uninstaller hands the keyboard choice back to KDE through kwriteconfig6 (card #502).
KWRITECONFIG = '''#!/bin/sh
printf 'kwriteconfig6 %s\\n' "$*" >> "$FAKE_LOG"
'''
# WHY: on ordinary desktops the installer checks the KDE Plasma version (card #502, edge case 2)
PLASMASHELL = '''#!/bin/sh
# WHY "crash": without a screen the real one aborts (seen over SSH in the Debian 13 VM)
[ "$FAKE_PLASMA" = crash ] && exit 134
echo "plasmashell ${FAKE_PLASMA:-6.6.3}"
'''
DPKG_QUERY = '''#!/bin/sh
[ -n "$FAKE_DPKG_PLASMA" ] || exit 1
printf '%s' "$FAKE_DPKG_PLASMA"
'''
SUDO = '''#!/bin/sh
printf 'sudo %s\\n' "$*" >> "$FAKE_LOG"
[ "$FAKE_SUDO" = deny ] && { echo 'sudo: a password is required' >&2; exit 1; }
[ "$1" = -v ] && exit 0
FAKE_ROOT=1 exec "$@"
'''


UPDATE_UNITS = ['swipekb-update.service', 'swipekb-update.timer']
# WHY (card #502): the removal safety check is on for every install, like the update timer; it is not Gaming Mode either.
CHECK_UNITS = ['swipekb-keyboard-check.path', 'swipekb-keyboard-check.service']
SOCKET_QUERIES = {'systemctl cat steam-web-debug-portforward.socket', 'systemctl is-enabled steam-web-debug-portforward.socket',
                  'systemctl is-active -q steam-web-debug-portforward.socket'}


def gaming_units(f):
    """The Gaming Mode unit files present (the automatic-update timer is not Gaming Mode: it is on for the Desktop keyboard too)."""
    return sorted(p.name for p in f.units.iterdir() if p.name not in UPDATE_UNITS + CHECK_UNITS) if f.units.exists() else []


class Fixture:
    def __init__(self, test, version='0.3.1-1', steam=True, symlink=False, system='steamos'):
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
        for name, body in (('flatpak', FLATPAK), ('systemctl', SYSTEMCTL), ('sudo', SUDO), ('kwriteconfig6', KWRITECONFIG),
                           ('plasmashell', PLASMASHELL)):
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
        # WHY a stand-in /etc/os-release: the installer's wording names the system it runs on (SteamOS or e.g. Bazzite)
        self.os_release = base / 'os-release'
        self.os_release.write_text({'steamos': 'NAME="SteamOS"\nID=steamos\n',
                                    'bazzite': 'NAME="Bazzite"\nPRETTY_NAME="Bazzite"\nID=bazzite\nID_LIKE="fedora"\n',
                                    'kubuntu': 'NAME="Ubuntu"\nPRETTY_NAME="Ubuntu 26.04 LTS"\nID=ubuntu\nID_LIKE=debian\n',
                                    'leap': 'NAME="openSUSE Leap"\nID="opensuse-leap"\nID_LIKE="suse opensuse"\n'}[system])
        # WHY stand-ins for the login-session folder and the regular package's folder: this very laptop has the regular
        # package installed, and a real machine's sessions must never decide what a test sees.
        self.sessions = base / 'wayland-sessions'
        self.sessions.mkdir()
        self.native = base / 'usr-share-swipe-keyboard'
        self.appdir = base / 'usr-share-applications'     # stands in for /usr/share/applications
        self.appdir.mkdir()
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
        shutil.copy2(SETUP / 'auto-update.sh', self.bundle)
        shutil.copy2(SETUP / 'keyboard-check.sh', self.bundle)
        shutil.copy2(ROOT / 'packaging/updates/update-key.gpg', self.bundle)
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
        env = {'SWIPEKB_SYSTEM_UNIT_DIR': str(self.sysdir), 'SWIPEKB_PROC_NET': str(self.proc),
               'SWIPEKB_OS_RELEASE': str(self.os_release), 'SWIPEKB_APPLICATIONS_DIR': str(self.appdir),
               'SWIPEKB_SESSIONS_DIR': str(self.sessions), 'SWIPEKB_NATIVE_DIR': str(self.native), 'SWIPEKB_FLATPAK_SYSTEM_DIR': str(self.home.parent / 'var-lib-flatpak'), **(extra or {}), 'HOME': str(self.home), 'PATH': (str(self.shims) + ':' if shims else '') + str(self.tools),
               'LANG': 'C', 'FAKE_LOG': str(self.log), 'FAKE_APPS': str(self.apps), 'FAKE_FAIL': fail or ''}
        env.update(extra or {})
        return subprocess.run(['bash', str(self.bundle / script), *args], input=stdin, env=env,
                              capture_output=True, text=True, timeout=60)

    def calls(self):
        """Every command run, except the read-only questions about SteamOS's socket (does it exist, is it on).

        WHY left out: they change nothing and are asked before the plan is shown (also in a dry run), so "nothing was
        run" checks would otherwise fail on them. What the installer does with the answers is tested on its own."""
        return [c for c in self.log.read_text().splitlines() if c not in SOCKET_QUERIES]

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
                       'administrator password', 'passwd', 'if you say yes below', 'The keyboard itself never uses the internet', 'automatic updates', 'Gaming Mode', 'uninstall.sh',
                       'restart Steam'):
            self.assertIn(phrase, out)

    def test_an_unclear_answer_or_no_answer_at_all_changes_nothing(self):
        # WHY: Enter means yes (card #201), but closed input is nobody pressing Enter, and an unclear answer is not a yes
        for answer in ('', 'maybe\n'):
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
        self.assertEqual(sorted(record['created_units']), sorted(['swipekb-engine.service', 'swipekb-helper.service', 'swipekb-switch-watch.service', 'swipekb-switch-watch.timer'] + UPDATE_UNITS + CHECK_UNITS))
        self.assertEqual(record['auto_update'], 'yes')
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
            if call.startswith(('sudo ', 'root-systemctl ')):
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
                    # WHY the one extra shape (card #502): advice TEXT telling a Kubuntu/Debian/openSUSE/Fedora user how to
                    # install Flatpak themselves; the check below proves the installer never runs it.
                    self.assertRegex(line.strip(), r'^(if ! |\{ )?(command -v sudo|sudo -v|sudo (mkdir -p|rm -f|rmdir|systemctl (daemon-reload|stop|start)|tee)\b)|sudo -v;|&& sudo|\| sudo tee|\{ sudo mkdir'
                                     r'|^\*.*\) how="sudo (apt|zypper|dnf) install flatpak" ;;$', f'{script}: {line}')
            uses = [l.strip() for l in text.splitlines() if '$how' in l]
            self.assertTrue(all(u.startswith('die "') for u in uses), uses)

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

    def test_a_scripted_first_install_is_desktop_only_and_asks_for_nothing(self):
        # WHY: --yes (no person at the keyboard) must never switch on a Valve developer feature or ask for a password.
        for args, stdin in ((('--yes',), ''),):
            with self.subTest(args=args):
                f = Fixture(self)
                result = f.run('install.sh', *args, stdin=stdin)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse((f.steam / MARKER).exists())
                self.assertEqual(gaming_units(f), [])
                self.assertEqual(sys_files(f), [])
                self.assertEqual([c for c in f.calls() if c.startswith(('sudo', 'root-'))], [])
                # WHY only these: automatic updates and the removal safety check (card #502) are on for the Desktop keyboard
                # too, and need nothing else.
                self.assertEqual([c for c in f.calls() if c.startswith('systemctl')],
                                 ['systemctl --user daemon-reload', 'systemctl --user enable --now swipekb-update.timer',
                                  'systemctl --user daemon-reload', 'systemctl --user enable --now swipekb-keyboard-check.path',
                                  'systemctl --user enable swipekb-keyboard-check.service'])
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
        self.assertEqual(gaming_units(f), [])
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

    def test_a_system_that_keeps_the_socket_switched_off_needs_no_password_and_no_system_change(self):
        # WHY (Bazzite VM, card #502): Bazzite ships SteamOS's forwarding socket but disabled, so port 8081 is closed
        # already. Writing the block would ask for a password and START a socket nobody had running; the helper's own
        # check (exposure.py) turns Gaming Mode swipe off if anything opens the port later.
        f = Fixture(self, system='bazzite')
        (f.proc / 'tcp').write_text('header\n')
        result = f.run('install.sh', '--gaming', '--block-outside', extra={'FAKE_SOCKET_OFF': '1', 'FAKE_SUDO': 'deny'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sys_files(f), [])
        self.assertEqual([c for c in f.calls() if c.startswith(('sudo', 'root-'))], [])
        self.assertTrue((f.steam / MARKER).is_file())
        self.assertEqual(f.record()['outside_access'], 'blocked')
        self.assertIn('already closed', result.stdout)

    def test_a_switched_off_socket_with_the_port_open_anyway_does_not_turn_gaming_on(self):
        # Something else listening on 8081 for every device: the final look must still catch it.
        f = Fixture(self, system='bazzite')
        result = f.run('install.sh', '--gaming', '--block-outside', stdin='y\n\n', extra={'FAKE_SOCKET_OFF': '1', 'FAKE_SUDO': 'deny'})
        self.assertIn('NOT closed', result.stdout + result.stderr)
        self.assertFalse((f.steam / MARKER).exists())
        self.assertEqual(sys_files(f), [])

    def test_wording_names_the_system_it_runs_on(self):
        deck = Fixture(self).run('install.sh', '--dry-run', stdin='').stdout
        self.assertIn('Swipe Keyboard for Steam Deck', deck)
        self.assertIn("your Deck's administrator password", deck)
        f = Fixture(self, system='bazzite')
        (f.proc / 'tcp').write_text('header\n')
        out = f.run('install.sh', '--dry-run', stdin='', extra={'FAKE_SOCKET_OFF': '1'}).stdout
        self.assertIn('Swipe Keyboard for Bazzite', out)
        self.assertNotIn('Deck', out)
        self.assertNotIn('SteamOS', out)
        self.assertNotIn('passwd', out)   # nothing asks for a password here, so no hint about setting one
        self.assertIn('already closed', out)

    def test_interactive_answers_map_to_the_right_choices(self):
        # WHY Enter = yes with other devices blocked (Destin, card #201, 2026-10-24)
        cases = (('\n', True, 'blocked'), ('y\n', True, 'blocked'), ('Yes\n', True, 'blocked'),
                 ('n\n', False, None), ('N\n', False, None))
        for stdin, gaming, outside in cases:
            with self.subTest(stdin=stdin):
                f = Fixture(self)
                result = f.run('install.sh', stdin=stdin)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((f.steam / MARKER).exists(), gaming)
                if gaming:
                    self.assertEqual(f.record()['outside_access'], outside)
                    self.assertEqual(door_open(f), outside == 'open')

    def test_a_person_is_asked_exactly_one_question(self):
        out = Fixture(self).run('install.sh', stdin='\n').stdout
        self.assertEqual(out.count('Also swipe in Gaming Mode? [Y/n]'), 1, out)
        self.assertNotIn('Continue?', out)
        self.assertIn('If you answer yes to Gaming Mode', out)   # the plan says what yes adds before asking

    def test_someone_who_said_no_keeps_no_as_the_default_on_a_rerun(self):
        # WHY: running a newer download to upgrade must not turn on a Valve developer feature they turned down
        f = Fixture(self)
        f.run('install.sh', stdin='n\n')
        result = f.run('install.sh', stdin='\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Also swipe in Gaming Mode? [y/N]', result.stdout)
        self.assertFalse((f.steam / MARKER).exists())
        self.assertEqual(f.record()['gaming'], 'no')
        result = f.run('install.sh', stdin='y\n')
        self.assertTrue((f.steam / MARKER).exists(), result.stdout + result.stderr)

    def test_turning_gaming_off_by_answering_no_on_a_rerun(self):
        f = Fixture(self)
        f.run('install.sh', stdin='\n')
        result = f.run('install.sh', stdin='n\n')
        self.assertIn('If you answer no, Gaming Mode is turned off instead', result.stdout)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((f.steam / MARKER).exists())
        self.assertEqual(f.record()['gaming'], 'no')

    def test_a_rerun_can_turn_gaming_mode_off_and_keeps_the_desktop_keyboard(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--block-outside')
        f.log.write_text('')
        result = f.run('install.sh', '--desktop-only')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((f.steam / MARKER).exists())           # we made it, so it goes
        self.assertEqual(gaming_units(f), [])
        self.assertTrue((f.units / 'swipekb-update.timer').is_file())   # turning Gaming Mode off keeps automatic updates
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
        # WHY no 'if you answer yes' header: a dry run with no choice shows the default, which is now Gaming Mode on
        for phrase in ('administrator password', str(f.dropin), str(f.steam / MARKER)):
            self.assertIn(phrase, out)
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(f.calls(), [])
        self.assertEqual(sys_files(f), [])
        out = Fixture(self).run('install.sh', '--dry-run', '--gaming', '--leave-open', stdin='').stdout
        self.assertIn('leave SteamOS', out)
        self.assertNotIn('write one small file', out)


class DesktopDistroTests(unittest.TestCase):
    """Card #502, Flatpak on older long-term desktops (Kubuntu 26.04, Debian 13, openSUSE Leap 16): Desktop keyboard only."""

    def test_the_plan_says_nothing_about_steam_or_gaming_mode(self):
        f = Fixture(self, steam=False, system='kubuntu')
        out = f.run('install.sh', '--dry-run', stdin='').stdout
        for phrase in ('developer switch', '8081', 'Subscriber Agreement', 'Gaming Mode', 'Deck', 'administrator password'):
            self.assertNotIn(phrase, out)
        self.assertIn('Swipe Keyboard for Ubuntu', out)
        self.assertIn('System Settings', out)
        self.assertIn(MAIN_ID, out)

    def test_install_adds_only_the_keyboard_app_and_says_where_to_choose_it(self):
        f = Fixture(self, steam=True, system='kubuntu')   # even with Steam installed: no Gaming Mode on a plain desktop
        result = f.run('install.sh', '--yes')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(f.apps.read_text().split(), [MAIN_ID])
        self.assertEqual(gaming_units(f), [])
        self.assertFalse((f.steam / MARKER).exists())
        self.assertEqual(f.record()['gaming'], 'no')
        self.assertIn('Virtual Keyboard', result.stdout)
        self.assertIn('log out and back in', result.stdout, 'no Flatpak folder in this login yet')
        ok = f.run('install.sh', '--yes', extra={'XDG_DATA_DIRS': f'{f.home}/.local/share/flatpak/exports/share:/usr/share'})
        self.assertNotIn('log out and back in', ok.stdout)

    def test_gaming_mode_is_refused_on_a_desktop_and_nothing_changes(self):
        f = Fixture(self, system='kubuntu')
        before = f.snapshot()
        result = f.run('install.sh', '--gaming', '--yes')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Gaming Mode swipe is for SteamOS and Bazzite', result.stderr)
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(f.calls(), [])

    def test_without_flatpak_it_gives_the_one_time_setup_for_that_family(self):
        for system, how in (('kubuntu', 'sudo apt install flatpak'), ('leap', 'sudo zypper install flatpak')):
            with self.subTest(system=system):
                f = Fixture(self, system=system)
                before = f.snapshot()
                result = f.run('install.sh', '--yes', shims=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(how, result.stderr)
                self.assertIn('log out and back in', result.stderr)
                self.assertEqual(f.snapshot(), before)

    def test_kde_too_old_or_missing_is_refused_plainly(self):
        for version, ok in (('5.27.11', False), ('6.2.5', False), ('6.3.0', True), ('6.10.1', True)):
            with self.subTest(version=version):
                f = Fixture(self, system='kubuntu')
                result = f.run('install.sh', '--yes', extra={'FAKE_PLASMA': version})
                self.assertEqual(result.returncode == 0, ok, result.stderr)
                if not ok:
                    self.assertIn(f'this computer has {version}', result.stderr)
                    self.assertEqual(f.calls(), [])
        # without a screen plasmashell fails; the package list gives the same version (Debian epoch and revision dropped)
        for dpkg, ok in (('4:6.3.6-2', True), ('4:5.27.11-0ubuntu4', False), ('', False)):
            with self.subTest(dpkg=dpkg):
                f = Fixture(self, system='kubuntu')
                (f.shims / 'dpkg-query').write_text(DPKG_QUERY); (f.shims / 'dpkg-query').chmod(0o755)
                result = f.run('install.sh', '--yes', extra={'FAKE_PLASMA': 'crash', 'FAKE_DPKG_PLASMA': dpkg})
                self.assertEqual(result.returncode == 0, ok, result.stderr)
                if dpkg == '':
                    self.assertIn('an unknown version', result.stderr)
        f = Fixture(self, system='kubuntu')
        (f.shims / 'plasmashell').unlink()
        result = f.run('install.sh', '--yes')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('KDE Plasma was not found', result.stderr)

    def test_the_regular_package_already_installed_is_refused_with_disable_first(self):
        for system in ('kubuntu', 'steamos'):
            with self.subTest(system=system):
                f = Fixture(self, system=system)
                (f.native / 'integration').mkdir(parents=True)
                (f.native / 'integration/setup.py').write_text('')
                result = f.run('install.sh', '--yes')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('regular Swipe Keyboard package is already installed', result.stderr)
                self.assertIn('swipe-keyboard disable', result.stderr, 'switch off before removing, or no keyboard')
                self.assertEqual(f.calls(), [])

    def test_a_leftover_folder_from_a_removed_package_does_not_block(self):
        # WHY (review): package managers leave a folder with stray cache files behind; that is not an installed package
        f = Fixture(self, system='kubuntu')
        (f.native / 'integration/__pycache__').mkdir(parents=True)
        self.assertEqual(f.run('install.sh', '--yes').returncode, 0)

    def test_the_first_download_size_is_said_only_when_the_kde_runtime_is_missing(self):
        f = Fixture(self, system='kubuntu')
        self.assertIn('about 700 MB, once', f.run('install.sh', '--dry-run', stdin='').stdout)
        (f.home.parent / 'var-lib-flatpak/runtime/org.kde.Platform/x86_64/6.10').mkdir(parents=True)
        out = f.run('install.sh', '--dry-run', stdin='').stdout
        self.assertNotIn('700 MB', out)
        self.assertIn('already on this computer', out)

    def test_a_download_without_the_gaming_helper_is_fine_on_a_desktop(self):
        f = Fixture(self, system='kubuntu')
        for p in f.bundle.glob('swipe-keyboard-gaming-helper-*'):
            p.unlink()
        f.write_sums()
        self.assertEqual(f.run('install.sh', '--yes').returncode, 0)

    def test_a_system_with_a_steam_gaming_session_counts_as_a_gaming_one(self):
        f = Fixture(self, system='kubuntu')
        (f.sessions / 'gamescope-session-steam.desktop').write_text('[Desktop Entry]\n')
        out = f.run('install.sh', '--dry-run', stdin='').stdout
        self.assertIn('developer switch', out)

    def test_uninstall_removes_it_again(self):
        f = Fixture(self, system='kubuntu')
        before = f.snapshot()
        f.run('install.sh', '--yes')
        result = f.run('uninstall.sh', '--yes', '--remove-apps')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(f.apps.read_text().split(), [])
        self.assertEqual(f.snapshot(), before)
        self.assertNotIn('Steam', result.stdout, 'a desktop that never had Gaming Mode reads nothing about Steam')

    def test_keeping_the_app_names_only_the_keyboard_app(self):
        f = Fixture(self, system='kubuntu')
        f.run('install.sh', '--yes')
        out = f.run('uninstall.sh', '--dry-run').stdout
        self.assertIn(f'flatpak uninstall --user {MAIN_ID}', out)
        self.assertNotIn(HELPER_ID, out)


class RemovalCheckTests(unittest.TestCase):
    """Card #502, edge case 1: removed some other way (Discover, flatpak uninstall) while chosen -> KDE's keyboard comes back."""

    def setup(self, choice, installed=False, kde=True):
        f = Fixture(self, system='kubuntu')
        (f.home / '.config').mkdir(parents=True, exist_ok=True)
        (f.home / '.config/kwinrc').write_text(f'[Wayland]\nInputMethod={choice}\n')
        if kde:
            (f.appdir / 'org.kde.plasma.keyboard.desktop').write_text('[Desktop Entry]\n')
        if installed:
            f.apps.write_text(MAIN_ID + '\n')
        return f

    def kwrites(self, f):
        return [c for c in f.calls() if c.startswith('kwriteconfig6')]

    def test_ours_chosen_and_gone_hands_the_choice_to_kdes_keyboard_and_stops_our_jobs(self):
        f = self.setup(f'{Path.home()}/.local/share/flatpak/exports/share/applications/{MAIN_ID}.desktop')
        result = f.run('keyboard-check.sh')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.kwrites(f), [f'kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify {f.appdir}/org.kde.plasma.keyboard.desktop'])
        for unit in ('swipekb-update.timer', 'swipekb-keyboard-check.path', 'swipekb-engine.service', 'swipekb-helper.service'):
            self.assertIn(f'systemctl --user disable --now {unit}', f.calls())

    def test_the_form_system_settings_really_writes_is_recognised(self):
        # WHY exactly this line: copied from kwinrc in the Debian 13 VM after choosing Swipe Keyboard in System Settings
        f = Fixture(self, system='kubuntu')
        (f.home / '.config').mkdir(parents=True)
        (f.home / '.config/kwinrc').write_text('[Wayland]\nInputMethod[$e]=$HOME/.local/share/flatpak/exports/share/applications/org.itsdestin.SwipeKeyboard.desktop\n')
        (f.appdir / 'org.kde.plasma.keyboard.desktop').write_text('[Desktop Entry]\n')
        f.run('keyboard-check.sh')
        self.assertEqual(len(self.kwrites(f)), 1, 'the safety check sees it')
        f2 = Fixture(self, system='kubuntu')
        f2.run('install.sh', '--yes')
        (f2.home / '.config/kwinrc').write_text((f.home / '.config/kwinrc').read_text())
        f2.run('uninstall.sh', '--yes', '--remove-apps')
        self.assertTrue([c for c in f2.calls() if c.startswith('kwriteconfig6')], 'and so does the uninstaller')

    def test_without_kdes_keyboard_the_choice_is_cleared(self):
        f = self.setup(f'{MAIN_ID}.desktop', kde=False)
        f.run('keyboard-check.sh')
        self.assertEqual(self.kwrites(f), ['kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify '])

    def test_nothing_changes_while_the_app_is_installed(self):
        for chosen in (f'{MAIN_ID}.desktop', '/usr/share/applications/org.kde.plasma.keyboard.desktop'):
            f = self.setup(chosen, installed=True)
            before = f.snapshot()
            f.run('keyboard-check.sh')
            self.assertEqual([c for c in f.calls() if not c.startswith('flatpak info')], [])
            self.assertEqual(f.snapshot(), before)

    def test_app_gone_but_another_keyboard_chosen_only_stops_our_jobs(self):
        # WHY (review): the Gaming Mode engine would otherwise restart every two minutes for an app that is gone
        for chosen in ('/usr/share/applications/org.kde.plasma.keyboard.desktop',
                       '/usr/share/applications/not-org.itsdestin.SwipeKeyboard.desktop.bak'):
            f = self.setup(chosen)
            f.run('keyboard-check.sh')
            self.assertEqual(self.kwrites(f), [])
            self.assertIn('systemctl --user disable --now swipekb-engine.service', f.calls())

    def test_no_kwinrc_means_no_keyboard_change(self):
        f = Fixture(self, system='kubuntu')
        f.run('keyboard-check.sh')
        self.assertEqual([c for c in f.calls() if c.startswith('kwriteconfig6')], [])

    def test_the_installer_sets_it_up_and_says_so(self):
        f = Fixture(self, system='kubuntu')
        self.assertIn('clear that choice', f.run('install.sh', '--dry-run', stdin='').stdout, 'Debian 13 has no KDE keyboard')
        (f.appdir / 'org.kde.plasma.keyboard.desktop').write_text('[Desktop Entry]\n')
        out = f.run('install.sh', '--dry-run', stdin='').stdout
        self.assertIn("hand the choice back to your system's", out)
        f.run('install.sh', '--yes')
        script = f.home / '.local/share/swipe-keyboard-gaming/keyboard-check.sh'
        self.assertTrue(os.access(script, os.X_OK))
        self.assertIn(f'ExecStart={script}', (f.units / 'swipekb-keyboard-check.service').read_text())
        self.assertIn('PathChanged=%h/.local/share/flatpak/exports/share/applications', (f.units / 'swipekb-keyboard-check.path').read_text())


class KeyboardChoiceTests(unittest.TestCase):
    """Card #502, edge case 1: removing the app must never leave the person with no on-screen keyboard after the next login."""
    OURS = '/home/x/.local/share/flatpak/exports/share/applications/org.itsdestin.SwipeKeyboard.desktop'

    def setup(self, chosen, kde_keyboard=True):
        f = Fixture(self)
        f.run('install.sh', '--desktop-only', '--yes')
        (f.home / '.config').mkdir(exist_ok=True)
        (f.home / '.config/kwinrc').write_text(f'[Wayland]\nInputMethod={chosen}\n')
        if kde_keyboard:
            (f.appdir / 'org.kde.plasma.keyboard.desktop').write_text('[Desktop Entry]\n')
        f.log.write_text('')
        return f

    def keyboard_calls(self, f):
        return [c for c in f.calls() if c.startswith('kwriteconfig6')]

    def test_removing_the_chosen_keyboard_hands_the_choice_back_to_kdes_own(self):
        f = self.setup(self.OURS)
        result = f.run('uninstall.sh', '--yes', '--remove-apps')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.keyboard_calls(f), [
            f'kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify {f.appdir}/org.kde.plasma.keyboard.desktop'])
        self.assertIn("your system's own", result.stdout)

    def test_without_kdes_keyboard_the_choice_becomes_none_and_says_so(self):
        f = self.setup(self.OURS, kde_keyboard=False)
        result = f.run('uninstall.sh', '--yes', '--remove-apps')
        self.assertEqual(self.keyboard_calls(f), ['kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify '])
        self.assertIn('System Settings', result.stdout)

    def test_another_chosen_keyboard_is_left_alone(self):
        f = self.setup('/usr/share/applications/org.fcitx.Fcitx5.desktop')
        f.run('uninstall.sh', '--yes', '--remove-apps')
        self.assertEqual(self.keyboard_calls(f), [])

    def test_keeping_the_app_still_hands_the_choice_back(self):
        # WHY (review, card #502): this uninstall removes the safety check too; keeping the choice would leave no keyboard
        # the day the app is removed in Discover.
        f = self.setup(self.OURS)
        f.run('uninstall.sh', '--yes')
        self.assertEqual(self.keyboard_calls(f), [
            f'kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify {f.appdir}/org.kde.plasma.keyboard.desktop'])

    def test_maliit_is_the_fallback_where_kde_has_no_keyboard(self):
        f = self.setup(self.OURS, kde_keyboard=False)
        (f.appdir / 'com.github.maliit.keyboard.desktop').write_text('[Desktop Entry]\n')
        f.run('uninstall.sh', '--yes', '--remove-apps')
        self.assertEqual(self.keyboard_calls(f), [
            f'kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify {f.appdir}/com.github.maliit.keyboard.desktop'])

    def test_the_plan_mentions_it(self):
        f = self.setup(self.OURS)
        out = f.run('uninstall.sh', '--dry-run', '--remove-apps').stdout
        self.assertIn('Virtual Keyboard', out)
        self.assertEqual(self.keyboard_calls(f), [])


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
        self.assertIn('systemctl --user disable --now swipekb-engine.service swipekb-helper.service swipekb-switch-watch.service swipekb-switch-watch.timer swipekb-update.service swipekb-update.timer swipekb-keyboard-check.path swipekb-keyboard-check.service', calls)
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



class AutoUpdateTests(unittest.TestCase):
    """Automatic updates on the Deck (plan: docs/active/plans/2026-10-23-auto-updates.md)."""

    def test_on_by_default_it_points_both_apps_at_the_signed_site_and_starts_the_daily_timer(self):
        f = Fixture(self)
        self.assertEqual(f.run('install.sh', '--gaming', '--leave-open', '--yes').returncode, 0)
        calls = f.calls()
        for origin in ('orgitsdestinswipekeyboard-origin', 'orgitsdestinswipekeyboardgaminghelper-origin'):
            self.assertIn(f'flatpak remote-modify --user --url=https://itsdestin.github.io/swipe-keyboard/repo/ --gpg-verify '
                          f'--gpg-import={f.bundle}/update-key.gpg {origin}', calls)
        self.assertIn('systemctl --user enable --now swipekb-update.timer', calls)
        service = (f.units / 'swipekb-update.service').read_text()
        self.assertIn(f'ExecStart={f.home}/.local/share/swipe-keyboard-gaming/auto-update.sh', service)
        self.assertNotIn('@', service)
        self.assertTrue((f.home / '.local/share/swipe-keyboard-gaming/auto-update.sh').stat().st_mode & 0o111)

    def test_an_origin_flatpak_already_dropped_is_recreated_with_the_key(self):
        # WHY: seen on SteamOS: right after the helper bundle was installed its origin remote no longer existed.
        f = Fixture(self)
        self.assertEqual(f.run('install.sh', '--gaming', '--leave-open', '--yes', extra={'FAKE_LOST_ORIGIN': '1'}).returncode, 0)
        self.assertIn(f'flatpak remote-add --user --no-enumerate --gpg-import={f.bundle}/update-key.gpg '
                      'orgitsdestinswipekeyboardgaminghelper-origin https://itsdestin.github.io/swipe-keyboard/repo/', f.calls())
        # The main app's origin still exists, so it is modified, never re-added (re-adding fails: "already exists").
        self.assertFalse([c for c in f.calls() if c.startswith('flatpak remote-add') and 'orgitsdestinswipekeyboard-origin' in c])

    def test_no_auto_update_adds_nothing_and_is_remembered(self):
        f = Fixture(self)
        self.assertEqual(f.run('install.sh', '--no-auto-update', '--yes').returncode, 0)
        self.assertFalse((f.units / 'swipekb-update.timer').exists())
        self.assertEqual([c for c in f.calls() if 'remote-modify' in c or 'swipekb-update' in c], [])
        self.assertEqual(f.record()['auto_update'], 'no')
        f.log.write_text('')
        self.assertEqual(f.run('install.sh', '--yes').returncode, 0)      # a plain re-run keeps your "no"
        self.assertFalse((f.units / 'swipekb-update.timer').exists())
        self.assertEqual(f.record()['auto_update'], 'no')

    def test_turning_it_off_later_removes_only_the_update_timer(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--leave-open', '--yes')
        f.log.write_text('')
        self.assertEqual(f.run('install.sh', '--no-auto-update', '--yes').returncode, 0)
        self.assertFalse((f.units / 'swipekb-update.timer').exists())
        self.assertFalse((f.home / '.local/share/swipe-keyboard-gaming/auto-update.sh').exists())
        self.assertEqual(gaming_units(f), ['swipekb-engine.service', 'swipekb-helper.service',
                                           'swipekb-switch-watch.service', 'swipekb-switch-watch.timer'])
        self.assertNotIn('swipekb-update.service', f.record()['created_units'])
        self.assertIn('systemctl --user disable --now swipekb-update.timer', f.calls())

    def run_update(self, f, **extra):
        script = f.home / '.local/share/swipe-keyboard-gaming/auto-update.sh'
        env = {'HOME': str(f.home), 'PATH': f'{f.shims}:{f.tools}', 'FAKE_LOG': str(f.log), 'FAKE_APPS': str(f.apps), **extra}
        return subprocess.run(['bash', str(script)], env=env, capture_output=True, text=True, timeout=60)

    def test_the_daily_job_updates_only_our_apps_and_restarts_gaming_services_after_a_change(self):
        f = Fixture(self)
        f.run('install.sh', '--gaming', '--leave-open', '--yes')
        f.log.write_text('')
        self.assertEqual(self.run_update(f).returncode, 0)                 # nothing new: no restart
        self.assertEqual(f.calls(), [c for c in f.calls() if c.startswith('flatpak ')])
        self.assertIn(f'flatpak update --user --noninteractive -y {MAIN_ID}', f.calls())
        self.assertIn(f'flatpak update --user --noninteractive -y {HELPER_ID}', f.calls())
        f.log.write_text('')
        result = self.run_update(f, FAKE_NEW='1')
        self.assertEqual(result.returncode, 0)
        self.assertIn('was updated', result.stdout)
        self.assertIn('systemctl --user try-restart swipekb-engine.service swipekb-helper.service', f.calls())

    def test_a_failed_update_check_changes_nothing_and_reports_failure(self):
        f = Fixture(self)
        f.run('install.sh', '--yes')
        f.log.write_text('')
        result = self.run_update(f, FAKE_FAIL='update')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([c for c in f.calls() if c.startswith('systemctl')], [])


if __name__ == '__main__':
    unittest.main()
