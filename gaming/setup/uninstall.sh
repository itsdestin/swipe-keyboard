#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# Swipe Keyboard, Steam Deck Gaming Mode: uninstaller. Removes only what install.sh recorded as created.
#
#   bash uninstall.sh              show what will be removed, ask, then do it
#   bash uninstall.sh --dry-run    show only
#   bash uninstall.sh --yes        do not ask
#   bash uninstall.sh --remove-apps   also remove the two Flatpak apps, if install.sh was the one that added them
set -euo pipefail

CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"
UNIT_DIR="$CONFIG_HOME/systemd/user"
RECORD_DIR="$DATA_HOME/swipe-keyboard-gaming"
STATE_DIR="$STATE_HOME/swipekb-gaming"
RECORD="$RECORD_DIR/install-record.json"
# Same test hook as install.sh; on a real Deck these are never set.
SOCKET_UNIT=steam-web-debug-portforward.socket
PROXY_UNIT=steam-web-debug-portforward.service   # the forwarding service that socket starts on demand
SYSTEM_UNIT_DIR="${SWIPEKB_SYSTEM_UNIT_DIR:-/etc/systemd/system}"
DROPIN_DIR="$SYSTEM_UNIT_DIR/$SOCKET_UNIT.d"
DROPIN="$DROPIN_DIR/swipekb-loopback-only.conf"
UNITS=(swipekb-engine.service swipekb-helper.service swipekb-switch-watch.service swipekb-switch-watch.timer)
MAIN_ID=org.itsdestin.SwipeKeyboard
MARKER_NAME=.cef-enable-remote-debugging
HELPER_ID=org.itsdestin.SwipeKeyboard.GamingHelper

DRY_RUN=0; ASSUME_YES=0; REMOVE_APPS=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --yes) ASSUME_YES=1 ;;
        --remove-apps) REMOVE_APPS=1 ;;
        -h|--help) sed -n '2,7p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "Unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done

# Same fixed-layout reader as install.sh (kept in both so each script can be read on its own).
record_list() {
    [ -f "$RECORD" ] || return 0
    sed -n "/^  \"$1\": \[/,/^  \]/p" "$RECORD" | grep '^    "' | sed 's/^    "//; s/",\{0,1\}$//' || true
}

CREATED_MARKERS=(); CREATED_SYSTEM_FILES=(); INSTALLED_APPS=()
while IFS= read -r line; do [ -n "$line" ] && CREATED_MARKERS+=("$line"); done < <(record_list created_markers)
while IFS= read -r line; do [ -n "$line" ] && CREATED_SYSTEM_FILES+=("$line"); done < <(record_list created_system_files)
while IFS= read -r line; do [ -n "$line" ] && INSTALLED_APPS+=("$line"); done < <(record_list installed_flatpaks)

# WHY the stamp: a unit file is only ours to delete if install.sh wrote it (first line), never a file of yours that
# happens to share the name.
ours() { [ -f "$UNIT_DIR/$1" ] && [ "$(head -n 1 "$UNIT_DIR/$1")" = '# swipekb-gaming-managed' ]; }

# Review fix 4. Decky Loader (a popular community tool) uses the same developer-switch file. If it was installed after us, deleting
# the file here would quietly break it, so the file is kept and the person is told.
decky_present() { [ -e "$HOME/homebrew/services/PluginLoader" ] || [ -e "$SYSTEM_UNIT_DIR/plugin_loader.service" ]; }
# A recorded path is deleted only if it really is the switch file inside one of Steam's own folders: the record is a plain text file,
# and a wrong or edited line must never lead to deleting something else.
marker_is_switch_file() {
    case "$1" in */"$MARKER_NAME") ;; *) return 1 ;; esac
    local real candidate
    real="$(readlink -f "$(dirname "$1")" 2>/dev/null)" || return 1
    for candidate in "$HOME/.steam/steam" "$HOME/.local/share/Steam"; do
        [ -d "$candidate" ] && [ "$(readlink -f "$candidate")" = "$real" ] && return 0
    done
    return 1
}

echo "Swipe Keyboard for Gaming Mode: removal. This will:"
echo "  - stop and turn off the background services (the engine, the helper and the once-a-minute switch check), and delete their files from $UNIT_DIR (only files this installer wrote)"
if [ -f "$RECORD" ]; then
    for marker in "${CREATED_MARKERS[@]+"${CREATED_MARKERS[@]}"}"; do
        if ! marker_is_switch_file "$marker"; then
            echo "  - leave $marker alone: it is not Steam's developer-switch file in Steam's folder"
        elif decky_present; then
            echo "  - keep the developer-switch file $marker: Decky Loader appears to be installed and relies on it"
        else
            echo "  - delete the developer-switch file $marker (this installer created it)"
        fi
    done
    echo "  - leave any developer-switch file that existed before this install exactly as it is"
else
    echo "  - leave Steam's developer-switch file alone: there is no install record, so it is not known whether this installer made it"
fi
for file in "${CREATED_SYSTEM_FILES[@]+"${CREATED_SYSTEM_FILES[@]}"}"; do
    echo "  - remove the outside-access block $file (this installer made it; asks for your administrator password once, so other devices on your network can reach Steam's developer switch again if it is on)"
done
echo "  - delete $RECORD_DIR (the install record) and $STATE_DIR (status the helper wrote)"
if [ "$REMOVE_APPS" = 1 ]; then
    for id in "${INSTALLED_APPS[@]+"${INSTALLED_APPS[@]}"}"; do echo "  - remove the app $id (it was added by this installer)"; done
else
    echo "  - leave the two apps installed; to remove them yourself:"
    echo "      flatpak uninstall --user $HELPER_ID"
    echo "      flatpak uninstall --user $MAIN_ID"
fi
echo "Steam keeps nothing of ours on disk: the keyboard layer lives only in Steam's memory and is gone after Steam's next restart."

if [ "$DRY_RUN" = 1 ]; then echo; echo "Dry run: nothing was changed."; exit 0; fi
if [ "$ASSUME_YES" = 0 ]; then
    printf '\nContinue? [y/N] '
    answer=""; read -r answer || answer=""
    case "$answer" in y|Y|yes|YES|Yes) ;; *) echo "Nothing was changed."; exit 1 ;; esac
fi

HAD_RECORD=0; [ -f "$RECORD" ] && HAD_RECORD=1

# WHY only units that carry our stamp (review fix 4): a file of yours with the same name is not ours to switch off either.
# disable can complain if a unit is already gone; that is fine, we are removing it anyway.
OURS=(); for unit in "${UNITS[@]}"; do if ours "$unit"; then OURS+=("$unit"); fi; done
if [ "${#OURS[@]}" -gt 0 ]; then systemctl --user disable --now "${OURS[@]}" || true; fi
for unit in "${UNITS[@]}"; do
    if ours "$unit"; then
        # ${VAR:?} makes the shell stop instead of deleting a wrong path if a name were ever empty.
        rm -f "${UNIT_DIR:?}/${unit:?}"
    elif [ -e "$UNIT_DIR/$unit" ]; then
        echo "Left $UNIT_DIR/$unit alone: it was not written by this installer."
    fi
done
systemctl --user daemon-reload || true

for marker in "${CREATED_MARKERS[@]+"${CREATED_MARKERS[@]}"}"; do
    if ! marker_is_switch_file "$marker"; then
        echo "Left $marker alone: it is not Steam's developer-switch file in Steam's folder."
    elif decky_present; then
        echo "Kept $marker: Decky Loader appears to be installed and relies on this developer-switch file. Remove it yourself if you no longer use Decky Loader."
    # WHY a regular file only: never follow a link or remove a folder because a path was edited by hand.
    elif [ -f "$marker" ] && [ ! -L "$marker" ]; then rm -f "${marker:?}"; fi
done

# WHY only a recorded, stamped file: never remove a system file this installer did not make.
for file in "${CREATED_SYSTEM_FILES[@]+"${CREATED_SYSTEM_FILES[@]}"}"; do
    if [ "$file" = "$DROPIN" ] && [ -f "$file" ] && [ "$(head -n 1 "$file")" = '# swipekb-gaming-managed' ]; then
        if sudo -v; then
            sudo rm -f "$file"
            sudo rmdir "$DROPIN_DIR" 2>/dev/null || true
            sudo systemctl daemon-reload || true
            # WHY stop then start: a plain restart fails while SteamOS's forwarding service is still running (install.sh explains)
            { sudo systemctl stop "$SOCKET_UNIT" "$PROXY_UNIT" && sudo systemctl start "$SOCKET_UNIT"; } || true
        else
            echo "Could not get administrator rights, so $file was left in place. It only keeps other devices out; delete it any time." >&2
        fi
    fi
done

if [ "$REMOVE_APPS" = 1 ]; then
    for id in "${INSTALLED_APPS[@]+"${INSTALLED_APPS[@]}"}"; do flatpak uninstall --user -y "$id" || true; done
fi
rm -rf "${STATE_DIR:?}" "${RECORD_DIR:?}"

if [ "$HAD_RECORD" = 0 ]; then
    echo "Note: no install record was found, so Steam's developer-switch file (if any) was not touched."
fi
echo "Removed. The Steam developer switch change takes effect at Steam's next restart; until then Steam may still have its debug port open."
