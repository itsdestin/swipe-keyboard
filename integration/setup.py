#!/usr/bin/env python3
"""Reversible per-user setup. Package transactions never touch KWin settings."""
import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout import build_overlay

REQUIRED_ASSETS = (
    'integration/swipekb-preflight',
    'qml/SwipeKeyboard/qmldir', 'qml/SwipeKeyboard/libswipekeyboardplugin.so',
    'qml/SwipeKeyboard/libswipekeyboard.so', 'qml/SwipeKeyboard/libswipekb_engine.so',
    'models/honorable_sturgeon/model_fp32.pte', 'models/honorable_sturgeon/metadata.json',
    'models/magic_macaw/model_fp32.pte', 'models/magic_macaw/metadata.json',
    'models/hungry_jellyfish/context_lm.pte', 'models/hungry_jellyfish/metadata.json',
    'models/hungry_jellyfish/vocab.txt', 'models/FUTO-MODEL-LICENSE.md',
    'dict/en_swipe.combined',
)
UNSET = '__SWIPEKB_UNSET__'


class SetupError(RuntimeError):
    pass


def atomic_json(path, value):
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(value, stream)
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    os.replace(temporary, path)


class Controller:
    def __init__(self, root, env=None, run=subprocess.run):
        self.root = Path(root).resolve()
        self.env = dict(os.environ if env is None else env)
        self.run = run
        home = Path(self.env['HOME'])
        self.data_home = Path(self.env.get('XDG_DATA_HOME') or home / '.local/share')
        self.data = self.data_home / 'swipe-keyboard'
        self.state_file = self.data / 'setup-state.json'
        self.layouts = self.data / 'overlay/plasma/keyboard/layouts'
        self.stock = Path(self.env.get('SWIPEKB_SYSTEM_LAYOUTS', '/usr/share/plasma/keyboard/layouts'))
        self.wanted = {'InputMethod': str(self.root.parent / 'applications/swipe-keyboard.desktop'),
                       'VirtualKeyboardMode': '1'}

    def command(self, args, **kwargs):
        try:
            return self.run(args, check=True, capture_output=True, text=True,
                            env=kwargs.get('env', self.env)).stdout.strip()
        except (OSError, subprocess.CalledProcessError) as error:
            raise SetupError(f'{args[0]} failed; no compatible setup could be confirmed. {error}') from error

    def preflight(self):
        # WHY a child QPluginLoader with eager binding: version strings cannot prove
        # KF/Qt/libstdc++ symbols exist. No instance(), QML engine or desktop is started.
        args = [str(self.root / 'integration/swipekb-preflight'),
                str(self.root / 'qml/SwipeKeyboard/libswipekeyboardplugin.so')]
        try:
            self.run(args, check=True, capture_output=True, text=True, env=self.env, timeout=15)
        except (OSError, subprocess.SubprocessError) as error:
            detail = getattr(error, 'stderr', None) or str(error)
            if isinstance(detail, bytes):
                detail = detail.decode(errors='replace')
            raise SetupError('Native plugin load failed; stock keyboard is unchanged. ' + detail.strip()) from error

    def check(self):
        if platform.machine() not in ('x86_64', 'AMD64'):
            raise SetupError('This package targets x86_64 computers only.')
        if self.env.get('XDG_SESSION_TYPE') != 'wayland' or 'KDE' not in self.env.get('XDG_CURRENT_DESKTOP', '').split(':'):
            raise SetupError('Use a KDE Plasma Wayland session. Other desktops/sessions are not supported.')
        # WHY: upstream skips its XDG layout search when this override is enabled, so our private
        # English overlay would never load even though the plugin itself is installed correctly.
        if self.env.get('PLASMA_KEYBOARD_USE_QT_LAYOUTS', '0') not in ('', '0'):
            raise SetupError('Custom Qt layout override is enabled. Disable that override before using this Plasma layout extension.')
        version = self.command(['plasma-keyboard', '--version'], env={**self.env, 'QT_QPA_PLATFORM': 'offscreen'})
        if not re.search(r'\b6\.(?:7|8)\.\d+\b', version):
            raise SetupError('This candidate targets Plasma Keyboard 6.7.x/6.8.x; installed version: ' + version)
        try:
            runtime = json.loads((self.root / 'runtime.json').read_text())
            build_qt = runtime['qt']
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise SetupError('Package compatibility metadata is missing or damaged. Reinstall the package.') from error
        qt = None
        # WHY explicit Arch path: qt6-base installs this tool under /usr/lib/qt6/bin,
        # which is not on a fresh desktop user's PATH. Fedora also supplies /usr/bin aliases.
        for binary in ('qtpaths6', 'qtpaths-qt6', '/usr/lib/qt6/bin/qtpaths6'):
            try:
                qt = self.command([binary, '--qt-version'])
                break
            except SetupError:
                continue
        # WHY one-way compatibility: public Qt permits an older-built plugin on a
        # newer same-major runtime, never a newer-built plugin on an older runtime.
        built = re.fullmatch(r'6\.(\d+)', build_qt) if isinstance(build_qt, str) else None
        installed = re.fullmatch(r'6\.(\d+)(?:\.\d+)?', qt or '')
        if (not built or int(built[1]) < 8 or not installed or int(installed[1]) < int(built[1])):
            raise SetupError(f'Package needs Qt {build_qt} or newer in the same major; installed Qt is {qt or "unavailable"}. Use a compatible package.')
        missing = [name for name in REQUIRED_ASSETS if not (self.root / name).is_file() or (self.root / name).stat().st_size == 0]
        if missing:
            raise SetupError('Package files are missing; reinstall the package: ' + ', '.join(missing))
        self.preflight()
        if not (self.stock / 'en_US').is_dir():
            raise SetupError('Stock Plasma Keyboard English layouts are missing.')
        if (self.data_home / 'plasma/keyboard/layouts').exists():
            raise SetupError('Existing custom keyboard layouts detected. Nothing was overwritten; use agent-assisted migration first.')
        # WHY fail before changing settings if the stock layout format has drifted upstream.
        from layout import patch_english
        english = self.stock / 'en_US/main.qml'
        if not english.is_file():
            fallback = self.stock / 'en_US/main.fallback'
            # Qt's empty .fallback marker selects the default fallback layout.
            if not fallback.is_file() or fallback.read_text().strip():
                raise SetupError('Unsupported English layout fallback; stock keyboard is unchanged.')
            english = self.stock / 'fallback/main.qml'
        try:
            patch_english(english.read_text())
        except (OSError, ValueError) as error:
            raise SetupError(str(error)) from error
        # WHY fixed labels rather than raw settings: support needs versions and selection,
        # not a user's home directory or the path of an unrelated input method.
        selected = self.read_setting('InputMethod') == self.wanted['InputMethod']
        try:
            distro = platform.freedesktop_os_release().get('PRETTY_NAME', 'Unknown Linux')
        except OSError:
            distro = 'Unknown Linux'
        return {'Distribution': distro,
                'Package': runtime.get('package', '0.1.0 (legacy metadata)'),
                'Plasma Keyboard': version,
                'Qt installed / build': f'{qt} / {build_qt}',
                'Session': 'KDE Plasma Wayland',
                'English (US) overlay': 'Compatible',
                'Selected input method': 'Swipe Keyboard' if selected else 'Other / stock',
                'Setup record': 'Present' if self.state_file.exists() else 'Absent'}

    def read_setting(self, key):
        return self.command(['kreadconfig6', '--file', 'kwinrc', '--group', 'Wayland', '--key', key, '--default', UNSET])

    def write_setting(self, key, value):
        args = ['kwriteconfig6', '--notify', '--file', 'kwinrc', '--group', 'Wayland', '--key', key]
        self.command(args + (['--delete'] if value == UNSET else [value]))

    @contextmanager
    def locked(self, wait=False):
        self.data.mkdir(parents=True, exist_ok=True, mode=0o700)
        with (self.data / '.setup.lock').open('a') as lock:
            # WHY: KWin may launch us during enable's notified settings write. Let that
            # transaction finish, but cap waiting in case its D-Bus refresh waits on us.
            deadline = time.monotonic() + 5 if wait else None
            while True:
                try:
                    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError as error:
                    remaining = deadline - time.monotonic() if wait else 0
                    if remaining <= 0:
                        raise SetupError('Another keyboard setup is running; try again after it finishes.') from error
                    time.sleep(min(0.05, remaining))
            yield

    def saved_state(self):
        try:
            state = json.loads(self.state_file.read_text())
            if set(state['previous']) != set(self.wanted) or state['wanted'] != self.wanted:
                raise ValueError('unexpected settings')
            if (not all(isinstance(v, str) for v in state['previous'].values()) or
                    type(state['live_mode']) is not int or state['live_mode'] not in (0, 1, 2)):
                raise ValueError('unexpected values')
            pending = state.get('mode_restore')
            if pending is not None and (not isinstance(pending, dict) or set(pending) != {'before', 'target'} or
                                        not all(isinstance(value, str) for value in pending.values())):
                raise ValueError('unexpected restoration journal')
            return state
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise SetupError('Setup record is damaged; refusing to guess your previous keyboard settings.') from error

    def refresh_kwin(self):
        # WHY both names: Arch installs qdbus6; Fedora's qt6-qttools installs qdbus-qt6.
        args = ['org.kde.KWin', '/KWin', 'org.kde.KWin.reconfigure']
        try:
            self.command(['qdbus6', *args])
        except SetupError as error:
            if not isinstance(error.__cause__, FileNotFoundError):
                raise
            self.command(['qdbus-qt6', *args])

    def read_live_mode(self):
        value = self.command(['busctl', '--user', 'get-property', 'org.kde.KWin', '/VirtualKeyboard',
                              'org.kde.kwin.VirtualKeyboard', 'mode'])
        if value not in ('i 0', 'i 1', 'i 2'):
            raise SetupError('KWin returned an unknown keyboard visibility mode; nothing was changed.')
        return int(value[2:])

    def set_live_mode(self, value):
        # WHY the typed D-Bus property: KWin 6.7 does not reload VirtualKeyboardMode from
        # kwinrc on reconfigure. Its setter applies live and also persists the value.
        self.command(['busctl', '--user', 'set-property', 'org.kde.KWin', '/VirtualKeyboard',
                      'org.kde.kwin.VirtualKeyboard', 'mode', 'i', str(value)])

    def restore_owned(self, state):
        mode_config = self.read_setting('VirtualKeyboardMode')
        live_mode = self.read_live_mode()
        config_owned = mode_config == self.wanted['VirtualKeyboardMode']
        pending = state.get('mode_restore')
        if pending is None and live_mode == 1:
            # WHY journal before the live setter: it persists its own value to disk. A failed
            # config restore must not make retry mistake that intermediate value for a user edit.
            pending = {'before': mode_config,
                       'target': state['previous']['VirtualKeyboardMode'] if config_owned else mode_config}
            state['mode_restore'] = pending
            atomic_json(self.state_file, state)
        if pending is not None:
            known_config = (pending['before'], str(state['live_mode']))
            if live_mode == 1:
                if mode_config not in known_config:
                    pending['target'] = mode_config
                    pending['before'] = mode_config
                    atomic_json(self.state_file, state)
                self.set_live_mode(state['live_mode'])
                mode_config = self.read_setting('VirtualKeyboardMode')
                live_mode = self.read_live_mode()
            if live_mode == state['live_mode'] and mode_config in (pending['before'], str(state['live_mode'])):
                self.write_setting('VirtualKeyboardMode', pending['target'])
        elif config_owned:
            self.write_setting('VirtualKeyboardMode', state['previous']['VirtualKeyboardMode'])
        if self.read_setting('InputMethod') == self.wanted['InputMethod']:
            self.write_setting('InputMethod', state['previous']['InputMethod'])

    def enable(self):
        self.check()
        with self.locked():
            if self.state_file.exists():
                self.saved_state()
                if self.read_setting('InputMethod') != self.wanted['InputMethod']:
                    raise SetupError('Keyboard selection changed since setup. Run disable before choosing to enable again.')
                self.rebuild_layout()
                return
            previous = {key: self.read_setting(key) for key in self.wanted}
            if previous['InputMethod'] == self.wanted['InputMethod']:
                raise SetupError('Swipe Keyboard is selected without a setup record. Select your old keyboard before enabling.')
            live_mode = self.read_live_mode()
            self.rebuild_layout()
            # WHY snapshot before writes: interrupted activation can be recovered with disable.
            state = {'previous': previous, 'wanted': self.wanted, 'live_mode': live_mode}
            atomic_json(self.state_file, state)
            try:
                self.write_setting('InputMethod', self.wanted['InputMethod'])
                self.set_live_mode(1)
                self.write_setting('VirtualKeyboardMode', '1')
                self.refresh_kwin()
            except SetupError:
                # Restore only state still owned by this activation; retain the record if rollback fails.
                self.restore_owned(state)
                self.refresh_kwin()
                self.state_file.unlink()
                raise

    def disable(self):
        # WHY no compatibility check: removal must work after an upstream update or package removal.
        if not self.state_file.exists():
            return
        with self.locked():
            state = self.saved_state()
            self.restore_owned(state)
            self.refresh_kwin()
            self.state_file.unlink()
            if self.layouts.exists():
                shutil.rmtree(self.layouts)
            # Personal recordings/options are deliberately retained, never silently deleted.

    def rebuild_layout(self):
        try:
            build_overlay(self.stock, self.layouts)
        except (OSError, ValueError) as error:
            raise SetupError(str(error)) from error

    def launch_environment(self):
        self.check()
        with self.locked(wait=True):
            self.rebuild_layout()
        env = dict(self.env)
        for key in ('QML_IMPORT_PATH', 'QML2_IMPORT_PATH'):
            env[key] = str(self.root / 'qml') + (':' + env[key] if env.get(key) else '')
        # WHY private XDG_DATA_DIRS: Plasma overrides QT_VIRTUALKEYBOARD_LAYOUT_PATH itself.
        # Keeping its GenericDataLocation lookup private avoids writing the user's global layout tree.
        env['XDG_DATA_DIRS'] = str(self.data / 'overlay') + ':' + (env.get('XDG_DATA_DIRS') or '/usr/local/share:/usr/share')
        env['SWIPEKB_MODEL_DATA'] = str(self.root)
        env['SWIPEKB_DATA'] = str(self.data)
        return env


def main():
    parser = argparse.ArgumentParser(description='Standalone English swipe typing — Powered by FUTO Swipe')
    parser.add_argument('action', choices=('check', 'enable', 'disable', 'launch'))
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('Run as your normal desktop user, not root or sudo.')
    controller = Controller(Path(__file__).resolve().parent.parent)
    try:
        if args.action == 'launch':
            try:
                env = controller.launch_environment()
            except (SetupError, OSError) as error:
                print(f'Swipe Keyboard: {error}\nStarting the unmodified stock keyboard instead.', file=sys.stderr)
                env = dict(os.environ)
            os.execvpe('plasma-keyboard', ['plasma-keyboard'], env)
        report = getattr(controller, args.action)()
        if args.action == 'check':
            for label, value in report.items():
                print(f'{label}: {value}')
        print({'check': 'Compatibility checks passed. Touch activation and typing still need a real KDE session test.',
               'enable': 'Swipe Keyboard enabled. Select English (US) and swipe a word in a supported text field.',
               'disable': 'Swipe Keyboard disabled; settings still owned by setup were restored.'}[args.action])
    except (SetupError, OSError) as error:
        print('Swipe Keyboard: ' + str(error), file=sys.stderr)
        print('Help: https://github.com/itsdestin/swipe-keyboard#troubleshooting\n'
              'Optional AI-agent assistance: https://youcoded.ai (not required).\n'
              'Share the error and versions, not typing recordings or private logs. '
              'Do not force dependencies or overwrite custom layouts.', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
