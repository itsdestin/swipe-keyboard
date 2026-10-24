# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Activation must not take ownership of a convertible manager's visibility policy."""
import json
import unittest
import test_setup


class VisibilityPolicy(unittest.TestCase):
    def setUp(self):
        self.fixture = test_setup.SetupTests('test_native_arm_passes_same_compatibility_and_load_gates_as_x86')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.controller = self.fixture.controller
        self.commands = self.fixture.commands

    def mode_writes(self):
        return [call for call in self.commands.calls
                if ('set-property' in call and call[-3:-1] == ['mode', 'i'])
                or (call[0].endswith('kwriteconfig6') and 'VirtualKeyboardMode' in call)]

    def test_default_preserves_desktop_tablet_and_touch_only_modes(self):
        for mode in (0, 1, 2):
            with self.subTest(mode=mode):
                self.commands.settings['VirtualKeyboardMode'] = str(mode)
                self.commands.live_mode = mode
                self.controller.enable()
                self.assertEqual(self.commands.live_mode, mode)
                self.assertEqual(self.commands.settings['VirtualKeyboardMode'], str(mode))
                self.assertEqual(self.mode_writes(), [])
                self.controller.disable()
                self.assertEqual(self.commands.live_mode, mode)
                self.assertEqual(self.mode_writes(), [])

    def test_locked_visibility_policy_does_not_block_preserving_activation(self):
        self.commands.immutable = {'VirtualKeyboardMode'}
        self.controller.enable()
        self.assertEqual(self.commands.live_mode, 0)
        self.controller.disable()
        self.assertEqual(self.mode_writes(), [])

    def test_opt_in_restores_previous_mode_when_still_owned(self):
        self.controller.enable(always_available=True)
        self.assertEqual(self.commands.live_mode, 1)
        self.controller.disable()
        self.assertEqual(self.commands.live_mode, 0)
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '0')

    def test_external_tablet_desktop_switch_stays_authoritative(self):
        self.controller.enable()
        # External manager captures the unchanged desktop baseline, switches to
        # tablet and back on physical-keyboard reconnect. Swipe never rewrites it.
        desktop_baseline = self.commands.live_mode
        self.assertEqual(desktop_baseline, 0)
        for mode in (1, desktop_baseline):
            self.commands.live_mode = mode
            self.commands.settings['VirtualKeyboardMode'] = str(mode)
            self.controller.enable()  # repeat activation/layout regeneration
            self.assertEqual(self.commands.live_mode, mode)
        self.controller.disable()
        self.assertEqual(self.commands.live_mode, 0)
        self.assertEqual(self.mode_writes(), [])

    def test_preserves_inherited_absence_and_later_manager_change(self):
        self.commands.settings.pop('VirtualKeyboardMode')
        self.commands.defaults['VirtualKeyboardMode'] = '0'
        self.controller.enable()
        self.assertNotIn('VirtualKeyboardMode', self.commands.settings)
        self.commands.live_mode = 1
        self.commands.defaults['VirtualKeyboardMode'] = '1'
        self.controller.disable()
        self.assertNotIn('VirtualKeyboardMode', self.commands.settings)
        self.assertEqual(self.commands.live_mode, 1)
        self.assertEqual(self.mode_writes(), [])

    def test_always_available_requires_explicit_choice_and_preserves_later_policy(self):
        self.controller.enable(always_available=True)
        self.assertEqual(self.commands.live_mode, 1)
        self.commands.live_mode = 0
        self.commands.settings['VirtualKeyboardMode'] = '0'
        self.controller.disable()
        self.assertEqual(self.commands.live_mode, 0)
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '0')

    def test_schema_two_repeat_preserves_journal_then_restores_owned_mode(self):
        self.controller.enable(always_available=True)
        state = json.loads(self.controller.state_file.read_text())
        state['schema'] = 2
        state.pop('visibility_policy')
        state['mode_restore'] = {'before': '1', 'target': '0'}
        self.fixture.module.atomic_json(self.controller.state_file, state)
        before = self.controller.state_file.read_bytes()
        self.commands.calls.clear()
        # WHY an upgrade must neither relabel old ownership as preserve nor lose
        # an interrupted restoration journal during default repeated activation.
        self.controller.enable()
        self.assertEqual(self.controller.state_file.read_bytes(), before)
        self.assertEqual(self.mode_writes(), [])
        self.controller.disable()
        self.assertEqual(self.commands.live_mode, 0)
        self.assertEqual(self.commands.settings['VirtualKeyboardMode'], '0')

    def test_preserve_record_rejects_visibility_journal_without_writes(self):
        self.controller.enable()
        state = json.loads(self.controller.state_file.read_text())
        self.assertEqual(state['schema'], 3)
        self.assertEqual(state['visibility_policy'], 'preserve')
        state['mode_restore'] = {'before': '1', 'target': '0'}
        self.fixture.module.atomic_json(self.controller.state_file, state)
        before = self.controller.state_file.read_bytes()
        self.commands.calls.clear()
        with self.assertRaisesRegex(self.fixture.module.SetupError, 'damaged'):
            self.controller.disable()
        self.assertEqual(self.mode_writes(), [])
        self.assertEqual(self.controller.state_file.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
