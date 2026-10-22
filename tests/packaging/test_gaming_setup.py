"""Run gaming/setup/install.sh and uninstall.sh against a throwaway HOME with stand-in `flatpak` and `systemctl`.

WHY stand-ins and a PATH that holds nothing else: the scripts must be provable without touching this computer's
real Flatpak, systemd or Steam. The PATH given to the scripts contains only symlinks to a few basic tools plus the
stand-ins, so a real `flatpak` or `systemctl` can never be reached by accident.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
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
           case "$*" in *GamingHelper*) echo org.itsdestin.SwipeKeyboard.GamingHelper >> "$FAKE_APPS";; *) echo org.itsdestin.SwipeKeyboard >> "$FAKE_APPS";; esac ;;
  uninstall) for last; do :; done; grep -vx "$last" "$FAKE_APPS" > "$FAKE_APPS.new" 2>/dev/null; mv "$FAKE_APPS.new" "$FAKE_APPS" ;;
esac
exit 0
'''
SYSTEMCTL = '''#!/bin/sh
printf 'systemctl %s\\n' "$*" >> "$FAKE_LOG"
exit 0
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
        for name, body in (('flatpak', FLATPAK), ('systemctl', SYSTEMCTL)):
            (shims / name).write_text(body)
            (shims / name).chmod(0o755)
        for name in TOOLS:
            found = shutil.which(name)
            if found:
                (tools / name).symlink_to(found)
        self.shims, self.tools = shims, tools
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

    def run(self, script, *args, stdin='y\n', fail=None, shims=True):
        env = {'HOME': str(self.home), 'PATH': (str(self.shims) + ':' if shims else '') + str(self.tools),
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
                       'no administrator', 'Nothing is sent to the internet', 'Gaming Mode', 'uninstall.sh',
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
        result = f.run('install.sh')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((f.steam / MARKER).is_file())
        engine = (f.units / 'swipekb-engine.service').read_text()
        self.assertEqual(engine.splitlines()[1], '# swipekb-gaming-version: 0.3.1-1')
        self.assertNotIn('@VERSION@', engine)
        self.assertTrue((f.units / 'swipekb-helper.service').is_file())
        record = f.record()
        self.assertEqual(record['version'], '0.3.1-1')
        self.assertEqual(record['created_markers'], [str(f.steam / MARKER)])
        self.assertEqual(record['existing_markers'], [])
        self.assertEqual(sorted(record['created_units']), ['swipekb-engine.service', 'swipekb-helper.service'])
        self.assertEqual(record['installed_flatpaks'], [MAIN_ID, HELPER_ID])
        calls = f.calls()
        installs = [c for c in calls if c.startswith('flatpak install')]
        self.assertEqual(len(installs), 2)
        self.assertIn('swipe-keyboard-0.3.1-1-x86_64.flatpak', installs[0])
        self.assertIn('swipe-keyboard-gaming-helper-0.3.1-1-x86_64.flatpak', installs[1])
        self.assertIn('systemctl --user daemon-reload', calls)
        self.assertIn('systemctl --user enable --now swipekb-engine.service swipekb-helper.service', calls)
        self.assertIn('restart Steam', result.stdout)

    def test_only_user_scope_commands_are_ever_run(self):
        # WHY: no sudo, no system-wide Flatpak, no system service (plan Section 5).
        f = Fixture(self)
        f.run('install.sh')
        f.run('uninstall.sh', '--yes', '--remove-apps')
        for call in f.calls():
            self.assertIn('--user', call, call)
        for script in ('install.sh', 'uninstall.sh'):
            # WHY comments are skipped: the header says "No sudo"; only executable lines and messages matter.
            text = '\n'.join(l for l in (SETUP / script).read_text().splitlines() if not l.lstrip().startswith('#'))
            for forbidden in ('sudo', 'pkexec', 'doas', 'firewall-cmd', 'nft ', 'iptables', 'steamos-readonly', '/usr/lib/systemd/system'):
                self.assertNotIn(forbidden, text, f'{script} contains {forbidden}')

    def test_existing_marker_is_left_alone_and_recorded_as_not_ours(self):
        f = Fixture(self)
        marker = f.steam / MARKER
        marker.write_text('decky was here')
        os.utime(marker, (1000, 1000))
        result = f.run('install.sh')
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
        f.run('install.sh')
        first = {k: v for k, v in f.snapshot().items() if k != '.local/share/swipe-keyboard-gaming/install-record.json'}
        result = f.run('install.sh')
        self.assertEqual(result.returncode, 0, result.stderr)
        second = {k: v for k, v in f.snapshot().items() if k != '.local/share/swipe-keyboard-gaming/install-record.json'}
        self.assertEqual(first, second)
        record = f.record()
        self.assertEqual(record['created_markers'], [str(f.steam / MARKER)])
        self.assertEqual(record['existing_markers'], [])
        self.assertEqual(record['installed_flatpaks'], [MAIN_ID, HELPER_ID])

    def test_steam_folder_reached_two_ways_gets_one_marker(self):
        f = Fixture(self, symlink=True)
        result = f.run('install.sh')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(f.record()['created_markers'], [str(f.steam / MARKER)])

    def test_refuses_when_steam_has_never_run(self):
        f = Fixture(self, steam=False)
        before = f.snapshot()
        result = f.run('install.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Steam', result.stderr)
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(f.calls(), [])

    def test_refuses_tampered_download_before_changing_anything(self):
        f = Fixture(self)
        (f.bundle / 'swipe-keyboard-0.3.1-1-x86_64.flatpak').write_bytes(b'tampered')
        before = f.snapshot()
        result = f.run('install.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('checksum', result.stderr.lower())
        self.assertEqual(f.snapshot(), before)
        self.assertEqual(f.calls(), [])

    def test_refuses_without_flatpak_or_systemctl(self):
        f = Fixture(self)
        result = f.run('install.sh', shims=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('flatpak', result.stderr)
        self.assertEqual(f.snapshot(), {})

    def test_never_overwrites_a_unit_file_it_did_not_write(self):
        f = Fixture(self)
        f.units.mkdir(parents=True)
        (f.units / 'swipekb-helper.service').write_text('[Service]\nExecStart=/bin/true\n')
        before = f.snapshot()
        result = f.run('install.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('swipekb-helper.service', result.stderr)
        self.assertEqual(f.snapshot(), before)  # refused up front: not even the marker was made

    def test_flatpak_failure_leaves_no_marker_or_units(self):
        # WHY: the network-dependent step runs first so a failure cannot leave a half-installed system.
        f = Fixture(self)
        result = f.run('install.sh', fail='install')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((f.steam / MARKER).exists())
        self.assertFalse(f.units.exists())

    def test_skip_flatpaks_installs_no_apps_and_records_none(self):
        f = Fixture(self)
        result = f.run('install.sh', '--skip-flatpaks')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([c for c in f.calls() if c.startswith('flatpak install')], [])
        self.assertEqual(f.record()['installed_flatpaks'], [])

    def test_upgrade_rewrites_stamped_units_restarts_and_refuses_downgrade(self):
        f = Fixture(self)
        f.run('install.sh')
        f.make_bundle('0.3.2-1')
        f.log.write_text('')
        result = f.run('install.sh')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((f.units / 'swipekb-engine.service').read_text().splitlines()[1], '# swipekb-gaming-version: 0.3.2-1')
        self.assertIn('systemctl --user restart swipekb-engine.service swipekb-helper.service', f.calls())
        self.assertEqual(f.record()['version'], '0.3.2-1')
        self.assertEqual(f.record()['created_markers'], [str(f.steam / MARKER)])
        f.make_bundle('0.3.1-1')
        result = f.run('install.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('older', result.stderr)
        self.assertEqual(f.record()['version'], '0.3.2-1')
        self.assertEqual(f.run('install.sh', '--force').returncode, 0)
        self.assertEqual(f.record()['version'], '0.3.1-1')

    def test_network_exposure_hook_is_present_but_does_nothing(self):
        # Plan Section 15 question 18 is an open owner decision: the 8081 hardening is NOT built yet.
        text = (SETUP / 'install.sh').read_text()
        self.assertIn('network_exposure_hook', text)
        self.assertIn('OWNER DECISION', text)
        self.assertIn('question 18', text)
        body = text[text.index('network_exposure_hook() {'):]
        body = body[:body.index('\n}\n')]
        code = [l for l in body.splitlines()[1:] if l.strip() and not l.strip().startswith('#')]
        self.assertEqual(code, ['    :'])


class UninstallTests(unittest.TestCase):
    def installed(self, **kwargs):
        f = Fixture(self, **kwargs)
        self.assertEqual(f.run('install.sh').returncode, 0)
        f.log.write_text('')
        return f

    def test_install_then_uninstall_restores_the_home_folder_exactly(self):
        f = Fixture(self)
        before = f.snapshot()
        f.run('install.sh')
        (f.home / '.local/state/swipekb-gaming').mkdir(parents=True)
        (f.home / '.local/state/swipekb-gaming/status.json').write_text('{}')  # what the helper writes
        result = f.run('uninstall.sh', '--yes')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(f.snapshot(), before)
        calls = f.calls()
        self.assertIn('systemctl --user disable --now swipekb-engine.service swipekb-helper.service', calls)
        self.assertIn('systemctl --user daemon-reload', calls)
        self.assertEqual([c for c in calls if c.startswith('flatpak uninstall')], [])
        self.assertIn('flatpak uninstall --user', result.stdout)  # printed as a hint, not run
        self.assertIn('next restart', result.stdout)

    def test_pre_existing_marker_survives_uninstall(self):
        f = Fixture(self)
        (f.steam / MARKER).write_text('mine')
        f.run('install.sh')
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
        f.run('install.sh')
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


if __name__ == '__main__':
    unittest.main()
