"""gaming/setup/switch-watch.sh against a throwaway HOME with stand-in desktop tools.

WHY: the keyboard helper cannot see Steam's folder, so this host-side script is the only thing that notices the
developer-switch file is gone. It must only LOOK (never write in Steam's folder), tell the person once per
occurrence, and only put the file back when asked (owner decision 2026-10-22: notify with a recovery option, never
silently replace).
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'gaming/setup/switch-watch.sh'
MARKER = '.cef-enable-remote-debugging'
TOOLS = ['bash', 'sh', 'sed', 'grep', 'cat', 'head', 'readlink', 'mkdir', 'rm', 'mv', 'printf', 'dirname', 'tr', 'touch', 'env']

STANDINS = {
    # a notification server is present unless FAKE_NO_SERVER is set
    'gdbus': '''#!/bin/sh
printf "gdbus %s\\n" "$*" >> "$FAKE_LOG"
case "$*" in
  *NameHasOwner*) [ -n "$FAKE_NO_SERVER" ] && echo "(false,)" || echo "(true,)" ;;
  *GetNameOwner*) echo "(':1.42',)" ;;
  *GetConnectionUnixProcessID*) echo "(uint32 ${FAKE_OWNER_PID:-$PPID},)" ;;
esac
''',
    # --help mentions --action unless FAKE_OLD_NOTIFY; with actions it prints the chosen action id
    'notify-send': '''#!/bin/sh
case "$*" in *--help*) [ -n "$FAKE_OLD_NOTIFY" ] && echo "usage: notify-send [OPTION]" || echo "usage: notify-send --action=NAME=Text"; exit 0;; esac
printf "notify-send %s\\n" "$*" >> "$FAKE_LOG"
case "$*" in *--action*) echo "$FAKE_CHOICE";; esac
''',
    # runs the command at once (the real one starts it in the background)
    'systemd-run': '#!/bin/sh\nprintf "systemd-run %s\\n" "$*" >> "$FAKE_LOG"\nwhile [ "$1" != -- ]; do shift; done; shift; exec "$@"\n',
    'pgrep': '#!/bin/sh\n[ -n "$FAKE_STEAM_PID" ] && echo "$FAKE_STEAM_PID"; exit 0\n',
    'konsole': '#!/bin/sh\nprintf "konsole %s\\n" "$*" >> "$FAKE_LOG"\n',
}


class Rig:
    def __init__(self, test, steam=True, marker=True):
        self.tmp = tempfile.TemporaryDirectory()
        test.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.home = base / 'home'
        self.steam = self.home / '.local/share/Steam'
        if steam:
            self.steam.mkdir(parents=True)
            (self.home / '.steam').mkdir()
            (self.home / '.steam/steam').symlink_to(self.steam)
        self.marker = self.steam / MARKER
        if steam and marker:
            self.marker.write_text('')
        self.state = self.home / '.local/state/swipekb-gaming'
        self.log = base / 'calls.log'
        self.log.write_text('')
        shims, tools = base / 'shims', base / 'tools'
        shims.mkdir(), tools.mkdir()
        for name, body in STANDINS.items():
            (shims / name).write_text(body)
            (shims / name).chmod(0o755)
        for name in TOOLS:
            found = shutil.which(name)
            if found:
                (tools / name).symlink_to(found)
        self.shims, self.tools = shims, tools

    def run(self, *args, stdin='', env=None):
        e = {'HOME': str(self.home), 'PATH': f'{self.shims}:{self.tools}', 'LANG': 'C', 'FAKE_LOG': str(self.log), **(env or {})}
        return subprocess.run(['bash', str(SCRIPT), *args], input=stdin, env=e, capture_output=True, text=True, timeout=30)

    def word(self):
        try:
            return (self.state / 'switch-state').read_text().strip()
        except OSError:
            return None

    def calls(self, prefix):
        return [l for l in self.log.read_text().splitlines() if l.startswith(prefix)]

    def notifications(self):
        return [l for l in self.calls('notify-send') if '--action' in l or 'Turn it back on' in l or 'Swipe Keyboard:' in l]

    def steam_snapshot(self):
        return {str(p.relative_to(self.steam)): (p.stat().st_mtime_ns, p.read_bytes()) for p in sorted(self.steam.rglob('*')) if p.is_file()}


class CheckTests(unittest.TestCase):
    def test_switch_present_is_ok_and_says_nothing(self):
        r = Rig(self)
        self.assertEqual(r.run('check').returncode, 0)
        self.assertEqual(r.word(), 'ok')
        self.assertEqual(r.calls('notify-send'), [])

    def test_check_never_writes_in_steams_folder(self):
        for marker in (True, False):
            r = Rig(self, marker=marker)
            (r.steam / 'config').mkdir()
            (r.steam / 'config/login.vdf').write_text('secret-looking')
            before = r.steam_snapshot()
            r.run('check')
            self.assertEqual(r.steam_snapshot(), before)
            self.assertEqual(sorted(p.name for p in r.steam.iterdir()), sorted(['config'] + ([MARKER] if marker else [])))

    def test_switch_gone_is_off_and_the_person_is_told_once_with_two_buttons(self):
        r = Rig(self, marker=False)
        r.run('check')
        r.run('check')
        r.run('check')
        self.assertEqual(r.word(), 'off')
        shown = r.notifications()
        self.assertEqual(len(shown), 1, shown)
        self.assertIn('--action=restore=Turn it back on', shown[0])
        self.assertIn('--action=dismiss=Dismiss', shown[0])
        self.assertIn("developer switch was turned off", shown[0])
        self.assertIn("Steam's normal keyboard is in use", shown[0])

    def test_told_again_only_if_it_happens_again_after_being_fixed(self):
        r = Rig(self, marker=False)
        r.run('check')
        r.marker.write_text('')
        r.run('check')
        self.assertEqual(r.word(), 'ok')
        r.run('check')
        self.assertEqual(len(r.notifications()), 1)
        r.marker.unlink()
        r.run('check')
        self.assertEqual(len(r.notifications()), 2)

    def test_no_notification_server_means_try_again_later_not_given_up(self):
        # Gaming Mode has no notification server; the person is told as soon as Desktop Mode has one.
        r = Rig(self, marker=False)
        r.run('check', env={'FAKE_NO_SERVER': '1'})
        self.assertEqual(r.word(), 'off')
        self.assertEqual(r.notifications(), [])
        r.run('check')
        self.assertEqual(len(r.notifications()), 1)

    def test_steams_own_notification_service_in_gaming_mode_does_not_count(self):
        # WHY (found in the SteamOS VM, round 3): in Gaming Mode the name org.freedesktop.Notifications is owned by
        # Steam's own steam_notif_daemon, which shows nothing of ours. Counting that as "told" meant the person was
        # never told, not even later in Desktop Mode. Only a real desktop's service counts.
        r = Rig(self, marker=False)
        fake = r.tools / 'steam_notif_daemon'
        fake.symlink_to(shutil.which('sleep'))
        proc = subprocess.Popen([str(fake), '60'])
        self.addCleanup(proc.kill)
        r.run('check', env={'FAKE_OWNER_PID': str(proc.pid)})
        self.assertEqual(r.notifications(), [])
        self.assertFalse((r.state / 'switch-notified').exists())
        r.run('check')       # Desktop Mode: the owner is an ordinary program
        self.assertEqual(len(r.notifications()), 1)

    def test_pressing_turn_it_back_on_opens_the_restore_in_a_window(self):
        r = Rig(self, marker=False)
        r.run('check', env={'FAKE_CHOICE': 'restore'})
        self.assertTrue(any(l.endswith(' restore') and 'switch-watch.sh' in l for l in r.calls('konsole')), r.calls('konsole'))
        self.assertFalse(r.marker.exists(), 'the button opens a window that asks first; the file is not made by the check')

    def test_dismiss_does_nothing_more(self):
        r = Rig(self, marker=False)
        r.run('check', env={'FAKE_CHOICE': 'dismiss'})
        self.assertEqual(r.calls('konsole'), [])
        self.assertFalse(r.marker.exists())

    def test_old_notification_tool_without_buttons_says_how_to_fix_in_words(self):
        r = Rig(self, marker=False)
        r.run('check', env={'FAKE_OLD_NOTIFY': '1'})
        shown = r.notifications()
        self.assertEqual(len(shown), 1)
        self.assertIn('run the Swipe Keyboard installer again', shown[0])
        self.assertIn('restore', shown[0])

    def test_no_steam_folder(self):
        r = Rig(self, steam=False)
        r.run('check')
        self.assertEqual(r.word(), 'no-steam')
        self.assertEqual(r.calls('notify-send'), [])

    def test_file_made_after_steam_started_means_steam_needs_a_restart(self):
        r = Rig(self)
        r.run('check', env={'FAKE_STEAM_PID': str(os.getpid())})
        self.assertEqual(r.word(), 'restart')

    def test_file_older_than_steam_is_ok(self):
        r = Rig(self)
        os.utime(r.marker, (1000, 1000))
        r.run('check', env={'FAKE_STEAM_PID': str(os.getpid())})
        self.assertEqual(r.word(), 'ok')


class RestoreTests(unittest.TestCase):
    def test_restore_shows_what_it_will_do_asks_and_makes_only_the_missing_file(self):
        r = Rig(self, marker=False)
        r.run('check')
        result = r.run('restore', stdin='y\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(r.marker), result.stdout)
        self.assertIn('change nothing else', result.stdout)
        self.assertIn('restart Steam', result.stdout)
        self.assertTrue(r.marker.is_file())
        self.assertEqual(r.marker.read_bytes(), b'')
        self.assertFalse((r.state / 'switch-notified').exists())

    def test_declining_changes_nothing(self):
        r = Rig(self, marker=False)
        for answer in ('n\n', '\n', ''):
            result = r.run('restore', stdin=answer)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(r.marker.exists())

    def test_yes_flag_does_not_ask(self):
        r = Rig(self, marker=False)
        self.assertEqual(r.run('restore', '--yes').returncode, 0)
        self.assertTrue(r.marker.exists())

    def test_nothing_to_do_when_the_file_is_there(self):
        r = Rig(self)
        os.utime(r.marker, (1000, 1000))
        result = r.run('restore', stdin='y\n')
        self.assertIn('Nothing to do', result.stdout)
        self.assertEqual(int(r.marker.stat().st_mtime), 1000)


if __name__ == '__main__':
    unittest.main()
