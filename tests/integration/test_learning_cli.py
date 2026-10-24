# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""The on/off/forget switch for "learn from my typing" (integration/learning.py and `swipe-keyboard learning`)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'integration'))
import learning  # noqa: E402


class LearningSwitchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.data = Path(self.tmp.name) / 'swipe-keyboard'
        self.addCleanup(self.tmp.cleanup)
        self.said = []

    def run_action(self, action):
        return learning.run(action, self.data, out=self.said.append)

    def test_nothing_exists_until_it_is_switched_on(self):
        self.assertFalse((self.data / 'learn-typing').exists())
        self.run_action('status')
        self.assertIn('OFF', self.said[-1])

    def test_on_creates_only_the_flag_and_off_removes_it_but_keeps_the_words(self):
        self.run_action('on')
        self.assertTrue((self.data / 'learn-typing').is_file())
        (self.data / 'learning').mkdir()
        (self.data / 'learning' / 'en.swklearn').write_text('x')
        self.run_action('off')
        self.assertFalse((self.data / 'learn-typing').exists())
        self.assertTrue((self.data / 'learning' / 'en.swklearn').exists())  # off is not erase

    def test_forget_erases_every_language_file_and_leaves_a_marker_for_a_running_keyboard(self):
        (self.data / 'learning').mkdir(parents=True)
        for lang in ('en', 'es', 'fr', 'de'):
            (self.data / 'learning' / f'{lang}.swklearn').write_text('private')
        (self.data / 'learn-typing').touch()
        self.run_action('forget')
        self.assertFalse((self.data / 'learning').exists())
        self.assertTrue((self.data / 'learning-reset').is_file())
        self.assertTrue((self.data / 'learn-typing').exists())  # forgetting does not switch learning off

    def test_forget_works_when_nothing_was_ever_learned(self):
        self.run_action('forget')
        self.assertTrue((self.data / 'learning-reset').exists())

    def test_status_reports_sizes_never_words(self):
        (self.data / 'learning').mkdir(parents=True)
        (self.data / 'learning' / 'en.swklearn').write_text('secret words here')
        self.run_action('status')
        self.assertIn('1 language file', self.said[-1])
        self.assertNotIn('secret', self.said[-1])

    def test_data_folder_follows_the_keyboards_own_rule(self):
        self.assertEqual(learning.default_data({'SWIPEKB_DATA': '/x/y'}), Path('/x/y'))
        self.assertEqual(learning.default_data({'XDG_DATA_HOME': '/d', 'HOME': '/h'}), Path('/d/swipe-keyboard'))
        self.assertEqual(learning.default_data({'HOME': '/h'}), Path('/h/.local/share/swipe-keyboard'))

    def test_command_line_runs_as_a_script_and_through_the_package_command(self):
        env = dict(os.environ, SWIPEKB_DATA=str(self.data), HOME=self.tmp.name)
        env.pop('XDG_DATA_HOME', None)
        done = subprocess.run([sys.executable, str(ROOT / 'integration/learning.py'), 'on'], env=env, capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue((self.data / 'learn-typing').exists())
        # The package command `swipe-keyboard learning ...` acts on <XDG data>/swipe-keyboard, the folder `launch` uses.
        env['XDG_DATA_HOME'] = self.tmp.name
        done = subprocess.run([sys.executable, str(ROOT / 'integration/setup.py'), 'learning', 'status'], env=env, capture_output=True, text=True)
        if os.geteuid() != 0:
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn('ON', done.stdout)
        bad = subprocess.run([sys.executable, str(ROOT / 'integration/setup.py'), 'learning'], env=env, capture_output=True, text=True)
        self.assertNotEqual(bad.returncode, 0)
        bad = subprocess.run([sys.executable, str(ROOT / 'integration/setup.py'), 'check', 'on'], env=env, capture_output=True, text=True)
        self.assertNotEqual(bad.returncode, 0)


if __name__ == '__main__':
    unittest.main()
