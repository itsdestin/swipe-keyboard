# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

import importlib.util
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('vm_touch', Path(__file__).with_name('vm-touch.py'))
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)


class Codes:
    def __getattr__(self, name):
        return name


class FakeUInput:
    def __init__(self):
        self.events = []
        self.closed = False
        self.close_count = 0

    def write(self, *event):
        if self.closed:
            raise AssertionError('write after close')
        self.events.append(event)

    def syn(self):
        self.events.append(('syn',))

    def close(self):
        self.closed = True
        self.close_count += 1


class TouchTests(unittest.TestCase):
    def test_import_does_not_load_evdev(self):
        self.assertNotIn('evdev', sys.modules)

    def test_guard_precedes_hardware_and_client_socket(self):
        with patch.object(vm.os, 'geteuid', return_value=1000), \
                patch.object(vm, 'create_device') as factory, \
                patch.object(vm.socket, 'socket') as connection:
            with self.assertRaises(PermissionError):
                vm.serve(1920, 1080)
            with self.assertRaises(PermissionError):
                vm.request({'action': 'stop'})
            factory.assert_not_called()
            connection.assert_not_called()
        for marker, vendor in [(False, 'QEMU'), (True, 'Other')]:
            with self.subTest(marker=marker, vendor=vendor), \
                    patch.object(vm.os, 'geteuid', return_value=0), \
                    patch.object(vm.Path, 'is_file', return_value=marker), \
                    patch.object(vm.Path, 'read_text', return_value=vendor):
                with self.assertRaises(PermissionError):
                    vm.create_device(1920, 1080)
        self.assertNotIn('evdev', sys.modules)

    def test_invalid_daemon_dimensions_before_device_creation(self):
        with patch.object(vm, 'guard'), patch.object(vm, 'create_device') as factory:
            for width, height in [(0, 1080), (1920, 4097), (True, 1080)]:
                with self.assertRaises(ValueError):
                    vm.serve(width, height)
            factory.assert_not_called()

    def test_persistent_device_and_validation(self):
        ui = FakeUInput()
        errors = []
        ready = threading.Event()
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(vm, 'guard'), \
                patch.object(vm.time, 'sleep') as sleep, \
                patch.object(vm, 'create_device', return_value=(ui, Codes())) as factory:
            path = str(Path(directory) / 'control.sock')

            def run():
                try:
                    vm.serve(1920, 1080, path=path, ready=ready)
                except BaseException as error:
                    errors.append(error)
                    ready.set()

            thread = threading.Thread(target=run, daemon=True)
            thread.start()
            self.assertTrue(ready.wait(3))
            try:
                self.assertEqual(Path(path).stat().st_mode & 0o777, 0o600)
                for action, points in [('tap', [[500, 200]]), ('swipe', [[1, 2], [30, 40]])]:
                    vm.request(dict(action=action, points=points, width=1920, height=1080), path)
                    self.assertFalse(ui.closed)
                    self.assertIn(('EV_ABS', 'ABS_MT_TRACKING_ID', -1), ui.events)
                    self.assertEqual(ui.events[-2:], [('EV_KEY', 'BTN_TOUCH', 0), ('syn',)])
                for raw in [b'not-json\n', b'[]\n', b'x' * (vm.LIMIT + 1)]:
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                        connection.settimeout(3)
                        connection.connect(path)
                        connection.sendall(raw)
                        self.assertIn('error', vm.receive(connection))
                    self.assertFalse(ui.closed)
                for changes in [dict(points=[[1920, 0]]), dict(width=1280),
                                dict(points=[[0, 1080]]), dict(points=[[-1, 0]]),
                                dict(points=[[True, 0]]), dict(action='swipe')]:
                    data = dict(action='tap', points=[[1, 2]], width=1920, height=1080)
                    data.update(changes)
                    with self.assertRaisesRegex(ValueError, '.+'):
                        vm.request(data, path)
                    self.assertFalse(ui.closed)
                factory.assert_called_once_with(1920, 1080)
                self.assertEqual(sum(call.args == (1.5,) for call in sleep.call_args_list), 1)
            finally:
                if thread.is_alive():
                    vm.request({'action': 'stop'}, path)
                thread.join(3)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])
            self.assertTrue(ui.closed)
            self.assertEqual(ui.close_count, 1)
            self.assertFalse(Path(path).exists())

    def test_cli_contract(self):
        with patch.object(vm, 'guard'), patch.object(vm, 'serve') as serve, \
                patch.object(vm, 'request') as request:
            for args in [('serve', '--width', '1920', '--height', '1080'),
                         ('tap', '500,200', '--width', '1920', '--height', '1080'),
                         ('swipe', '1,2', '30,40'), ('stop',)]:
                with patch.object(sys, 'argv', ['vm-touch.py', *args]):
                    self.assertEqual(vm.main(), 0)
            serve.assert_called_once_with(1920, 1080)
            self.assertEqual(request.call_count, 3)
            self.assertEqual(request.call_args_list[0].args[0],
                             dict(action='tap', points=[(500, 200)], width=1920, height=1080))
            self.assertEqual(request.call_args.args[0]['action'], 'stop')

    def test_gesture_exception_releases_contact(self):
        ui = FakeUInput()
        with patch.object(vm.time, 'sleep', side_effect=RuntimeError('injected')):
            with self.assertRaisesRegex(RuntimeError, 'injected'):
                vm.gesture(ui, Codes(), 'tap', [[1, 2]])
        self.assertEqual(ui.events[-3:], [('EV_ABS', 'ABS_MT_TRACKING_ID', -1),
                                        ('EV_KEY', 'BTN_TOUCH', 0), ('syn',)])


if __name__ == '__main__':
    unittest.main()
