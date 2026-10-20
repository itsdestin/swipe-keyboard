"""Deck setup disclosure is read-only; normal KDE selection remains explicit."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


class FlatpakSetup(unittest.TestCase):
    def load(self):
        path = ROOT/'packaging/flatpak/setup-guide.py'
        self.assertTrue(path.is_file(), 'Missing bundled keyboard setup disclosure')
        spec = importlib.util.spec_from_file_location('flatpak_setup_guide', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def run_guide(self, answer, interactive=True):
        module = self.load()
        output = io.StringIO()
        with contextlib.redirect_stdout(output), mock.patch.object(module.sys.stdin, 'isatty', return_value=interactive), \
             mock.patch('builtins.input', return_value=answer):
            result = module.main()
        return result, output.getvalue()

    def test_decline_explains_bundle_without_activation_instructions(self):
        result, text = self.run_guide('n')
        self.assertEqual(result, 0)
        self.assertIn('You may notice', text)
        self.assertIn('separate bundled Plasma Keyboard', text)
        self.assertIn('does not replace', text)
        self.assertIn('No settings changed', text)
        self.assertNotIn('Next steps:', text)

    def test_yes_shows_manual_selection_and_restoration(self):
        result, text = self.run_guide('y')
        self.assertEqual(result, 0)
        for expected in ('Next steps:', 'Virtual Keyboard', 'previous selection',
                         'before uninstalling', 'Gaming Mode', 'No settings changed'):
            self.assertIn(expected, text)

    def test_noninteractive_does_not_proceed(self):
        _, text = self.run_guide('', interactive=False)
        self.assertIn('interactive terminal', text)
        self.assertNotIn('Next steps:', text)

    def test_guide_is_installed_without_extra_permissions(self):
        build = (ROOT/'packaging/flatpak/build-swipe.sh').read_text()
        self.assertIn('packaging/flatpak/setup-guide.py /app/bin/swipe-keyboard-setup', build)
        manifest = (ROOT/'packaging/flatpak/org.itsdestin.SwipeKeyboard.json').read_text()
        self.assertNotIn('--filesystem=host', manifest)
        self.assertNotIn('--talk-name=org.kde.KWin', manifest)


if __name__ == '__main__':
    unittest.main()
