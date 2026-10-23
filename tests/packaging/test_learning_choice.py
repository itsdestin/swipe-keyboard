"""install.sh's question "Learn from your typing?" (scripts/learning-choice.sh), tested without installing anything."""
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'scripts/learning-choice.sh'


def choose(data, env_extra=None, answers=None):
    """Runs learning_choice for `data`. answers=None -> no terminal; otherwise a pty typing those answers."""
    env = {k: v for k, v in os.environ.items() if k != 'SWIPEKB_LEARN_TYPING'}
    env.update(env_extra or {})
    command = ['sh', '-c', f'. "{SCRIPT}"; learning_choice "{data}"']
    if answers is None:
        done = subprocess.run(command, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True)
        return done.returncode, done.stdout
    master, slave = pty.openpty()
    process = subprocess.Popen(command, env=env, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    output = b''
    sent = False
    while True:
        ready, _, _ = select.select([master], [], [], 5)
        if not ready:
            process.kill()
            break
        try:
            chunk = os.read(master, 4096)
        except OSError:
            break
        if not chunk:
            break
        output += chunk
        if not sent and b'[y/N]' in output:
            os.write(master, answers.encode() + b'\n')
            sent = True
    process.wait(timeout=5)
    os.close(master)
    return process.returncode, output.decode()


class LearningChoiceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.data = Path(self.tmp.name) / 'swipe-keyboard'
        self.flag = self.data / 'learn-typing'

    def test_without_a_terminal_it_is_never_asked_and_stays_off(self):
        code, out = choose(self.data)
        self.assertEqual(code, 0)
        self.assertFalse(self.flag.exists())
        self.assertIn('off', out)

    def test_a_terminal_y_switches_it_on(self):
        for answer in ('y', 'Y', 'yes'):
            if self.flag.exists():
                self.flag.unlink()
            code, out = choose(self.data, answers=answer)
            self.assertEqual(code, 0)
            self.assertTrue(self.flag.exists(), answer)
            self.assertIn('Learn from your typing?', out)
            self.assertIn('never sent anywhere', out)

    def test_anything_else_and_a_bare_enter_leave_it_off(self):
        for answer in ('', 'n', 'N', 'no', 'maybe'):
            code, out = choose(self.data, answers=answer)
            self.assertEqual(code, 0)
            self.assertFalse(self.flag.exists(), repr(answer))

    def test_an_existing_choice_is_kept_and_not_asked_again(self):
        self.data.mkdir(parents=True)
        self.flag.touch()
        code, out = choose(self.data, answers=None)
        self.assertTrue(self.flag.exists())
        self.assertIn('already ON', out)
        self.assertNotIn('[y/N]', out)

    def test_the_environment_choice_skips_the_question(self):
        code, out = choose(self.data, {'SWIPEKB_LEARN_TYPING': 'yes'})
        self.assertTrue(self.flag.exists())
        self.flag.unlink()
        code, out = choose(self.data, {'SWIPEKB_LEARN_TYPING': 'no'}, answers=None)
        self.assertFalse(self.flag.exists())

    def test_install_script_asks_after_installing_and_never_in_uninstall(self):
        text = (ROOT / 'install.sh').read_text()
        self.assertIn('learning_choice "$DEST"', text)
        self.assertLess(text.index('installed to $DEST'), text.index('learning_choice "$DEST"'))
        uninstall = text[text.index('--uninstall)'):text.index('--no-restart|""')]
        self.assertNotIn('learning_choice', uninstall)


if __name__ == '__main__':
    unittest.main()
