#!/bin/sh
set -eu
# WHY: native setup targets KWin 6.7's integer controller. This bundle never
# invokes it, writes host KConfig, or invents an inherited input-method socket.
if [ ! -f /.flatpak-info ] || [ "${FLATPAK_ID:-}" != org.itsdestin.SwipeKeyboard ]; then
    echo 'Swipe Keyboard: this launcher requires its Flatpak sandbox.' >&2
    exit 1
fi
case "${WAYLAND_SOCKET:-}" in
    ''|*[!0-9]*) echo 'Swipe Keyboard: select this input method in KDE Settings; an inherited WAYLAND_SOCKET is required.' >&2; exit 1 ;;
esac
exec python3 /app/share/swipe-keyboard/flatpak/launch.py "$@"
