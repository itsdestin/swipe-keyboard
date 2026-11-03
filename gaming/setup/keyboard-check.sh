#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# OpenSwipe: never leave the computer without an on-screen keyboard. Installed by install.sh; removed by uninstall.sh.
#
#   keyboard-check.sh   (run by swipekb-keyboard-check.path whenever Flatpak's list of apps changes, and once at login)
#
# WHY this exists (card #502, edge case 1): KDE remembers the chosen on-screen keyboard by its app file. If OpenSwipe
# is removed some other way than uninstall.sh (Discover, "flatpak uninstall"), that choice would point at nothing and the
# person would be left with no on-screen keyboard at all, which on a tablet means no typing. Flatpak has no "app removed"
# hook, so this small script, which lives outside the app, looks instead. It changes something only when the chosen keyboard
# IS OpenSwipe and the app is really gone: then it hands the choice back to KDE's own keyboard (KWin switches at once),
# says so in a notification, and switches off our own background jobs, which have nothing left to do.
set -u
MAIN_ID=org.itsdestin.SwipeKeyboard
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
# Where the system's own on-screen keyboards are listed (test hook; on a real system never set).
APPLICATIONS_DIR="${SWIPEKB_APPLICATIONS_DIR:-/usr/share/applications}"
# WHY a list (review, card #502): Debian 13 has no KDE keyboard, but may have Maliit; "none" only when neither is there.
FALLBACKS=("$APPLICATIONS_DIR/org.kde.plasma.keyboard.desktop" "$APPLICATIONS_DIR/com.github.maliit.keyboard.desktop")
# WHY all of ours (review): with the app gone, the Gaming Mode engine would restart every two minutes for nothing, and the
# helper would keep its layer in Steam with no engine behind it.
OUR_JOBS=(swipekb-update.timer swipekb-keyboard-check.path swipekb-engine.service swipekb-helper.service
          swipekb-switch-watch.timer swipekb-switch-watch.service)

# WHY ask Flatpak and not look for the file: during an update the app's files are swapped, but Flatpak still reports it.
if command -v flatpak >/dev/null 2>&1; then
    flatpak info --user "$MAIN_ID" >/dev/null 2>&1 && exit 0
    flatpak info --system "$MAIN_ID" >/dev/null 2>&1 && exit 0
fi

# The app is gone. The keyboard KWin is told to start (System Settings > Keyboard > Virtual Keyboard), as in kwinrc.
choice=""
# WHY the optional [$e] (Debian 13 VM, card #502): System Settings saves the choice as InputMethod[$e]=$HOME/..., a marker
# telling KDE to expand $HOME; reading only "InputMethod=" missed it, so the choice looked like someone else's.
[ -f "$CONFIG_HOME/kwinrc" ] && choice="$(sed -n '/^\[Wayland\]/,/^\[/{s/^InputMethod\(\[\$e\]\)\{0,1\}=//p}' "$CONFIG_HOME/kwinrc" | head -n 1)"
case "$choice" in
    "$MAIN_ID.desktop"|*/"$MAIN_ID.desktop") ours=1 ;;
    *) ours=0 ;;   # another keyboard (or none) is chosen: not ours to touch
esac
if [ "$ours" = 1 ] && command -v kwriteconfig6 >/dev/null 2>&1; then
    target=""
    for candidate in "${FALLBACKS[@]}"; do [ -f "$candidate" ] && { target="$candidate"; break; }; done
    if kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify "$target"; then
        if [ -n "$target" ]; then
            msg="Your system's own on-screen keyboard is back. Change it any time in System Settings > Keyboard > Virtual Keyboard."
        else
            msg="No on-screen keyboard is chosen now. Pick one in System Settings > Keyboard > Virtual Keyboard."
        fi
        if command -v notify-send >/dev/null 2>&1; then
            notify-send --app-name="OpenSwipe" --icon=input-keyboard "OpenSwipe was removed" "$msg" >/dev/null 2>&1 || true
        fi
    fi
fi
# WHY switch off and not delete: deleting our files is uninstall.sh's job (it knows what was ours); stopping is enough so
# nothing keeps running for an app that is gone. Only units that exist are touched; missing ones are simply skipped.
if command -v systemctl >/dev/null 2>&1; then
    for unit in "${OUR_JOBS[@]}"; do systemctl --user disable --now "$unit" >/dev/null 2>&1; done
fi
exit 0
