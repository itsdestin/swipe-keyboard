"""Private bundle launch only; selection/restoration belongs to KDE Settings."""
import fcntl
import os
from pathlib import Path
import socket
import sys

sys.path.insert(0, '/app/share/swipe-keyboard/integration')
from layout import build_overlay


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
    env = dict(os.environ)
    for key in ('QML_IMPORT_PATH', 'QML2_IMPORT_PATH'):
        env[key] = '/app/share/swipe-keyboard/qml:/app/lib/qml:/usr/lib/qml'
    env['QT_PLUGIN_PATH'] = '/app/lib/plugins:/usr/share/runtime/lib/plugins'
    env['XDG_DATA_DIRS'] = str(data / 'overlay') + ':/app/share:/usr/share:/usr/share/runtime/share'
    env['SWIPEKB_MODEL_DATA'] = '/app/share/swipe-keyboard'
    env['SWIPEKB_DATA'] = str(data)
    env['QT_QPA_PLATFORM'] = 'wayland'
    # WHY: no global environment edits and no WAYLAND_SOCKET unset/close; the
    # backend must consume KWin's privileged FD rather than connect to host input.
    os.execve('/app/bin/plasma-keyboard', ['plasma-keyboard'], env)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print('Swipe Keyboard: ' + str(error), file=sys.stderr)
        sys.exit(1)
