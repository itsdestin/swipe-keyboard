# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Real logical KConfigWatcher regression, exclusively on a generated private bus."""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def private_round_trip(helper, watcher):
    from test_layout import load_module
    module = load_module('setup')
    env = dict(os.environ)  # supplied by the clean parent environment below
    assert env['DBUS_SESSION_BUS_ADDRESS']
    assert 'DISPLAY' not in env and 'WAYLAND_DISPLAY' not in env
    config = Path(env['XDG_CONFIG_HOME']) / 'kwinrc'
    defaults = Path(env['XDG_CONFIG_DIRS']) / 'kwinrc'
    defaults.write_text('[Wayland]\nInputMethod=inherited.desktop\nVirtualKeyboardMode=2\n')

    def runner(args, **kwargs):
        if args[0] in ('qdbus6', 'qdbus-qt6'):
            # WHY stand in only for generic reconfigure: there is deliberately no KWin
            # service on this bus. This old refresh cannot notify KConfigWatcher.
            return subprocess.CompletedProcess(args, 0, '', '')
        if Path(args[0]).name == 'swipekb-config':
            args = [helper]
        return subprocess.run(args, **kwargs)

    controller = module.Controller(ROOT, env, runner)
    process = subprocess.Popen([watcher], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        def line(timeout=3):
            if not select.select([process.stdout], [], [], timeout)[0]:
                raise AssertionError('No Wayland/InputMethod KConfigWatcher event after verified absolute write + refresh')
            return process.stdout.readline().strip()
        assert line() == 'ready'
        # Control: absolute --notify alone must not reach the logical subscription.
        controller.write_setting('InputMethod', 'control.desktop')
        assert not select.select([process.stdout], [], [], 0.3)[0], 'unexpected absolute-path notification'
        for value, effective in (('swipe.desktop', 'swipe.desktop'), (None, 'inherited.desktop')):
            controller.write_setting('InputMethod', value)
            controller.refresh_kwin()
            assert line() == 'Wayland/InputMethod=' + effective
            state = controller.config_state()['InputMethod']
            assert state['raw'] == value and state['effective'] == effective, state
            if value is None:
                assert 'InputMethod=' not in config.read_text()
            print(json.dumps({'event': 'Wayland/InputMethod', **state}), flush=True)
    finally:
        process.terminate()  # only this test's private fixture, never a desktop process
        process.communicate(timeout=5)


class LiveSwitch(unittest.TestCase):
    def test_absolute_write_refresh_notifies_real_logical_watcher(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            # WHY whitelist: neither host bus/display nor DBUS_* activation variables
            # may leak into the fresh session. dbus-run-session supplies its address.
            env = {'PATH': os.environ['PATH'], 'HOME': str(base / 'home'),
                   'XDG_CONFIG_HOME': str(base / 'config'),
                   'XDG_CONFIG_DIRS': str(base / 'defaults'),
                   'XDG_DATA_HOME': str(base / 'data'),
                   'XDG_DATA_DIRS': str(base / 'system-data'),
                   'XDG_RUNTIME_DIR': str(base / 'runtime'), 'QT_QPA_PLATFORM': 'offscreen'}
            for key, value in env.items():
                if key.startswith('XDG_') or key == 'HOME':
                    Path(value).mkdir(mode=0o700)
            for source, name in ((ROOT / 'integration', 'helper'),
                                 (ROOT / 'tests/integration/watcher', 'watcher')):
                build = base / name
                subprocess.run(['cmake', '-S', str(source), '-B', str(build)],
                               env=env, check=True, capture_output=True)
                subprocess.run(['cmake', '--build', str(build), '--parallel', '2'],
                               env=env, check=True, capture_output=True)
            result = subprocess.run(['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()),
                                     '--private', str(base / 'helper/swipekb-config'),
                                     str(base / 'watcher/selection-watcher')],
                                    env=env, text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end='')


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--private':
        private_round_trip(*sys.argv[2:])
    else:
        unittest.main()
