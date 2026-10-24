# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Activation consent at the CLI boundary; never change live desktop settings."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('setup_consent', ROOT/'integration/setup.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class Consent(unittest.TestCase):
    def controller(self):
        controller = mock.Mock()
        controller.config_state.return_value = {
            'InputMethod': {'raw': 'old.desktop', 'effective': 'old.desktop', 'immutable': False},
            'VirtualKeyboardMode': {'raw': '0', 'effective': '0', 'immutable': False}}
        controller.wanted = {'InputMethod': 'swipe.desktop', 'VirtualKeyboardMode': '1'}
        return controller

    def invoke(self, answer=None, yes=False, interactive=True):
        controller = self.controller()
        output = io.StringIO()
        with contextlib.redirect_stdout(output), mock.patch.object(setup.sys.stdin, 'isatty', return_value=interactive):
            with mock.patch('builtins.input', return_value=answer) as prompt:
                result = setup.confirm_activation(controller, assume_yes=yes)
        return result, controller, output.getvalue(), prompt

    def test_decline_is_default_and_does_not_activate(self):
        for answer in ('', 'n', 'no', 'unexpected'):
            result, controller, output, prompt = self.invoke(answer)
            self.assertFalse(result)
            controller.enable.assert_not_called()
            controller.set_live_mode.assert_not_called()
            self.assertIn('No settings changed', output)
            prompt.assert_called_once()

    def test_explains_current_and_proposed_settings_before_yes(self):
        result, _, output, _ = self.invoke('y')
        self.assertTrue(result)
        for text in ('old.desktop', 'swipe.desktop', '0', '(preserved)', 'You may notice',
                     'system backend', 'prediction', 'disable'):
            self.assertIn(text, output)

    def test_noninteractive_requires_explicit_yes(self):
        result, _, output, prompt = self.invoke(interactive=False)
        self.assertFalse(result)
        prompt.assert_not_called()
        self.assertIn('--yes', output)
        result, _, output, prompt = self.invoke(yes=True, interactive=False)
        self.assertTrue(result)
        prompt.assert_not_called()
        self.assertIn('You may notice', output)

    def test_explicit_visibility_opt_in_is_explained_before_confirmation(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), \
             mock.patch.object(setup.sys.stdin, 'isatty', return_value=True), \
             mock.patch('builtins.input', return_value='y'):
            self.assertTrue(setup.confirm_activation(self.controller(), always_available=True))
        self.assertIn("'0' -> '1'", output.getvalue())
        self.assertIn('physical keyboard', output.getvalue())
        self.assertNotIn('(preserved)', output.getvalue())

    def test_cli_forwards_only_explicit_visibility_choice(self):
        for option, expected in (([], False), (['--always-available'], True)):
            with self.subTest(option=option):
                controller = self.controller()
                with mock.patch.object(setup.os, 'geteuid', return_value=1000), \
                     mock.patch.object(setup.sys, 'argv', ['swipe-keyboard', 'enable', '--yes', *option]), \
                     mock.patch.object(setup, 'Controller', return_value=controller), \
                     mock.patch.object(setup, 'confirm_activation', return_value=True) as confirm, \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(setup.main(), 0)
                confirm.assert_called_once_with(controller, assume_yes=True, always_available=expected)
                controller.enable.assert_called_once_with(always_available=expected)

    def test_cli_decline_never_calls_enable(self):
        controller = self.controller()
        with mock.patch.object(setup.os, 'geteuid', return_value=1000), \
             mock.patch.object(setup.sys, 'argv', ['swipe-keyboard', 'enable']), \
             mock.patch.object(setup, 'Controller', return_value=controller), \
             mock.patch.object(setup, 'confirm_activation', return_value=False, create=True), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(setup.main(), 0)
        controller.enable.assert_not_called()


if __name__ == '__main__':
    unittest.main()
