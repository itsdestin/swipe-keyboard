#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# Swipe Keyboard: never leave the computer without an on-screen keyboard. Installed by install.sh; removed by uninstall.sh.
#
#   keyboard-check.sh   (run by swipekb-keyboard-check.path whenever Flatpak's list of apps changes, and once at login)
#
# WHY this exists (card #502, edge case 1): KDE remembers the chosen on-screen keyboard by its app file. If Swipe Keyboard
# is removed some other way than uninstall.sh (Discover, "flatpak uninstall"), that choice would point at nothing and the
# person would be left with no on-screen keyboard at all, which on a tablet means no typing. Flatpak has no "app removed"
# hook, so this small script, which lives outside the app, looks instead. It changes something only when the chosen keyboard
# IS Swipe Keyboard and the app is really gone: then it hands the choice back to KDE's own keyboard (KWin switches at once),
# says so in a notification, and switches off our own background jobs, which have nothing left to do.
set -u
MAIN_ID=org.itsdestin.SwipeKeyboard
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
# Where KDE's own on-screen keyboard is listed (test hook; on a real system never set).
APPLICATIONS_DIR="${SWIPEKB_APPLICATIONS_DIR:-/usr/share/applications}"
KDE_KEYBOARD="$APPLICATIONS_DIR/org.kde.plasma.keyboard.desktop"
OUR_JOBS=(swipekb-update.timer swipekb-keyboard-check.path)

# The keyboard KWin is told to start (System Settings > Keyboard > Virtual Keyboard), as written in kwinrc. Reads only.
choice=""
# WHY the optional [$e] (Debian 13 VM, card #502): System Settings saves the choice as InputMethod[$e]=$HOME/..., a marker
# telling KDE to expand $HOME; reading only "InputMethod=" missed it, so the choice looked like someone else's.
[ -f "$CONFIG_HOME/kwinrc" ] && choice="$(sed -n '/^\[Wayland\]/,/^\[/{s/^InputMethod\(\[\$e\]\)\{0,1\}=//p}' "$CONFIG_HOME/kwinrc" | head -n 1)"
case "$choice" in
    "$MAIN_ID.desktop"|*/"$MAIN_ID.desktop") ;;
    *) exit 0 ;;   # another keyboard (or none) is chosen: not ours to touch
esac
# WHY ask Flatpak and not look for the file: during an update the app's files are swapped, but Flatpak still reports it.
if command -v flatpak >/dev/null 2>&1; then
    flatpak info --user "$MAIN_ID" >/dev/null 2>&1 && exit 0
    flatpak info --system "$MAIN_ID" >/dev/null 2>&1 && exit 0
fi
command -v kwriteconfig6 >/dev/null 2>&1 || exit 0

if [ -f "$KDE_KEYBOARD" ]; then
    kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify "$KDE_KEYBOARD" || exit 0
    msg="KDE's own on-screen keyboard is back. Change it any time in System Settings > Keyboard > Virtual Keyboard."
else
    kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify "" || exit 0
    msg="No on-screen keyboard is chosen now. Pick one in System Settings > Keyboard > Virtual Keyboard."
fi
if command -v notify-send >/dev/null 2>&1; then
    notify-send --app-name="Swipe Keyboard" --icon=input-keyboard "Swipe Keyboard was removed" "$msg" >/dev/null 2>&1 || true
fi
# WHY switch off and not delete: deleting our files is uninstall.sh's job (it knows what was ours); stopping is enough so
# nothing keeps running for an app that is gone. The daily update job would otherwise try to update nothing, every day.
command -v systemctl >/dev/null 2>&1 && systemctl --user disable --now "${OUR_JOBS[@]}" >/dev/null 2>&1
exit 0
