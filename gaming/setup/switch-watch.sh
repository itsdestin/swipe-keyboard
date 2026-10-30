#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# Swipe Keyboard, Gaming Mode: watches Steam's developer-switch file. Installed by install.sh; removed by uninstall.sh.
#
#   switch-watch.sh check             (run by swipekb-switch-watch.timer) look, record what it saw, notify once if the switch is gone
#   switch-watch.sh restore [--yes]   show what it will do, ask, then put the switch file back (the "Turn it back on" button)
#   switch-watch.sh stop-helper       (run by swipekb-helper.service when it stops) ask the helper to take its layer out of Steam first
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
            started="$(process_start "$pid")"
            while IFS= read -r d; do
                # WHY not "-nt /proc/PID": that folder's time is when the kernel first listed it, which can be long after
                # Steam started, so a file made after Steam's start could look older and the reminder was skipped
                # (found on openSUSE, 2026-10-30). WHY -ge: in the same second we cannot tell; asking for a restart is safe.
                [ -e "$d/$MARKER_NAME" ] && [ -n "$started" ] \
                    && [ "$(date -r "$d/$MARKER_NAME" +%s 2>/dev/null || echo 0)" -ge "$started" ] && { echo restart; return; }
            done <<< "$dirs"
        fi
    fi
    echo ok
}

process_start() { # PID -> when it started, in seconds since 1970, from the kernel's own record (or nothing)
    local stat boot='' key value hz=100
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || return 0
    # WHY after the last ")": the program name in field 2 may itself contain spaces or brackets. What follows starts
    # at field 3, so the start time (field 22, in clock ticks since boot) is the 20th word.
    # shellcheck disable=SC2086
    set -- ${stat##*)}
    while read -r key value _; do [ "$key" = btime ] && boot="$value"; done < /proc/stat
    # WHY 100 when getconf is missing: Linux reports these times in 100ths of a second on every common build.
    command -v getconf >/dev/null 2>&1 && hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
    [ -n "${20:-}" ] && [ -n "$boot" ] && [ "${hz:-0}" -gt 0 ] 2>/dev/null && echo $(( boot + ${20} / hz ))
    return 0
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

# Review fix 5. systemd restarts our two background programs when they stop; if one cannot stay up, nothing else would tell
# the person (the helper writes the status file, and it is the one that is down). Only our own two units are looked at.
unit_substate() { systemctl --user show -p SubState --value "$1" 2>/dev/null | head -n 1; }

service_check() {
    command -v systemctl >/dev/null 2>&1 || return 0
    local unit sub bad=""
    for unit in swipekb-engine.service swipekb-helper.service; do
        sub="$(unit_substate "$unit")"
        case "$sub" in
            auto-restart) bad="$bad $unit" ;;
            failed)
                # WHY start it again: a unit that gave up stays down for good otherwise; this is once a minute, not a loop.
                bad="$bad $unit"
                systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
                systemctl --user start "$unit" >/dev/null 2>&1 || true ;;
        esac
    done
    case "$bad" in *swipekb-helper.service*) ;; *) return 0 ;; esac      # the helper is up and speaks for itself
    mkdir -p "$STATE_DIR"
    local reason="Gaming Mode's background program keeps stopping, so swipe is off and Steam's normal keyboard is in use. It is restarted automatically; if this stays, run the Swipe Keyboard installer again or restart the device."
    printf '{"state": "error", "reason": "%s", "steam_browser": null, "layer_version": null, "last_inject": null, "attached_pages": 0, "keyboard": null, "pid": null, "updated": %s}\n' \
        "$reason" "$(date +%s)" > "$STATE_DIR/status.json.new" && mv "$STATE_DIR/status.json.new" "$STATE_DIR/status.json"
}

# Review fix 8. Flatpak's --die-with-parent (needed so a stop really stops the app) turns the service's SIGTERM into an instant kill of
# the helper, so it never gets to take its layer out of Steam or say "off". Instead the service's stop runs this: it leaves a
# request file in the helper's state folder, the helper cleans up and deletes the file, and only then does the service go on to stop it.
do_stop_helper() {
    local ticks="${SWIPEKB_STOP_WAIT_TICKS:-100}" request="$STATE_DIR/stop-request"
    mkdir -p "$STATE_DIR"
    : > "$request"
    while [ -e "$request" ] && [ "$ticks" -gt 0 ]; do sleep 0.1; ticks=$((ticks - 1)); done
    rm -f "$request"      # nobody answered (the helper was not running): do not leave a request that would stop the next one
    return 0
}

do_check() {
    service_check
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
    stop-helper) do_stop_helper ;;
    -h|--help|"") sed -n '2,6p' "$SELF" ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
esac
