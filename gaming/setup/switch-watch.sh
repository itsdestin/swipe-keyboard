#!/usr/bin/env bash
# Swipe Keyboard, Gaming Mode: watches Steam's developer-switch file. Installed by install.sh; removed by uninstall.sh.
#
#   switch-watch.sh check             (run by swipekb-switch-watch.timer) look, record what it saw, notify once if the switch is gone
#   switch-watch.sh restore [--yes]   show what it will do, ask, then put the switch file back (the "Turn it back on" button)
#
# WHY this exists: the keyboard helper runs in a Flatpak sandbox that must not see Steam's folder (it holds login
# files), so it cannot tell that the developer switch file is gone. This small program runs on the Deck itself.
# It ONLY LOOKS: `check` never creates, changes or deletes anything in Steam's folder. It writes one word into
# Swipe Keyboard's own state folder, which the helper shows as its status:
#   ok          the switch file is there
#   off         the switch file is gone (the person turned it off, or Steam or another tool removed it)
#   restart     the file is there but Steam started before it was made, so Steam has not read it yet
#   no-steam    no Steam folder was found for this user
# Only `restore`, started by a person who clicked "Turn it back on" or typed the command, ever creates the file.
set -u

DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/swipekb-gaming"
SELF="$(readlink -f "${BASH_SOURCE[0]}")"
MARKER_NAME=.cef-enable-remote-debugging
SWITCH_FILE="$STATE_DIR/switch-state"
NOTIFIED_FILE="$STATE_DIR/switch-notified"

# Where Steam keeps its folder; ~/.steam/steam is normally a link to ~/.local/share/Steam, so resolve and de-duplicate.
steam_dirs() {
    local candidate real seen=""
    for candidate in "$HOME/.steam/steam" "$HOME/.local/share/Steam"; do
        [ -d "$candidate" ] || continue
        real="$(readlink -f "$candidate")"
        case " $seen " in *" $real "*) continue ;; esac
        seen="$seen $real"
        echo "$real"
    done
}

# The word that describes the switch right now. Reads only.
current_word() {
    local dirs d have_any=0 present=0 pid
    dirs="$(steam_dirs)"
    [ -n "$dirs" ] || { echo no-steam; return; }
    while IFS= read -r d; do
        have_any=1
        [ -e "$d/$MARKER_NAME" ] && present=1
    done <<< "$dirs"
    [ "$have_any" = 1 ] || { echo no-steam; return; }
    [ "$present" = 1 ] || { echo off; return; }
    # Steam reads the file only when it starts. If Steam has been running since before the file was made, it never saw it.
    if command -v pgrep >/dev/null 2>&1; then
        pid="$(pgrep -o -x steam 2>/dev/null | head -n 1)"
        if [ -n "$pid" ] && [ -e "/proc/$pid" ]; then
            while IFS= read -r d; do
                [ -e "$d/$MARKER_NAME" ] && [ "$d/$MARKER_NAME" -nt "/proc/$pid" ] && { echo restart; return; }
            done <<< "$dirs"
        fi
    fi
    echo ok
}

write_word() { # only touches our own state folder, and only when the word changed
    mkdir -p "$STATE_DIR"
    [ "$(cat "$SWITCH_FILE" 2>/dev/null)" = "$1" ] && return 0
    printf '%s\n' "$1" > "$SWITCH_FILE.new" && mv "$SWITCH_FILE.new" "$SWITCH_FILE"
}

TITLE="Swipe Keyboard: Gaming Mode swipe is off"
BODY="Steam's developer switch was turned off. Swipe in Gaming Mode is off; Steam's normal keyboard is in use."

# Is a DESKTOP ready to show a notification? In Gaming Mode there is none: the name is owned by Steam's own
# steam_notif_daemon, which shows nothing of ours (found in the SteamOS VM). Counting it would use up our one
# notification where nobody can see it, so only another owner counts; we simply try again next minute.
notification_server_present() {
    command -v gdbus >/dev/null 2>&1 || return 1
    local reply owner pid comm
    gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner org.freedesktop.Notifications 2>/dev/null | grep -q true || return 1
    owner="$(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.GetNameOwner org.freedesktop.Notifications 2>/dev/null | sed -n "s/^('\(.*\)',)\$/\1/p")"
    [ -n "$owner" ] || return 1
    pid="$(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.GetConnectionUnixProcessID "$owner" 2>/dev/null | sed -n 's/^(uint32 \([0-9]*\),)$/\1/p')"
    [ -n "$pid" ] || return 1
    comm="$(cat "/proc/$pid/comm" 2>/dev/null)"
    case "$comm" in steam_notif*) return 1 ;; esac
    return 0
}

notify_once() {
    [ -e "$NOTIFIED_FILE" ] && return 0                       # already told them for this occurrence
    command -v notify-send >/dev/null 2>&1 || return 0
    notification_server_present || return 0                    # try again next minute; do NOT mark as told
    : > "$NOTIFIED_FILE"
    # WHY a separate transient unit: waiting for the button press can take hours and must not block the next check.
    if command -v systemd-run >/dev/null 2>&1; then
        systemd-run --user --collect --quiet --unit=swipekb-switch-notify -- "$SELF" notify-wait || true
    else
        "$SELF" notify-wait &
    fi
}

launch_restore_window() {
    if command -v konsole >/dev/null 2>&1; then
        konsole -e "$SELF" restore
    else
        notify-send --app-name="Swipe Keyboard" "To turn it back on" "Open Konsole and run: bash $SELF restore" || true
    fi
}

notify_wait() {
    if notify-send --help 2>&1 | grep -q -- '--action'; then
        local choice
        choice="$(notify-send --app-name="Swipe Keyboard" --icon=input-keyboard --expire-time=0 \
            --action=restore="Turn it back on" --action=dismiss="Dismiss" "$TITLE" "$BODY" 2>/dev/null)" || true
        [ "$choice" = restore ] && launch_restore_window
    else
        # An older notification tool cannot show buttons: say how to fix it in words instead.
        notify-send --app-name="Swipe Keyboard" --icon=input-keyboard --expire-time=0 "$TITLE" \
            "$BODY To turn it back on, run the Swipe Keyboard installer again, or open Konsole and run: bash $SELF restore" || true
    fi
}

do_check() {
    local word; word="$(current_word)"
    write_word "$word"
    if [ "$word" = off ]; then
        notify_once
    else
        rm -f "$NOTIFIED_FILE"      # fixed: if it goes off again later, that is a new occurrence and we tell them again
    fi
}

do_restore() {
    local assume_yes=0 d missing=() answer
    [ "${1:-}" = --yes ] && assume_yes=1
    while IFS= read -r d; do
        [ -n "$d" ] && [ ! -e "$d/$MARKER_NAME" ] && missing+=("$d/$MARKER_NAME")
    done <<< "$(steam_dirs)"
    if [ "${#missing[@]}" = 0 ]; then
        echo "Steam's developer switch file is already there. Nothing to do."
        write_word "$(current_word)"; rm -f "$NOTIFIED_FILE"
        [ -t 0 ] && [ "$assume_yes" = 0 ] && { printf 'Press Enter to close. '; read -r _ || true; }
        return 0
    fi
    echo "Turn Swipe Keyboard's Gaming Mode back on."
    echo "This will create one empty file (Steam's developer switch) and change nothing else:"
    for d in "${missing[@]}"; do echo "  $d"; done
    echo "Steam reads it only when it starts, so restart Steam once afterwards (if a game is running, finish it first)."
    if [ "$assume_yes" = 0 ]; then
        printf '\nContinue? [y/N] '
        answer=""; read -r answer || answer=""
        case "$answer" in y|Y|yes|YES|Yes) ;; *) echo "Nothing was changed."; return 1 ;; esac
    fi
    for d in "${missing[@]}"; do : > "$d"; done
    rm -f "$NOTIFIED_FILE"
    write_word "$(current_word)"
    echo "Done. Restart Steam once so it notices the switch."
    [ -t 0 ] && [ "$assume_yes" = 0 ] && { printf 'Press Enter to close. '; read -r _ || true; }
    return 0
}

case "${1:-}" in
    check) do_check ;;
    restore) shift; do_restore "$@" ;;
    notify-wait) notify_wait ;;
    -h|--help|"") sed -n '2,6p' "$SELF" ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
esac
