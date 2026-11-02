# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Private bundle launch only; selection/restoration belongs to KDE Settings."""
import fcntl
import os
from pathlib import Path
import re
import socket
import subprocess
import sys

sys.path.insert(0, '/app/share/swipe-keyboard/integration')
from layout import build_overlay


# Light or dark keyboard (card #502, Destin 2026-11-02: "follow light/dark").
# WHY this way: the keyboard takes its colours from KDE's colour scheme, but this app may not read the person's KDE settings
# file (it also lists recent files and folders; the manifest hides it on purpose). So we ask the desktop's standard settings
# service ONE question, "does the person prefer dark?", and give the keyboard a stock Breeze Dark scheme in the app's own
# private config folder. Nothing else is read; custom colour schemes and accent colours are not followed. A change of mode
# shows the next time KDE starts the keyboard (for example the next login).
OUR_COLORS = '# Written by Swipe Keyboard: the stock Breeze Dark colours, because the desktop prefers dark. Deleted when it does not.\n'
SCHEMES = Path('/usr/share/color-schemes')


def prefers_dark():
    """True when the desktop prefers dark, False when it prefers light, None when it does not say (or cannot be asked)."""
    for method in ('ReadOne', 'Read'):   # WHY both: older portals (before ReadOne existed) only have Read
        try:
            done = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.portal.Desktop',
                                   '--object-path', '/org/freedesktop/portal/desktop',
                                   '--method', 'org.freedesktop.portal.Settings.' + method,
                                   'org.freedesktop.appearance', 'color-scheme'],
                                  capture_output=True, text=True, timeout=2)
        except (OSError, subprocess.TimeoutExpired):
            return None
        found = re.search(r'uint32 (\d+)', done.stdout)
        if done.returncode == 0 and found:
            return {1: True, 2: False}.get(int(found.group(1)))   # 0 = no preference
    return None


def follow_dark_mode(config_home, dark):
    """Write (dark) or remove (otherwise) our private colour file. A kdeglobals we did not write is never touched."""
    target = Path(config_home) / 'kdeglobals'
    if target.exists() and not target.read_text(errors='replace').startswith(OUR_COLORS):
        return
    if dark:
        target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        new = target.with_name('kdeglobals.new')
        new.write_text(OUR_COLORS + (SCHEMES / 'BreezeDark.colors').read_text())
        new.replace(target)
    elif target.exists():
        target.unlink()


def main():
    if not Path('/.flatpak-info').is_file() or os.environ.get('FLATPAK_ID') != 'org.itsdestin.SwipeKeyboard':
        raise RuntimeError('This entry point requires its Flatpak sandbox.')
    if len(sys.argv) != 1:
        raise RuntimeError('No enable/disable or other native setup actions are supported; use KDE Settings.')
    # WHY: a numeric env var alone is not proof that KWin passed a live socket.
    # Duplicate it for inspection and leave the original descriptor untouched.
    fd = int(os.environ['WAYLAND_SOCKET'])
    with socket.socket(fileno=os.dup(fd)) as inherited:
        if inherited.family != socket.AF_UNIX or inherited.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) != socket.SOCK_STREAM:
            raise RuntimeError('WAYLAND_SOCKET must be an inherited Unix stream socket.')
    data = Path(os.environ['XDG_DATA_HOME'])
    if not data.is_absolute():
        raise RuntimeError('Flatpak app-scoped XDG_DATA_HOME must be absolute.')
    data = data / 'swipe-keyboard'
    data.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (data / '.layout.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        build_overlay('/app/share/plasma/keyboard/layouts', data / 'overlay/plasma/keyboard/layouts')
    config_home = os.environ.get('XDG_CONFIG_HOME')
    if config_home and Path(config_home).is_absolute():
        try:
            follow_dark_mode(config_home, prefers_dark())
        except OSError:
            pass   # WHY: colours are cosmetic; a keyboard must still start
    env = dict(os.environ)
    for key in ('QML_IMPORT_PATH', 'QML2_IMPORT_PATH'):
        env[key] = '/app/share/swipe-keyboard/qml:/app/lib/qml:/usr/lib/qml'
    env['QT_PLUGIN_PATH'] = '/app/lib/plugins:/usr/share/runtime/lib/plugins'
    env['XDG_DATA_DIRS'] = str(data / 'overlay') + ':/app/share:/usr/share:/usr/share/runtime/share'
    env['SWIPEKB_MODEL_DATA'] = '/app/share/swipe-keyboard'
    env['SWIPEKB_DATA'] = str(data)
    env['QT_QPA_PLATFORM'] = 'wayland'
    # WHY Qt's own software drawing (card #502, Debian 13 VM): this app has no graphics-card access (no device permission,
    # on purpose), so it always draws on the processor. By default it did so through Mesa's imitation graphics card
    # (llvmpipe), and where KDE itself runs without a graphics card (Debian 13's KWin 6.3 in a VM; also a computer with a
    # missing or broken driver) those pictures arrived blank: an empty keyboard, so no typing at all. Qt's plain software
    # drawing showed the same keys and swipe trail everywhere we looked. setdefault: a person can still choose otherwise.
    env.setdefault('QT_QUICK_BACKEND', 'software')
    # WHY: recognition needs every touch sample, not just the last position in a
    # rendered frame. Set before Qt starts; scoped to this keyboard process only.
    env['QML_NO_TOUCH_COMPRESSION'] = '1'
    # WHY: Qt marks a window hidden after one frame slower than 100 ms, and Plasma Keyboard
    # then drops completions and corrections. A slow frame must not lose an edit.
    env.setdefault('QT_WAYLAND_FRAME_CALLBACK_TIMEOUT', '5000')
    # WHY: the session's QT_WAYLAND_RECONNECT=1 made a deselected keyboard reconnect as an
    # ordinary app and stay running invisibly (~440 MB on SteamOS). KWin relaunches it itself.
    env.pop('QT_WAYLAND_RECONNECT', None)
    # WHY: no global environment edits and no WAYLAND_SOCKET unset/close; the
    # backend must consume KWin's privileged FD rather than connect to host input.
    os.execve('/app/bin/plasma-keyboard', ['plasma-keyboard'], env)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print('Swipe Keyboard: ' + str(error), file=sys.stderr)
        sys.exit(1)
