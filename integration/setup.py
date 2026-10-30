#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

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
import learning
import plasma69

REQUIRED_ASSETS = (
    'integration/swipekb-preflight', 'integration/swipekb-config',
    'qml/SwipeKeyboard/qmldir', 'qml/SwipeKeyboard/libswipekeyboardplugin.so',
    'qml/SwipeKeyboard/libswipekeyboard.so', 'qml/SwipeKeyboard/libswipekb_engine.so',
    # WHY only the dictionary among the data files: English swipes from it alone; the swipe model and the next-word
    # files are optional (geometry-only swiping, no suggestions) and are verified by the plugin itself.
    'dict/en_swipe.combined',
)
# WHY a separate list: Plasma 6.9's front end (frontend-plasma69, QML module SwipeKeyboard69) uses none of FUTO's
# models and no Qt Virtual Keyboard; it must start even after the FUTO files are gone from the package.
REQUIRED_ASSETS_69 = (
    'integration/swipekb-preflight',
    'qml/SwipeKeyboard69/qmldir', 'qml/SwipeKeyboard69/libswipekeyboard69plugin.so',
    'qml/SwipeKeyboard69/libswipekeyboard69.so', 'dict/en_swipe.combined',
)
# KDE's rebuilt keyboard core entered plasma-keyboard in the 6.9 development series (6.8.80, beta 6.8.90).
NEW_CORE_VERSION = (6, 8, 80)
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
        self.packages = self.data / 'overlay' / plasma69.PACKAGE_DIR
        self.stock = Path(self.env.get('SWIPEKB_SYSTEM_LAYOUTS', '/usr/share/plasma/keyboard/layouts'))
        self.front_end = None   # 'legacy' (Qt Virtual Keyboard, Plasma 6.7/6.8) or 'plasma69'; set by check()
        self.wanted = {'InputMethod': str(self.root.parent / 'applications/swipe-keyboard.desktop'),
                       'VirtualKeyboardMode': '1'}

    def command(self, args, **kwargs):
        try:
            return self.run(args, check=True, capture_output=True, text=True,
                            env=kwargs.get('env', self.env)).stdout.strip()
        except (OSError, subprocess.CalledProcessError) as error:
            raise SetupError(f'{args[0]} failed; no compatible setup could be confirmed. {error}') from error

    def preflight(self, plugin='qml/SwipeKeyboard/libswipekeyboardplugin.so'):
        # WHY a child QPluginLoader with eager binding: version strings cannot prove
        # KF/Qt/libstdc++ symbols exist. No instance(), QML engine or desktop is started.
        args = [str(self.root / 'integration/swipekb-preflight'), str(self.root / plugin)]
        try:
            self.run(args, check=True, capture_output=True, text=True, env=self.env, timeout=15)
        except (OSError, subprocess.SubprocessError) as error:
            detail = getattr(error, 'stderr', None) or str(error)
            if isinstance(detail, bytes):
                detail = detail.decode(errors='replace')
            raise SetupError('Native plugin load failed; stock keyboard is unchanged. ' + detail.strip()) from error

    def profile_checks(self):
        """Fixed offline queries, not claims about the selected locale or running engine."""
        utility = self.root / 'integration/swipekb-profilecheck'
        if not utility.is_file():
            return {'status': 'utility-missing', 'scope': 'queried-installation-capability',
                    'engineReady': 'unknown', 'activeFieldPolicy': 'unknown'}
        results = []
        for locale, layout in (('en-US', 'qwerty'), ('en-GB', 'qwerty'), ('es-ES', 'qwerty'),
                               ('fr-FR', 'azerty'), ('de-DE', 'qwertz')):
            try:
                process = self.run([str(utility), '--model-root', str(self.root),
                                    '--locale', locale, '--layout', layout, '--mode', 'Latin'],
                                   check=False, capture_output=True, text=True, env=self.env, timeout=30)
                value = json.loads(process.stdout)
                if (process.returncode not in (0, 1) or not isinstance(value, dict) or
                        value.get('scope') != 'queried-installation-capability' or
                        value.get('engineReady') != 'unknown' or value.get('activeFieldPolicy') != 'unknown'):
                    raise ValueError('invalid utility result')
                results.append(value)
            except (OSError, subprocess.SubprocessError, ValueError):
                # WHY no child stderr/path text in shareable diagnostics, and no success inferred from absence.
                return {'status': 'utility-failed', 'scope': 'queried-installation-capability',
                        'engineReady': 'unknown', 'activeFieldPolicy': 'unknown'}
        return {'status': 'queried', 'profiles': results}

    def system_data_dirs(self):
        """The data folders KDE's keyboard reads, without our own overlay (SWIPEKB_SYSTEM_DATA_DIRS in tests)."""
        raw = self.env.get('SWIPEKB_SYSTEM_DATA_DIRS') or self.env.get('XDG_DATA_DIRS') or '/usr/local/share:/usr/share'
        overlay = str(self.data / 'overlay')
        return [d for d in raw.split(':') if d and d != overlay]

    def plasma_keyboard_version(self):
        text = self.command(['plasma-keyboard', '--version'], env={**self.env, 'QT_QPA_PLATFORM': 'offscreen'})
        found = re.search(r'\b(\d+)\.(\d+)\.(\d+)\b', text)
        return text, (tuple(int(part) for part in found.groups()) if found else None)

    def detect_front_end(self):
        """Which of our two front ends matches the installed keyboard. Raises SetupError for anything else.

        WHY both the version AND the files must agree: the version says which engine the plasma-keyboard binary runs,
        the files say what it will load. A disagreement (half-finished upgrade, distro patch) or an unknown version
        starts KDE's plain keyboard: typing always works, swiping waits for a matching Swipe Keyboard.
        """
        text, found = self.plasma_keyboard_version()
        new_core = plasma69.is_new_core(self.system_data_dirs())
        legacy = (self.stock / 'en_US').is_dir()
        if found and found >= (7, 0, 0):
            # WHY: a new major version means a new Qt major; our Qt 6 modules could not load there.
            raise SetupError(f'Plasma Keyboard {text.split()[-1]} is newer than this Swipe Keyboard supports (6.7 to 6.x); '
                             'it steps aside and the plain KDE keyboard is used.')
        if found and found >= NEW_CORE_VERSION:
            if not new_core:
                raise SetupError('Plasma Keyboard ' + text + ' has no keyboard packages Swipe Keyboard can extend; '
                                 'the plain KDE keyboard is used.')
            return 'plasma69', text
        if found and found >= (6, 7, 0):
            if not legacy:
                raise SetupError('Stock Plasma Keyboard English layouts are missing.')
            return 'legacy', text
        if not found and new_core != legacy:
            return ('plasma69' if new_core else 'legacy'), text
        raise SetupError('This candidate targets Plasma Keyboard 6.7 or newer (6.x); installed version: ' + text)

    def qt_check(self):
        try:
            runtime = json.loads((self.root / 'runtime.json').read_text())
            build_qt = runtime['qt']
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise SetupError('Package compatibility metadata is missing or damaged. Reinstall the package.') from error
        qt = None
        # WHY our own probe first: it is installed with the package and reports the Qt it really loads; qtpaths6 is a
        # developer tool on openSUSE (card #502). The qtpaths names stay as a fallback. WHY explicit Arch path: qt6-base
        # installs qtpaths6 under /usr/lib/qt6/bin, which is not on a fresh desktop user's PATH.
        for binary in (str(self.root / 'integration/swipekb-preflight'), 'qtpaths6', 'qtpaths-qt6', '/usr/lib/qt6/bin/qtpaths6'):
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
        return runtime, qt, build_qt

    def check(self):
        # WHY ARM is explicit: the native Fedora aarch64 package has its own
        # required build/load acceptance; other architectures still fail closed.
        if platform.machine() not in ('x86_64', 'AMD64', 'aarch64'):
            raise SetupError('This package targets x86_64 or aarch64 Linux only.')
        if self.env.get('XDG_SESSION_TYPE') != 'wayland' or 'KDE' not in self.env.get('XDG_CURRENT_DESKTOP', '').split(':'):
            raise SetupError('Use a KDE Plasma Wayland session. Other desktops/sessions are not supported.')
        self.front_end, version = self.detect_front_end()
        if self.front_end == 'plasma69':
            return self.check_plasma69(version)
        return self.check_legacy(version)

    def check_plasma69(self, version):
        """Plasma 6.9's rebuilt keyboard: our module loads, and KDE's packages can be extended. Writes nothing."""
        runtime, qt, build_qt = self.qt_check()
        missing = [name for name in REQUIRED_ASSETS_69 if not (self.root / name).is_file() or (self.root / name).stat().st_size == 0]
        if missing:
            raise SetupError('Package files are missing; reinstall the package: ' + ', '.join(missing))
        self.preflight('qml/SwipeKeyboard69/libswipekeyboard69plugin.so')
        # WHY a throwaway build: proves KDE's package format is one we understand before any setting changes.
        with tempfile.TemporaryDirectory(prefix='swipe-keyboard-check-') as temp:
            try:
                generated = plasma69.build_packages(self.system_data_dirs(), temp)
            except (OSError, ValueError) as error:
                raise SetupError(str(error)) from error
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
                'Front end': 'Plasma 6.9 keyboard core (no Qt Virtual Keyboard, no FUTO)',
                'Keyboard packages': f'Compatible ({len(generated)} languages)',
                'Selected input method': 'Swipe Keyboard' if selected else 'Other / stock',
                'Setup record': 'Present' if self.state_file.exists() else 'Absent'}

    def check_legacy(self, version):
        # WHY: upstream skips its XDG layout search when this override is enabled, so our private
        # language overlays would never load even though the plugin itself is installed correctly.
        if self.env.get('PLASMA_KEYBOARD_USE_QT_LAYOUTS', '0') not in ('', '0'):
            raise SetupError('Custom Qt layout override is enabled. Disable that override before using this Plasma layout extension.')
        runtime, qt, build_qt = self.qt_check()
        missing = [name for name in REQUIRED_ASSETS if not (self.root / name).is_file() or (self.root / name).stat().st_size == 0]
        if missing:
            raise SetupError('Package files are missing; reinstall the package: ' + ', '.join(missing))
        self.preflight()
        if (self.data_home / 'plasma/keyboard/layouts').exists():
            raise SetupError('Existing custom keyboard layouts detected. Nothing was overwritten; use agent-assisted migration first.')
        # WHY fail before changing settings if the stock layout format has drifted upstream.
        from layout import prepare_overrides
        try:
            prepare_overrides(self.stock)
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
                'Front end': 'Plasma 6.7/6.8 (Qt Virtual Keyboard)',
                'Language overlays': 'Compatible',
                'Profile capability queries (not active runtime)': self.profile_checks(),
                'Selected input method': 'Swipe Keyboard' if selected else 'Other / stock',
                'Setup record': 'Present' if self.state_file.exists() else 'Absent'}

    def config_state(self, writable=False, keys=None):
        # WHY native KConfig: INI parsing misses Kiosk file/group/key locks and session
        # overlays. Null is raw absence, never a magic string that a user could select.
        if (not Path(self.env['HOME']).is_absolute() or
                (self.env.get('XDG_CONFIG_HOME') and not Path(self.env['XDG_CONFIG_HOME']).is_absolute())):
            raise SetupError('An absolute HOME/XDG_CONFIG_HOME is required to establish KConfig ownership.')
        try:
            result = json.loads(self.command([str(self.root / 'integration/swipekb-config')]))
            if set(result) != set(self.wanted):
                raise ValueError('unexpected keys')
            for entry in result.values():
                if (set(entry) != {'raw', 'effective', 'immutable'} or
                        type(entry['immutable']) is not bool or
                        any(value is not None and not isinstance(value, str)
                            for value in (entry['raw'], entry['effective']))):
                    raise ValueError('unexpected entry')
        except SetupError as error:
            # WHY surface the read-only helper's refusal: unsupported raw syntax is
            # not a successful absence probe or an unexplained compatibility failure.
            cause = error.__cause__
            if isinstance(cause, subprocess.CalledProcessError) and cause.returncode == 2:
                raise SetupError('Native KConfig ownership check refused: ' + (cause.stderr or '').strip()) from error
            raise
        except (ValueError, TypeError, AttributeError) as error:
            raise SetupError('Native KConfig ownership check is unavailable or invalid; refusing to guess ownership.') from error
        if writable and any(result[key]['immutable'] for key in (self.wanted if keys is None else keys)):
            raise SetupError('KConfig keyboard settings are immutable; refusing settings and live-control mutations. '
                             'The setup record is retained if present.')
        return result

    def read_setting(self, key):
        return self.config_state()[key]['effective']

    def write_setting(self, key, value):
        self.config_state(writable=True, keys=(key,))
        # WHY absolute user file: a relative write can omit an explicit entry equal
        # to an inherited default, losing the raw ownership needed on later disable.
        config_home = Path(self.env.get('XDG_CONFIG_HOME') or Path(self.env['HOME']) / '.config')
        args = ['kwriteconfig6', '--notify', '--file', str(config_home / 'kwinrc'), '--group', 'Wayland', '--key', key]
        self.command(args + (['--delete'] if value is None else [value]))
        if self.config_state()[key]['raw'] != value:
            raise SetupError('KConfig write could not be confirmed; setup record retained for recovery.')

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
            schema = state.get('schema', 1)
            valid_value = lambda value: isinstance(value, str) or (schema in (2, 3) and value is None)
            if schema == 3 and (state.get('visibility_policy') not in ('preserve', 'always') or
                                (state['visibility_policy'] == 'preserve' and 'mode_restore' in state)):
                raise ValueError('unexpected visibility ownership')
            if (type(schema) is not int or schema not in (1, 2, 3) or
                    not all(valid_value(v) for v in state['previous'].values()) or
                    type(state['live_mode']) is not int or state['live_mode'] not in (0, 1, 2)):
                raise ValueError('unexpected values')
            pending = state.get('mode_restore')
            if pending is not None and (not isinstance(pending, dict) or set(pending) != {'before', 'target'} or
                                        not all(valid_value(value) for value in pending.values())):
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
        # WHY logical KConfig notification: KConfig::sync suppresses notifications for
        # absolute names, but KWin watches relative kwinrc's Wayland/InputMethod.
        # Keep raw absolute writes and generic reconfigure; emit KConfig's own
        # a{saay} protocol only after verified writes/restoration. Failure must reach
        # the transaction so rollback/retry retains the setup record when necessary.
        key = list(b'InputMethod')
        self.command(['busctl', '--user', 'emit', '/kwinrc', 'org.kde.kconfig.notify',
                      'ConfigChanged', 'a{saay}', '1', 'Wayland', '1', str(len(key)),
                      *map(str, key)])

    def read_live_mode(self):
        value = self.command(['busctl', '--user', 'get-property', 'org.kde.KWin', '/VirtualKeyboard',
                              'org.kde.kwin.VirtualKeyboard', 'mode'])
        if value not in ('i 0', 'i 1', 'i 2'):
            raise SetupError('KWin returned an unknown keyboard visibility mode; nothing was changed.')
        return int(value[2:])

    def set_live_mode(self, value):
        # WHY the typed D-Bus property: KWin 6.7 does not reload VirtualKeyboardMode from
        # kwinrc on reconfigure. Its setter applies live and also persists the value.
        self.config_state(writable=True)
        self.command(['busctl', '--user', 'set-property', 'org.kde.KWin', '/VirtualKeyboard',
                      'org.kde.kwin.VirtualKeyboard', 'mode', 'i', str(value)])

    def restore_owned(self, state):
        # WHY: preserving policy means never owning its raw key, live mode or a
        # tablet manager's baseline—even if the current mode happens to equal 1.
        if state.get('schema') == 3 and state.get('visibility_policy') == 'preserve':
            config = self.config_state(writable=True, keys=('InputMethod',))
            if config['InputMethod']['raw'] == self.wanted['InputMethod']:
                self.write_setting('InputMethod', state['previous']['InputMethod'])
            return
        config = self.config_state(writable=True)
        if state.get('schema', 1) == 1:
            # WHY fail closed for legacy values: r3 saved effective strings, which
            # cannot prove whether a previous key existed in the user's file. Retain
            # r3's UNSET convention, but never infer absence from another old value.
            ambiguous = [key for key in self.wanted
                         if config[key]['raw'] == self.wanted[key] and state['previous'][key] != UNSET]
            pending = state.get('mode_restore')
            if ambiguous or (pending and pending['target'] != UNSET):
                raise SetupError('Legacy setup record has no raw setting provenance. Previous values may be inherited; '
                                 'refusing automatic restoration. Record retained; choose your keyboard settings explicitly.')
            state['previous'] = {key: None if value == UNSET else config[key]['raw']
                                 for key, value in state['previous'].items()}
            if pending:
                state['mode_restore'] = {key: None if value == UNSET else value for key, value in pending.items()}
            state['schema'] = 2
            atomic_json(self.state_file, state)
        mode_config = config['VirtualKeyboardMode']['raw']
        live_mode = self.read_live_mode()
        config_owned = mode_config == self.wanted['VirtualKeyboardMode']
        pending = state.get('mode_restore')
        if pending is None and live_mode == 1 and config_owned:
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
                mode_config = self.config_state()['VirtualKeyboardMode']['raw']
                live_mode = self.read_live_mode()
            if live_mode == state['live_mode'] and mode_config in (pending['before'], str(state['live_mode'])):
                self.write_setting('VirtualKeyboardMode', pending['target'])
        elif config_owned:
            self.write_setting('VirtualKeyboardMode', state['previous']['VirtualKeyboardMode'])
        if self.config_state()['InputMethod']['raw'] == self.wanted['InputMethod']:
            self.write_setting('InputMethod', state['previous']['InputMethod'])

    def enable(self, always_available=False):
        self.check()
        with self.locked():
            owned_keys = tuple(self.wanted) if always_available else ('InputMethod',)
            config = self.config_state(writable=True, keys=owned_keys)
            if self.state_file.exists():
                saved = self.saved_state()
                if always_available and (saved.get('schema') != 3 or saved.get('visibility_policy') != 'always'):
                    raise SetupError('Already enabled with another visibility policy. Disable first, then explicitly choose --always-available.')
                if (config['InputMethod']['raw'] != self.wanted['InputMethod'] or
                        config['InputMethod']['effective'] != self.wanted['InputMethod']):
                    raise SetupError('Keyboard selection changed since setup. Run disable before choosing to enable again.')
                self.rebuild_layout()
                return
            previous = {key: config[key]['raw'] for key in self.wanted}
            if config['InputMethod']['effective'] == self.wanted['InputMethod']:
                raise SetupError('Swipe Keyboard is selected without a setup record. Select your old keyboard before enabling.')
            live_mode = self.read_live_mode()
            self.rebuild_layout()
            # WHY snapshot before writes: interrupted activation can be recovered with disable.
            state = {'schema': 3, 'previous': previous, 'wanted': self.wanted, 'live_mode': live_mode,
                     'visibility_policy': 'always' if always_available else 'preserve'}
            atomic_json(self.state_file, state)
            try:
                self.write_setting('InputMethod', self.wanted['InputMethod'])
                if always_available:
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
            for generated in (self.layouts, self.packages):
                if generated.exists():
                    shutil.rmtree(generated)
            # Personal recordings/options are deliberately retained, never silently deleted.

    def rebuild_layout(self):
        try:
            if self.front_end == 'plasma69':
                return plasma69.build_packages(self.system_data_dirs(), self.data / 'overlay')
            build_overlay(self.stock, self.layouts)
            return []
        except (OSError, ValueError) as error:
            raise SetupError(str(error)) from error

    def launch_environment(self):
        self.check()
        with self.locked(wait=True):
            generated = self.rebuild_layout()
        env = dict(self.env)
        for key in ('QML_IMPORT_PATH', 'QML2_IMPORT_PATH'):
            env[key] = str(self.root / 'qml') + (':' + env[key] if env.get(key) else '')
        # WHY private XDG_DATA_DIRS: Plasma overrides QT_VIRTUALKEYBOARD_LAYOUT_PATH itself.
        # Keeping its GenericDataLocation lookup private avoids writing the user's global layout tree.
        env['XDG_DATA_DIRS'] = str(self.data / 'overlay') + ':' + (env.get('XDG_DATA_DIRS') or '/usr/local/share:/usr/share')
        env['SWIPEKB_MODEL_DATA'] = str(self.root)
        env['SWIPEKB_DATA'] = str(self.data)
        # WHY: Qt Quick batches touch updates by rendered frame. On slow rendering
        # this dropped the middle of actual ARM swipes before recognition. Preserve
        # the trajectory from process startup; no desktop policy/global env changes.
        env['QML_NO_TOUCH_COMPRESSION'] = '1'
        # WHY: Qt marks a window hidden when one frame takes over 100 ms, and Plasma Keyboard
        # then silently drops completions, corrections and text updates. Measured on ARM
        # Plasma Mobile: "Helo" -> tap "Hello" did nothing until this was raised.
        env.setdefault('QT_WAYLAND_FRAME_CALLBACK_TIMEOUT', '5000')
        # WHY: Plasma's session QT_WAYLAND_RECONNECT=1 made a deselected keyboard reconnect as an
        # ordinary app and keep running invisibly with its models loaded. KWin relaunches it itself.
        env.pop('QT_WAYLAND_RECONNECT', None)
        if self.front_end == 'plasma69':
            # WHY: tells our module which packages it generated, so languages it did not wrap stay exactly KDE's.
            env['SWIPEKB_PLASMA69_PACKAGES'] = ','.join(generated)
            return env
        # WHY this distinct mode follows the checked stock Plasma 6.7/6.8 launch.
        # It is NOT raw application completion permission: the plugin also checks
        # the actual proxy class and gives any present raw bridge precedence.
        env['SWIPEKB_NATIVE_PREDICTION'] = 'owned-history-v1'
        return env


def confirm_activation(controller, assume_yes=False, always_available=False):
    # WHY: package installation and desktop activation are different consent boundaries.
    # Read current values before asking; declining must not create an overlay/journal or write settings.
    state = controller.config_state()
    print('Swipe Keyboard will change these per-user KDE settings:')
    print(f"  InputMethod: {state['InputMethod']['effective']!r} -> {controller.wanted['InputMethod']!r}")
    if always_available:
        print(f"  VirtualKeyboardMode: {state['VirtualKeyboardMode']['effective']!r} -> '1' (explicit always-available choice)")
        print('This opt-in changes visibility policy and can allow pop-ups while a physical keyboard is attached.')
    else:
        print(f"  VirtualKeyboardMode: {state['VirtualKeyboardMode']['effective']!r} (preserved)")
        print('Your attached-keyboard and tablet/desktop manager remains in control of keyboard visibility.')
    print(
          'You may notice different swipe suggestions and whole-word Backspace behavior.\n'
          'The native package uses your existing system backend; it does not replace Qt, Plasma or Maliit.\n'
          'With verified language packs, current-word completions and next-word suggestions are available for English, Spanish, French and German.\n'
          'On stock Plasma, next-word prediction uses only words this keyboard confirmed in the current field; the backend does not expose every raw application completion preference. Sensitive and restricted fields remain blocked.\n'
          'Run swipe-keyboard disable before removal to restore settings still owned by setup; '
          'legacy records without raw provenance may need an explicit manual choice.')
    if assume_yes:
        return True
    if not sys.stdin.isatty():
        print('No settings changed. Use an interactive terminal or pass --yes for explicit automation consent.')
        return False
    try:
        answer = input('Enable Swipe Keyboard with these changes? [y/N] ').strip().lower()
    except (EOFError, KeyboardInterrupt):
        answer = ''
    if answer not in ('y', 'yes'):
        print('No settings changed.')
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description='Standalone swipe typing')
    parser.add_argument('action', choices=('check', 'enable', 'disable', 'launch', 'learning'))
    parser.add_argument('learning_action', nargs='?', choices=('on', 'off', 'forget', 'status'),
                        help='with `learning`: turn learning from your typing on or off, erase it, or show its status')
    parser.add_argument('--yes', action='store_true', help='Explicitly accept the displayed activation changes without prompting')
    parser.add_argument('--always-available', action='store_true',
                        help='Explicitly set KDE visibility mode 1; default preserves your tablet/attached-keyboard policy')
    args = parser.parse_args()
    if args.always_available and args.action != 'enable':
        parser.error('--always-available applies only to enable')
    if (args.action == 'learning') != (args.learning_action is not None):
        parser.error('use: swipe-keyboard learning on|off|forget|status')
    if os.geteuid() == 0:
        parser.error('Run as your normal desktop user, not root or sudo.')
    controller = Controller(Path(__file__).resolve().parent.parent)
    try:
        if args.action == 'learning':
            # WHY the controller's data folder: it is the folder `launch` hands the keyboard as SWIPEKB_DATA.
            return learning.run(args.learning_action, controller.data)
        if args.action == 'launch':
            try:
                env = controller.launch_environment()
            except (SetupError, OSError) as error:
                print(f'Swipe Keyboard: {error}\nStarting the unmodified stock keyboard instead.', file=sys.stderr)
                env = dict(os.environ)
            os.execvpe('plasma-keyboard', ['plasma-keyboard'], env)
        if args.action == 'enable':
            controller.check()  # Refuse incompatible systems before offering activation.
            if not confirm_activation(controller, assume_yes=args.yes, always_available=args.always_available):
                return 0
            report = controller.enable(always_available=args.always_available)
        else:
            report = getattr(controller, args.action)()
        if args.action == 'check':
            for label, value in report.items():
                print(f'{label}: {value}')
        print({'check': 'Compatibility checks passed. Touch activation and typing still need a real KDE session test.',
               'enable': 'Swipe Keyboard enabled. Choose English (US/UK), Spanish, French or German in the keyboard language menu.',
               'disable': 'Swipe Keyboard disabled; settings still owned by setup were restored.'}[args.action])
    except (SetupError, OSError) as error:
        print('Swipe Keyboard: ' + str(error), file=sys.stderr)
        print('Help: https://github.com/itsdestin/swipe-keyboard#troubleshooting\n'
              'Share the error and versions, not typing recordings or private logs. '
              'Do not force dependencies or overwrite custom layouts.', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
