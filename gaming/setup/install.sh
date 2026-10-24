#!/usr/bin/env bash
# Swipe Keyboard for Steam Deck: installer. Runs as you, in Desktop Mode, in a terminal.
# Gaming Mode part asks for the Deck's administrator password ONCE, to close SteamOS's outside door (see below).
# Plan: docs/active/plans/2026-10-22-gaming-mode.md, Sections 5 and 9.
#
#   bash install.sh                  show the choice, ask, show what will change, ask again, then do it
#   bash install.sh --desktop-only   Desktop Mode keyboard only (turns Gaming Mode off if it was on)
#   bash install.sh --gaming         Desktop Mode keyboard AND Gaming Mode swipe
#   bash install.sh --block-outside  with --gaming: close SteamOS's door to other devices on your network
#                                    (asks for the administrator password once). This is the default.
#   bash install.sh --leave-open     with --gaming: leave that door open on purpose (no password; anyone on your
#                                    network can then control Steam). Rerun with --block-outside to close it.
#   bash install.sh --dry-run        show what would change, change nothing, ask nothing
#   bash install.sh --yes            do not ask (for scripted use). Without --gaming or --desktop-only this keeps
#                                    whatever you already have: a first install gets the Desktop keyboard only.
#   bash install.sh --skip-flatpaks  do not install the apps (they are already installed)
#   bash install.sh --force          allow installing an OLDER version over a newer one
#   bash install.sh --no-auto-update don't install updates by themselves (they are on by default; update by hand
#                                    by running a newer download's install.sh). --auto-update turns them back on.
#
# Safe to run again: a second run changes nothing that is already right, an upgrade is simply running the newer
# download's install.sh, and running it again is also how you turn Gaming Mode on later or off. Everything is
# undone by uninstall.sh (in the same download).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN_ID=org.itsdestin.SwipeKeyboard
HELPER_ID=org.itsdestin.SwipeKeyboard.GamingHelper
MARKER_NAME=.cef-enable-remote-debugging
UNITS=(swipekb-engine.service swipekb-helper.service swipekb-switch-watch.service swipekb-switch-watch.timer)
WATCH_NAME=switch-watch.sh   # looks whether Steam's developer-switch file is still there and tells you if not
# Automatic updates (plan: docs/active/plans/2026-10-23-auto-updates.md): a daily timer runs auto-update.sh, which asks
# Flatpak to update our apps from the signed update site. Separate from UNITS: it is on for the Desktop keyboard too.
UPDATE_UNITS=(swipekb-update.service swipekb-update.timer)
UPDATE_NAME=auto-update.sh
UPDATE_SITE="${SWIPEKB_UPDATE_REPO:-https://itsdestin.github.io/swipe-keyboard/repo/}"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
UNIT_DIR="$CONFIG_HOME/systemd/user"
RECORD_DIR="$DATA_HOME/swipe-keyboard-gaming"
RECORD="$RECORD_DIR/install-record.json"
# The helper reads its network choice from this one-word file in its own state folder (the helper's app cannot see the
# record). Missing file = "blocked", the strict reading.
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/swipekb-gaming"
OUTSIDE_FILE="$STATE_DIR/outside-access"
WATCH_SCRIPT="$RECORD_DIR/$WATCH_NAME"
UPDATE_SCRIPT="$RECORD_DIR/$UPDATE_NAME"
# The outside door: SteamOS's own system socket that forwards Steam's debug port (8080, this Deck only) to port
# 8081 on EVERY network interface. The two variables below exist so the tests can point at a throwaway folder; on a
# real Deck they are never set.
SOCKET_UNIT=steam-web-debug-portforward.socket
PROXY_UNIT=steam-web-debug-portforward.service   # the forwarding service that socket starts on demand
SYSTEM_UNIT_DIR="${SWIPEKB_SYSTEM_UNIT_DIR:-/etc/systemd/system}"
PROC_NET="${SWIPEKB_PROC_NET:-/proc/net}"
DROPIN_DIR="$SYSTEM_UNIT_DIR/$SOCKET_UNIT.d"
DROPIN="$DROPIN_DIR/swipekb-loopback-only.conf"

DRY_RUN=0; ASSUME_YES=0; SKIP_FLATPAKS=0; FORCE=0; CHOICE=""; OUTSIDE=""; AUTO=""
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --yes) ASSUME_YES=1 ;;
        --skip-flatpaks) SKIP_FLATPAKS=1 ;;
        --force) FORCE=1 ;;
        --gaming) CHOICE=gaming ;;
        --desktop-only) CHOICE=desktop ;;
        --block-outside) OUTSIDE=blocked ;;
        --leave-open) OUTSIDE=open ;;
        --auto-update) AUTO=yes ;;
        --no-auto-update) AUTO=no ;;
        -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "Unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done
CHOICE_FROM_FLAG=0; [ -n "$CHOICE" ] && CHOICE_FROM_FLAG=1

die() { echo "Swipe Keyboard setup: $*" >&2; exit 1; }

# --- Record helpers --------------------------------------------------------------------------------
# WHY a record: uninstall must remove only what THIS install created. A developer-switch file that already
# existed (Decky Loader users have one) is not ours to delete. The file is written in a fixed layout, one
# value per line, so these small functions can read it back without any JSON tool.
record_list() { # print the items of one list in the previous record (nothing if there is no record)
    [ -f "$RECORD" ] || return 0
    sed -n "/^  \"$1\": \[/,/^  \]/p" "$RECORD" | grep '^    "' | sed 's/^    "//; s/",\{0,1\}$//' || true
}
record_value() { # print one plain value from the previous record
    [ -f "$RECORD" ] || return 0
    sed -n "s/^  \"$1\": \"\(.*\)\",\{0,1\}$/\1/p" "$RECORD" | head -n 1
}
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

in_list() { # in_list WORD ITEM...
    local word="$1" item; shift
    for item in "$@"; do [ "$item" = "$word" ] && return 0; done
    return 1
}
json_list() { # print one list in the record layout
    local key="$1" item first=1; shift
    echo "  \"$key\": ["
    for item in "$@"; do
        [ "$first" = 1 ] || echo ","
        first=0
        printf '    "%s"' "$item"
    done
    [ "$first" = 1 ] || echo
    echo "  ],"
}
write_record() {
    mkdir -p "$RECORD_DIR"
    {
        echo '{'
        echo '  "schema": "1",'
        echo "  \"version\": \"$VERSION\","
        echo "  \"installed_at\": \"$INSTALLED_AT\","
        echo "  \"updated_at\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
        # WHY this value: a later run reads it to know whether Gaming Mode is on, so the choice screen can offer the
        # other option and "just press Enter" never changes what you have.
        echo "  \"gaming\": \"$GAMING_STATE\","
        # WHY recorded: it is your choice about other devices on your network, and re-running changes it on purpose only.
        echo "  \"outside_access\": \"$OUTSIDE_STATE\","
        # WHY recorded: a later run keeps your automatic-update choice unless you pass --auto-update/--no-auto-update.
        echo "  \"auto_update\": \"$AUTO_STATE\","
        json_list created_markers "${CREATED_MARKERS[@]+"${CREATED_MARKERS[@]}"}"
        json_list existing_markers "${EXISTING_MARKERS[@]+"${EXISTING_MARKERS[@]}"}"
        json_list created_units "${CREATED_UNITS[@]+"${CREATED_UNITS[@]}"}"
        json_list created_system_files "${CREATED_SYSTEM_FILES[@]+"${CREATED_SYSTEM_FILES[@]}"}"
        json_list installed_flatpaks "${INSTALLED_APPS[@]+"${INSTALLED_APPS[@]}"}"
        echo '  "note": "Written by install.sh. uninstall.sh removes only what is listed as created here."'
        echo '}'
    } > "$RECORD.new"
    mv "$RECORD.new" "$RECORD"
}

# --- The outside door --------------------------------------------------------------------------------
# WHY these live in the script and not a download: they are short, and the person reading this file should see
# exactly what is run with administrator rights. Only five commands ever do (mkdir, tee, rm, rmdir, systemctl).
outside_doors() { # print each watched port (8080, 8081) that is listening on something other than this device only
    # /proc/net/tcp and tcp6 list listening sockets with addresses in hex, least significant byte first, so
    # 127.0.0.1 is 0100007F (ends 7F) and the IPv6 loopback is 00000000000000000000000001000000. gaming/helper/
    # swipekb_helper/exposure.py does the same check inside the helper; the tests pin both to the same cases.
    local f
    for f in tcp tcp6; do
        [ -r "$PROC_NET/$f" ] || continue
        awk 'NR > 1 && $4 == "0A" {
            n = split($2, a, ":"); addr = toupper(a[1]); port = toupper(a[2])
            if (port != "1F90" && port != "1F91") next
            if (length(addr) == 8 && substr(addr, 7, 2) == "7F") next
            if (addr == "00000000000000000000000001000000") next
            if (length(addr) == 32 && substr(addr, 1, 24) == "0000000000000000FFFF0000" && substr(addr, 31, 2) == "7F") next
            print (port == "1F90" ? 8080 : 8081)
        }' "$PROC_NET/$f"
    done | sort -u
}
# WHY stop-then-start and not `restart` (found in the SteamOS VM, M8): SteamOS keeps a small forwarding service
# (steam-web-debug-portforward.service) alive for 5 minutes after the last connection to port 8081. While it runs,
# `systemctl restart` of the socket FAILS ("already active, refusing"), the old listener stays in use, and a
# reopened door ends up with no listener at all. Stopping both and starting the socket fresh always works.
restart_socket() { sudo systemctl stop "$SOCKET_UNIT" "$PROXY_UNIT" && sudo systemctl start "$SOCKET_UNIT"; }
socket_unit_exists() { systemctl cat "$SOCKET_UNIT" >/dev/null 2>&1; }
dropin_is_ours() { [ -f "$DROPIN" ] && [ "$(head -n 1 "$DROPIN")" = '# swipekb-gaming-managed' ]; }
dropin_text() {
    echo '# swipekb-gaming-managed'
    echo '# Written by the Swipe Keyboard installer. Makes SteamOS answer Steam debug requests from this Deck only,'
    echo '# not from other devices on the network. Remove with: bash uninstall.sh (or delete this file, then'
    echo "# run: systemctl daemon-reload && systemctl stop $SOCKET_UNIT $PROXY_UNIT"
    echo "# && systemctl start $SOCKET_UNIT)."
    echo '[Socket]'
    # An empty ListenStream= first clears SteamOS'"'"'s own "listen everywhere" line; the second line is the only one left.
    echo 'ListenStream='
    echo 'ListenStream=127.0.0.1:8081'
}
remove_block() { # remove our drop-in (needs the administrator password); returns 1 if it could not
    dropin_is_ours || return 0
    if ! sudo -v; then
        echo "Could not get administrator rights, so the outside-access block was left in place: $DROPIN" >&2
        return 1
    fi
    sudo rm -f "$DROPIN"
    sudo rmdir "$DROPIN_DIR" 2>/dev/null || true   # only succeeds if empty; never removes anyone else's files
    sudo systemctl daemon-reload || true
    restart_socket || true
    local keep=() item
    for item in "${CREATED_SYSTEM_FILES[@]+"${CREATED_SYSTEM_FILES[@]}"}"; do [ "$item" = "$DROPIN" ] || keep+=("$item"); done
    CREATED_SYSTEM_FILES=("${keep[@]+"${keep[@]}"}")
    return 0
}
# close_outside_door: 0 = confirmed closed, 1 = could not (reason already printed). Never leaves a half-done change behind.
close_outside_door() {
    if dropin_is_ours && cmp -s <(dropin_text) "$DROPIN" && [ -z "$(outside_doors)" ]; then
        echo "The outside door is already closed (from an earlier run); no password needed."
        in_list "$DROPIN" "${CREATED_SYSTEM_FILES[@]+"${CREATED_SYSTEM_FILES[@]}"}" || { CREATED_SYSTEM_FILES+=("$DROPIN"); write_record; }
        return 0
    fi
    if [ -e "$DROPIN" ] && ! dropin_is_ours; then
        echo "The outside door was NOT closed: $DROPIN already exists and was not made by this installer. Move it away and run this again." >&2
        return 1
    fi
    if socket_unit_exists; then
        echo
        echo "Closing the outside door needs your Deck's administrator password, once."
        echo "(Type it when asked; nothing shows while you type. If you have never set one, press Ctrl+C, open Konsole, run: passwd)"
        if ! command -v sudo >/dev/null 2>&1 || ! sudo -v; then
            cat >&2 <<'TEXT'

The outside door was NOT closed: the administrator password was not given (or none is set on this Deck).
To set one: open Konsole and run: passwd
TEXT
            return 1
        fi
        local existed=0; [ -e "$DROPIN" ] && existed=1
        if ! { sudo mkdir -p "$DROPIN_DIR" && dropin_text | sudo tee "$DROPIN" >/dev/null && sudo systemctl daemon-reload && restart_socket; }; then
            remove_block || true
            echo "The outside door was NOT closed: the system refused the change. Nothing else was changed." >&2
            return 1
        fi
        [ "$existed" = 1 ] || in_list "$DROPIN" "${CREATED_SYSTEM_FILES[@]+"${CREATED_SYSTEM_FILES[@]}"}" || CREATED_SYSTEM_FILES+=("$DROPIN")
        in_list "$DROPIN" "${CREATED_SYSTEM_FILES[@]+"${CREATED_SYSTEM_FILES[@]}"}" || CREATED_SYSTEM_FILES+=("$DROPIN")
        write_record
    fi
    # WHY look again instead of trusting the commands: this is the only proof that the door is really shut.
    local still; still="$(outside_doors | tr '\n' ' ')"
    if [ -n "$still" ]; then
        remove_block || true
        write_record
        echo "The outside door was NOT closed: Steam's debug port is still reachable from other devices after the change (port $still). The change was undone." >&2
        return 1
    fi
    echo "The outside door is closed: Steam's debug switch now answers this Deck only."
    return 0
}

# --- 1. Checks that change nothing -----------------------------------------------------------------
command -v flatpak >/dev/null 2>&1 || die "'flatpak' was not found. This installer is for SteamOS, where it comes with the system."
command -v systemctl >/dev/null 2>&1 || die "'systemctl' was not found. This installer is for SteamOS, where it comes with the system."

[ -f "$HERE/$WATCH_NAME" ] || die "$WATCH_NAME missing from the download."
[ -f "$HERE/VERSION" ] || die "VERSION file missing next to install.sh. Run it from the folder you extracted the download into."
VERSION="$(head -n 1 "$HERE/VERSION")"
# WHY verify before anything else: the download is the only thing we run code from; a damaged or edited
# file must stop here, before Steam's folder or your services are touched.
[ -f "$HERE/SHA256SUMS" ] || die "SHA256SUMS missing from the download. Download it again."
(cd "$HERE" && sha256sum -c SHA256SUMS >/dev/null 2>&1) || die "Checksum mismatch: a file in this download is damaged or was changed. Download it again; nothing was changed."

MAIN_BUNDLE=( "$HERE"/swipe-keyboard-[0-9]*-x86_64.flatpak )
HELPER_BUNDLE=( "$HERE"/swipe-keyboard-gaming-helper-*-x86_64.flatpak )
if [ "$SKIP_FLATPAKS" = 0 ]; then
    { [ -f "${MAIN_BUNDLE[0]}" ] && [ "${#MAIN_BUNDLE[@]}" = 1 ]; } || die "Expected exactly one Swipe Keyboard .flatpak file next to install.sh."
    { [ -f "${HELPER_BUNDLE[0]}" ] && [ "${#HELPER_BUNDLE[@]}" = 1 ]; } || die "Expected exactly one Gaming Mode helper .flatpak file next to install.sh."
fi
for unit in "${UNITS[@]}" "${UPDATE_UNITS[@]}"; do [ -f "$HERE/units/$unit" ] || die "units/$unit missing from the download."; done
{ [ -f "$HERE/$UPDATE_NAME" ] && [ -f "$HERE/update-key.gpg" ]; } || die "$UPDATE_NAME or update-key.gpg missing from the download. Download it again."

OLD_VERSION="$(record_value version)"
if [ -n "$OLD_VERSION" ] && [ "$OLD_VERSION" != "$VERSION" ] && [ "$FORCE" = 0 ]; then
    newest="$(printf '%s\n%s\n' "$OLD_VERSION" "$VERSION" | sort -V | tail -n 1)"
    [ "$newest" = "$VERSION" ] || die "Version $OLD_VERSION is installed and this download ($VERSION) is older. Use --force to go back. Nothing was changed."
fi
# What an earlier run chose. A record from before this choice existed (no "gaming" line) was a Gaming Mode install.
CURRENT_GAMING="$(record_value gaming)"
[ -n "$CURRENT_GAMING" ] || { [ -f "$RECORD" ] && CURRENT_GAMING=yes || CURRENT_GAMING=no; }

# Gaming Mode needs Steam's folder and no foreign unit files. These only make Gaming Mode unavailable (so the
# Desktop keyboard can still be installed); they are not reasons to refuse everything.
# Where Steam keeps its folder. ~/.steam/steam is normally a link to ~/.local/share/Steam; resolve both so the
# switch file is made once per real folder.
STEAM_DIRS=()
for candidate in "$HOME/.steam/steam" "$HOME/.local/share/Steam"; do
    [ -d "$candidate" ] || continue
    real="$(readlink -f "$candidate")"
    in_list "$real" "${STEAM_DIRS[@]+"${STEAM_DIRS[@]}"}" || STEAM_DIRS+=("$real")
done
GAMING_BLOCKED=""
if [ "${#STEAM_DIRS[@]}" = 0 ]; then
    GAMING_BLOCKED="Steam's folder was not found (looked for ~/.steam/steam and ~/.local/share/Steam). Start Steam once in Desktop Mode or Gaming Mode, then run this again."
fi
# WHY refuse a same-named file that is not ours: it may be yours; replacing it would break something we know nothing about.
for unit in "${UNITS[@]}"; do
    if [ -e "$UNIT_DIR/$unit" ] && [ "$(head -n 1 "$UNIT_DIR/$unit")" != '# swipekb-gaming-managed' ]; then
        GAMING_BLOCKED="$UNIT_DIR/$unit already exists and was not made by this installer. Move it away or rename it, then run this again."
    fi
done
if [ "$CHOICE" = gaming ] && [ -n "$GAMING_BLOCKED" ]; then die "$GAMING_BLOCKED Nothing was changed."; fi

# --- 2. The choice ---------------------------------------------------------------------------------
# WHY the safer default: pressing Enter without reading must never switch on a Valve developer feature or ask for
# an administrator password. A first install, or --yes without a choice, gets the Desktop keyboard only; someone
# who already has Gaming Mode keeps it.
DEFAULT_CHOICE=desktop
[ "$CURRENT_GAMING" = yes ] && DEFAULT_CHOICE=gaming
[ -z "$GAMING_BLOCKED" ] || DEFAULT_CHOICE=desktop

cat <<TEXT
Swipe Keyboard for Steam Deck, version $VERSION

You have two choices. You can change your mind later: run this installer again.

  1) Desktop Mode keyboard only
     Swipe typing and word suggestions in Desktop Mode. Nothing about Steam or your network changes, and no
     administrator password is needed. Steam's Gaming Mode keyboard stays exactly as it is.

  2) Desktop Mode keyboard AND Gaming Mode swipe
     Adds swipe typing and word suggestions on top of Steam's own on-screen keyboard in Gaming Mode, over Steam
     and over games. Steam's keyboard stays as it is underneath; if anything goes wrong you simply have Steam's
     normal keyboard.

     How it works: Steam has a hidden developer switch. We turn it on and use it to attach to Steam's keyboard.

     What you get:
       + Swipe typing and suggestions in Gaming Mode.
       + Everything can be undone: run this installer again and choose 1, or run uninstall.sh.

     What it costs, and the risks:
       - Steam's Subscriber Agreement says you may not modify or create derivative works of software accessed
         through Steam without Valve's written consent. Whether this counts is unclear. We change no Steam file, we
         do not touch games or anti-cheat, and a popular community tool (Decky Loader) uses the same switch, but
         Valve has not approved this and could stop it or object at any time. This is not legal advice.
       - On SteamOS this developer switch can also be reached from other devices on your Wi-Fi or home network
         (SteamOS opens a second door, port 8081). If you choose 2, the next question lets you block that.
       - Valve can change Steam at any time and break this. Then Gaming Mode swipe switches itself off and Steam's
         keyboard keeps working.
       - Steam needs one restart before it notices the switch. This installer never restarts it for you.
       - Passwords: inside Steam's own password boxes the keyboard layer does nothing. Over games and other programs
         Steam does not say whether a box is a password box, so nothing is learned or saved there.

TEXT
if [ -n "$GAMING_BLOCKED" ]; then
    echo "Gaming Mode is not available right now: $GAMING_BLOCKED"
    echo
fi

if [ -z "$CHOICE" ]; then
    if [ "$DRY_RUN" = 1 ] || [ "$ASSUME_YES" = 1 ]; then
        CHOICE="$DEFAULT_CHOICE"
    else
        if [ -n "$GAMING_BLOCKED" ]; then
            CHOICE=desktop
        else
            [ "$DEFAULT_CHOICE" = gaming ] && default_label="2 (keep Gaming Mode on)" || default_label="1 (Desktop only)"
            [ "$CURRENT_GAMING" = yes ] && echo "Gaming Mode is on now. Choosing 1 turns it off and keeps the Desktop keyboard."
            printf 'Type 1 or 2 and press Enter. Just pressing Enter picks %s: ' "$default_label"
            answer=""
            read -r answer || answer=""
            case "$answer" in
                "") CHOICE="$DEFAULT_CHOICE" ;;
                1) CHOICE=desktop ;;
                2) CHOICE=gaming ;;
                *) echo "Nothing was changed."; exit 1 ;;
            esac
        fi
    fi
fi

# Second question, only for Gaming Mode: who else may reach Steam's developer switch? Its own choice because some people
# want network access on purpose (developer tools on another computer). Enter = Block, the safer reading.
CURRENT_OUTSIDE="$(record_value outside_access)"; [ -n "$CURRENT_OUTSIDE" ] || CURRENT_OUTSIDE=blocked
# WHY on unless you said no: owner decision (2026-10-23), updates install by themselves on every platform. A record
# from before this choice existed gets it on too; an explicit earlier "no" is kept.
CURRENT_AUTO="$(record_value auto_update)"
[ -n "$AUTO" ] || AUTO="$CURRENT_AUTO"
[ -n "$AUTO" ] || AUTO=yes
AUTO_BLOCKED=""
for unit in "${UPDATE_UNITS[@]}"; do
    if [ -e "$UNIT_DIR/$unit" ] && [ "$(head -n 1 "$UNIT_DIR/$unit")" != '# swipekb-gaming-managed' ]; then
        AUTO_BLOCKED="$UNIT_DIR/$unit already exists and was not made by this installer"
    fi
done
DEFAULT_OUTSIDE=blocked; [ "$CURRENT_GAMING" = yes ] && DEFAULT_OUTSIDE="$CURRENT_OUTSIDE"
outside_text() {
    cat <<TEXT
Second question: should other devices on your network be blocked from Steam's developer switch?

On SteamOS the switch can also be reached from other devices on your Wi-Fi or home network (SteamOS opens a second
door, port 8081, to the whole network). Anyone there could control Steam's screens and run programs on your Deck.

  1) Block other devices (recommended)
     + Only this Deck can use the switch. Safe on shared or public Wi-Fi.
     - Asks for your Deck's administrator password once. If you have never set one, open Konsole and run: passwd
     - Tools on another computer that use the switch over the network stop working until you run this again and
       choose 2.

  2) Leave it open
     + No password needed. Keeps network access for people who use it on purpose (for example developer tools on
       another computer).
     - Anyone on the same network can control Steam on your Deck. Do not choose this on public or shared Wi-Fi.

TEXT
}
if [ "$CHOICE" = gaming ] || { [ "$DRY_RUN" = 1 ] && [ "$CHOICE_FROM_FLAG" = 0 ] && [ -z "$GAMING_BLOCKED" ]; }; then
    outside_text
    if [ -z "$OUTSIDE" ]; then
        if [ "$CHOICE" = gaming ] && [ "$DRY_RUN" = 0 ] && [ "$ASSUME_YES" = 0 ]; then
            [ "$DEFAULT_OUTSIDE" = open ] && default_label="2 (leave open)" || default_label="1 (block)"
            printf 'Type 1 or 2 and press Enter. Just pressing Enter picks %s: ' "$default_label"
            answer=""; read -r answer || answer=""
            case "$answer" in
                "") OUTSIDE="$DEFAULT_OUTSIDE" ;;
                1) OUTSIDE=blocked ;;
                2) OUTSIDE=open ;;
                *) echo "Nothing was changed."; exit 1 ;;
            esac
        else
            OUTSIDE="$DEFAULT_OUTSIDE"
        fi
    fi
fi
[ -n "$OUTSIDE" ] || OUTSIDE="$DEFAULT_OUTSIDE"

# --- 3. Say exactly what will happen ---------------------------------------------------------------
# A dry run with no choice given shows both plans, so you can see what each option would do.
SHOW_GAMING=0; { [ "$CHOICE" = gaming ] || { [ "$DRY_RUN" = 1 ] && [ "$CHOICE_FROM_FLAG" = 0 ] && [ -z "$GAMING_BLOCKED" ]; }; } && SHOW_GAMING=1
echo "The exact changes:"
if [ "$SKIP_FLATPAKS" = 0 ]; then
    echo "  - add the Flathub source for your user if missing, and install the keyboard app for your user only:"
    echo "      $MAIN_ID  (the word-guessing engine and Desktop Mode keyboard; it has no network access)"
    echo "    Flatpak may download its supporting components (the KDE runtime) from Flathub once, if not already here."
fi
if [ "$SHOW_GAMING" = 1 ]; then
    [ "$DRY_RUN" = 1 ] && [ "$CHOICE" != gaming ] && echo "  If you choose 2, these are added:"
    if [ "$OUTSIDE" = blocked ]; then
        echo "  - ask for your Deck's administrator password once, then close the outside door: write one small file"
        echo "      $DROPIN"
        echo "    so that SteamOS's $SOCKET_UNIT answers only this Deck (it takes effect at once and survives restarts)."
        echo "    The installer then checks that port 8081 really is closed to the network. If not, it undoes the change"
        echo "    and asks whether to leave it open or skip Gaming Mode."
    else
        echo "  - leave SteamOS's outside door open, as you chose: no password, no system change. Other devices on your"
        echo "    network can control Steam while the developer switch is on."
    fi
    echo "  - write the word '$OUTSIDE' to $OUTSIDE_FILE so the helper knows your choice."
    for dir in "${STEAM_DIRS[@]+"${STEAM_DIRS[@]}"}"; do
        if [ -e "$dir/$MARKER_NAME" ]; then
            echo "  - $dir/$MARKER_NAME already exists; it is left exactly as it is (and is never removed by uninstall)."
        else
            echo "  - create the empty file $dir/$MARKER_NAME (the developer switch; Steam reads it when it starts)."
        fi
    done
    [ "$SKIP_FLATPAKS" = 1 ] || echo "  - install the Gaming Mode helper app for your user only: $HELPER_ID  (the small program that talks to Steam on this device)"
    for unit in "${UNITS[@]}"; do echo "  - write $UNIT_DIR/$unit and start it (it starts by itself after every restart)."; done
    echo "  - write $WATCH_SCRIPT. Once a minute it only checks that the developer-switch file still exists and never changes"
    echo "    Steam's folder. If the file ever disappears, it shows a desktop notification (in Desktop Mode) with a"
    echo "    'Turn it back on' button; Gaming Mode cannot show notifications, so there Steam's normal keyboard is used."
elif [ "$CURRENT_GAMING" = yes ]; then
    echo "  - turn Gaming Mode off: stop and delete the two background services, delete the developer-switch file if this"
    echo "    installer created it (kept if Decky Loader is installed, because it relies on that file), remove the Gaming Mode helper app if this installer added it, and remove the outside-door"
    echo "    block (asks for the administrator password once). The Desktop keyboard stays."
fi
if [ "$AUTO" = yes ] && [ -z "$AUTO_BLOCKED" ]; then
    echo "  - turn on automatic updates: point the installed apps at the signed update site"
    echo "      $UPDATE_SITE"
    echo "    and write $UNIT_DIR/swipekb-update.timer, which once a day asks Flatpak to install a newer Swipe Keyboard"
    echo "    (only our apps; Flatpak refuses anything not signed with our key). Turn off: run this with --no-auto-update."
elif [ "$AUTO" = yes ]; then
    echo "  - automatic updates stay off: $AUTO_BLOCKED. Move it away and run this again to turn them on."
else
    echo "  - automatic updates off, as you chose: update by running a newer download's install.sh."
fi
echo "  - write a note of what was created to $RECORD, so uninstall.sh removes only that."
cat <<TEXT

The keyboard itself never uses the internet. Automatic updates only fetch new versions of Swipe Keyboard.
TEXT
if [ "$SHOW_GAMING" = 1 ]; then
    cat <<TEXT
After this, switch to Gaming Mode. If Steam was already running, restart Steam once so it notices the developer
switch. This installer never restarts Steam for you.
TEXT
fi
echo "To undo everything: bash uninstall.sh (in this same download). To switch Gaming Mode on or off later: run this installer again."

if [ "$DRY_RUN" = 1 ]; then
    echo
    echo "Dry run: nothing was changed."
    exit 0
fi

# --- 4. Ask ----------------------------------------------------------------------------------------
if [ "$ASSUME_YES" = 0 ]; then
    printf '\nContinue? [y/N] '
    answer=""
    read -r answer || answer=""
    case "$answer" in
        y|Y|yes|YES|Yes) ;;
        *) echo "Nothing was changed."; exit 1 ;;
    esac
fi

# --- 5. Do it, recording each step as it happens ---------------------------------------------------
CREATED_MARKERS=(); EXISTING_MARKERS=(); CREATED_UNITS=(); CREATED_SYSTEM_FILES=(); INSTALLED_APPS=()
while IFS= read -r line; do [ -n "$line" ] && CREATED_MARKERS+=("$line"); done < <(record_list created_markers)
while IFS= read -r line; do [ -n "$line" ] && EXISTING_MARKERS+=("$line"); done < <(record_list existing_markers)
while IFS= read -r line; do [ -n "$line" ] && CREATED_UNITS+=("$line"); done < <(record_list created_units)
while IFS= read -r line; do [ -n "$line" ] && CREATED_SYSTEM_FILES+=("$line"); done < <(record_list created_system_files)
while IFS= read -r line; do [ -n "$line" ] && INSTALLED_APPS+=("$line"); done < <(record_list installed_flatpaks)
INSTALLED_AT="$(record_value installed_at)"; [ -n "$INSTALLED_AT" ] || INSTALLED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
GAMING_STATE="$CURRENT_GAMING"
OUTSIDE_STATE="$CURRENT_OUTSIDE"
AUTO_STATE="$CURRENT_AUTO"
UPGRADE=0; [ -f "$RECORD" ] && [ "$CURRENT_GAMING" = yes ] && UPGRADE=1
EXIT_CODE=0

install_app() { # install_app ID BUNDLE
    local id="$1" bundle="$2" already=0
    flatpak info --user "$id" >/dev/null 2>&1 && already=1
    # WHY the output is kept: on a real SteamOS Deck (M8 VM run) Flatpak refuses to install a bundle whose exact version
    # is already here ("... already installed", exit 1). That means "nothing to do", and without this every re-run
    # (the way to change a choice or upgrade) stopped before doing anything. Any other failure is still fatal.
    local log rc=0; log="$(mktemp)"
    LC_ALL=C flatpak install --user --noninteractive -y "$bundle" 2>&1 | tee "$log" || rc=$?
    if [ "$rc" != 0 ]; then
        if [ "$already" = 1 ] && grep -qi 'already installed' "$log"; then
            echo "$id is already installed at this version; nothing to do."
        else
            rm -f "$log"
            die "Installing $id failed. If it asked to download something, check your internet connection and run this again."
        fi
    fi
    rm -f "$log"
    # Only an app that was NOT here before is ours to remove later.
    if [ "$already" = 0 ] && ! in_list "$id" "${INSTALLED_APPS[@]+"${INSTALLED_APPS[@]}"}"; then
        INSTALLED_APPS+=("$id")
    fi
    write_record
}

point_at_update_site() { # point_at_update_site ID: let Flatpak update this app from our signed site
    local id="$1" origin
    flatpak info --user "$id" >/dev/null 2>&1 || return 0
    origin="$(flatpak info --user --show-origin "$id")"
    # WHY modify the app's own origin: an app installed from a downloaded bundle gets a remote with no address, so
    # Flatpak never looks for updates. Giving that remote our address and key makes "flatpak update" (and Discover's
    # Updates page) use the site, without downloading the app again.
    # WHY --gpg-verify: importing the key alone does NOT switch checking on; a bundle's origin remote starts with
    # signature checks off (found in review, 2026-10-23). With it, Flatpak refuses any update not signed with our key.
    # WHY modify first, without looking at the list: Flatpak 1.18 (SteamOS 3.9) hides a bundle's origin remote from
    # "flatpak remotes" (it is disabled until it has an address), while on SteamOS 3.8 the helper's origin was already
    # gone. Both seen in the VM, 2026-10-23. Only "not found" means it must be created.
    local log; log="$(mktemp)"
    # WHY LC_ALL=C: the "not found" test below reads Flatpak's English message; a Deck set to another language must still match.
    if LC_ALL=C flatpak remote-modify --user --url="$UPDATE_SITE" --gpg-verify --gpg-import="$HERE/update-key.gpg" "$origin" 2>"$log"; then
        :
    elif grep -qi 'not found' "$log"; then
        # Recreate it under the same name: "flatpak update" looks the app's origin up by that name. remote-add checks
        # signatures by default; --no-enumerate keeps it out of listings.
        flatpak remote-add --user --no-enumerate --gpg-import="$HERE/update-key.gpg" "$origin" "$UPDATE_SITE" \
            || echo "Could not point $id at the update site; it keeps working and can be updated by hand." >&2
    else
        cat "$log" >&2
        echo "Could not point $id at the update site; it keeps working and can be updated by hand." >&2
    fi
    rm -f "$log"
}

auto_update_on() {
    local unit
    point_at_update_site "$MAIN_ID"
    point_at_update_site "$HELPER_ID"
    mkdir -p "$UNIT_DIR" "$RECORD_DIR"
    # Copied (not run from the download) so it keeps working if the download folder is deleted.
    cp "$HERE/$UPDATE_NAME" "$UPDATE_SCRIPT.new" && chmod 755 "$UPDATE_SCRIPT.new" && mv "$UPDATE_SCRIPT.new" "$UPDATE_SCRIPT"
    for unit in "${UPDATE_UNITS[@]}"; do
        sed -e "s/@VERSION@/$VERSION/g" -e "s|@UPDATE_SCRIPT@|$UPDATE_SCRIPT|g" "$HERE/units/$unit" > "$UNIT_DIR/$unit.new"
        mv "$UNIT_DIR/$unit.new" "$UNIT_DIR/$unit"
        in_list "$unit" "${CREATED_UNITS[@]+"${CREATED_UNITS[@]}"}" || CREATED_UNITS+=("$unit")
    done
    systemctl --user daemon-reload
    systemctl --user enable --now swipekb-update.timer
    AUTO_STATE=yes
    write_record
}

auto_update_off() { # stop and delete only our update timer; the apps stay as they are
    local unit keep=() item
    for unit in "${UPDATE_UNITS[@]}"; do
        if [ -f "$UNIT_DIR/$unit" ] && [ "$(head -n 1 "$UNIT_DIR/$unit")" = '# swipekb-gaming-managed' ]; then
            systemctl --user disable --now "$unit" 2>/dev/null || true
            rm -f "${UNIT_DIR:?}/${unit:?}"
        fi
    done
    systemctl --user daemon-reload || true
    rm -f "${UPDATE_SCRIPT:?}"
    for item in "${CREATED_UNITS[@]+"${CREATED_UNITS[@]}"}"; do in_list "$item" "${UPDATE_UNITS[@]}" || keep+=("$item"); done
    CREATED_UNITS=("${keep[@]+"${keep[@]}"}")
    AUTO_STATE=no
    write_record
}

apply_auto_update() {
    if [ "$AUTO" = yes ] && [ -z "$AUTO_BLOCKED" ]; then auto_update_on
    elif [ "$AUTO" = no ]; then auto_update_off; fi
}

# WHY the apps come first: it is the one step that needs the internet and can fail, so if it does we have
# not yet switched anything on.
if [ "$SKIP_FLATPAKS" = 0 ]; then
    flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
    install_app "$MAIN_ID" "${MAIN_BUNDLE[0]}"
fi
apply_auto_update

gaming_off() { # turn Gaming Mode off, keep the Desktop keyboard; touches only what the record says we made
    local unit marker keep item ours=()
    # WHY only units that carry our stamp (review fix 4): a unit file of yours with the same name is not ours to switch off.
    for unit in "${UNITS[@]}"; do
        if [ -f "$UNIT_DIR/$unit" ] && [ "$(head -n 1 "$UNIT_DIR/$unit")" = '# swipekb-gaming-managed' ]; then ours+=("$unit"); fi
    done
    if [ "${#ours[@]}" -gt 0 ]; then systemctl --user disable --now "${ours[@]}" || true; fi
    for unit in "${UNITS[@]}"; do
        if [ -f "$UNIT_DIR/$unit" ] && [ "$(head -n 1 "$UNIT_DIR/$unit")" = '# swipekb-gaming-managed' ]; then rm -f "${UNIT_DIR:?}/${unit:?}"; fi
    done
    systemctl --user daemon-reload || true
    rm -f "${WATCH_SCRIPT:?}" "${STATE_DIR:?}/switch-state" "${STATE_DIR:?}/switch-notified" "${STATE_DIR:?}/retry.json"
    local kept=()
    for marker in "${CREATED_MARKERS[@]+"${CREATED_MARKERS[@]}"}"; do
        if ! marker_is_switch_file "$marker"; then
            echo "Left $marker alone: it is not Steam's developer-switch file in Steam's folder."
        elif decky_present; then
            echo "Kept $marker: Decky Loader appears to be installed and relies on this developer-switch file."
            kept+=("$marker")      # no longer ours to remove: from now on it counts as a file that was there before
        # WHY a regular file only: never follow a link or remove a folder because a path was edited by hand.
        elif [ -f "$marker" ] && [ ! -L "$marker" ]; then rm -f "${marker:?}"; fi
    done
    for marker in "${kept[@]+"${kept[@]}"}"; do
        in_list "$marker" "${EXISTING_MARKERS[@]+"${EXISTING_MARKERS[@]}"}" || EXISTING_MARKERS+=("$marker")
    done
    CREATED_MARKERS=(); CREATED_UNITS=()
    if in_list "$HELPER_ID" "${INSTALLED_APPS[@]+"${INSTALLED_APPS[@]}"}"; then
        flatpak uninstall --user -y "$HELPER_ID" || true
        keep=(); for item in "${INSTALLED_APPS[@]}"; do [ "$item" = "$HELPER_ID" ] || keep+=("$item"); done
        INSTALLED_APPS=("${keep[@]+"${keep[@]}"}")
    fi
    # The block is removed last, and kept (with a note) if the password is not given: leaving the door closed is the safe side.
    if ! remove_block; then
        echo "Gaming Mode is off, but the outside-door block is still in place. That is harmless. Remove it any time with uninstall.sh." >&2
    fi
    GAMING_STATE=no
    rm -f "${OUTSIDE_FILE:?}"
    write_record
}

if [ "$CHOICE" = desktop ]; then
    if [ "$CURRENT_GAMING" = yes ]; then
        gaming_off
        echo
        echo "Gaming Mode is off. The Desktop Mode keyboard is installed. If Steam is running it may still have its debug switch open until its next restart."
    else
        write_record
        echo
        echo "Done. The Desktop Mode keyboard is installed. To add Gaming Mode later, run this installer again and choose 2."
    fi
    exit 0
fi

# Gaming Mode. Order matters: if you chose to block other devices, the outside door is closed and checked BEFORE the
# developer switch file is made, so there is never a moment when the switch is on and the door is open against your choice.
DOOR_OK=0
if [ "$OUTSIDE" = blocked ]; then
    if close_outside_door; then
        DOOR_OK=1
    elif [ "$ASSUME_YES" = 0 ]; then
        # WHY ask again and never carry on silently: you picked "block"; opening it needs your say-so, not our fallback.
        echo
        echo "What now?"
        echo "  1) Leave the door open and turn Gaming Mode on anyway (anyone on your network can control Steam)"
        echo "  2) Skip Gaming Mode (you keep the Desktop Mode keyboard)"
        printf 'Type 1 or 2 and press Enter. Just pressing Enter picks 2 (skip): '
        answer=""; read -r answer || answer=""
        if [ "$answer" = 1 ]; then OUTSIDE=open; echo "Leaving the outside door open, as you chose."; fi
    fi
fi
if [ "$OUTSIDE" = open ]; then
    # Switching from "block" to "open" on a later run: take our block away again (needs the password; if it is not
    # given the block simply stays, which is the safe side).
    if dropin_is_ours; then remove_block || echo "The earlier block is still in place; it is harmless." >&2; fi
    DOOR_OK=1
fi
if [ "$DOOR_OK" = 0 ]; then
    GAMING_STATE="$CURRENT_GAMING"
    write_record
    echo
    if [ "$CURRENT_GAMING" = yes ]; then
        echo "Your existing Gaming Mode install was left as it is; the helper keeps the layer off while the outside door is open."
    else
        echo "Gaming Mode was NOT turned on. Done, with the Desktop Mode keyboard. Run this installer again to add Gaming Mode."
    fi
    exit 3
fi
OUTSIDE_STATE="$OUTSIDE"
mkdir -p "$STATE_DIR"
printf '%s\n' "$OUTSIDE" > "$OUTSIDE_FILE"
[ "$SKIP_FLATPAKS" = 1 ] || install_app "$HELPER_ID" "${HELPER_BUNDLE[0]}"
[ "$AUTO_STATE" != yes ] || point_at_update_site "$HELPER_ID"

for dir in "${STEAM_DIRS[@]}"; do
    marker="$dir/$MARKER_NAME"
    if [ -e "$marker" ]; then
        # WHY check the record: on a second run the file exists because WE made it. It must stay marked as ours,
        # or uninstall would leave the developer switch on.
        if ! in_list "$marker" "${CREATED_MARKERS[@]+"${CREATED_MARKERS[@]}"}" && ! in_list "$marker" "${EXISTING_MARKERS[@]+"${EXISTING_MARKERS[@]}"}"; then
            EXISTING_MARKERS+=("$marker")
        fi
    else
        : > "$marker"
        CREATED_MARKERS+=("$marker")
        # WHY: if an earlier run saw this file already there and it has since been removed, it is ours now, not "from before".
        keep=(); for item in "${EXISTING_MARKERS[@]+"${EXISTING_MARKERS[@]}"}"; do [ "$item" = "$marker" ] || keep+=("$item"); done
        EXISTING_MARKERS=("${keep[@]+"${keep[@]}"}")
        # WHY say so: on a re-run with Gaming Mode already on, a missing file means the switch had been turned off.
        if [ "$CURRENT_GAMING" = yes ]; then
            echo "Steam's developer switch had been turned off (its file was gone). This run turned it back on: $marker"
            echo "Restart Steam once so it notices; until then Steam's normal keyboard is used."
            rm -f "$STATE_DIR/switch-state"
        fi
    fi
    write_record
done

mkdir -p "$UNIT_DIR"
# The watcher is copied (not run from the download) so it keeps working if the download folder is deleted.
mkdir -p "$RECORD_DIR"
cp "$HERE/$WATCH_NAME" "$WATCH_SCRIPT.new" && chmod 755 "$WATCH_SCRIPT.new" && mv "$WATCH_SCRIPT.new" "$WATCH_SCRIPT"
rm -f "$STATE_DIR/switch-notified"
# WHY (review fix 3): running the installer again is the way to ask the helper to try again after it gave up on a Steam version
# (the helper restarts below and reads this file only when it starts).
rm -f "$STATE_DIR/retry.json"
for unit in "${UNITS[@]}"; do
    # WHY the version is written into the file: it shows which download put it there, and re-running with a
    # newer download rewrites it.
    sed -e "s/@VERSION@/$VERSION/g" -e "s|@WATCH_SCRIPT@|$WATCH_SCRIPT|g" "$HERE/units/$unit" > "$UNIT_DIR/$unit.new"
    mv "$UNIT_DIR/$unit.new" "$UNIT_DIR/$unit"
    in_list "$unit" "${CREATED_UNITS[@]+"${CREATED_UNITS[@]}"}" || CREATED_UNITS+=("$unit")
done
GAMING_STATE=yes
write_record

systemctl --user daemon-reload
systemctl --user enable --now "${UNITS[@]}"
# WHY restart on an upgrade: "enable --now" leaves an already-running old copy running.
[ "$UPGRADE" = 0 ] || systemctl --user restart "${UNITS[@]}"

cat <<TEXT

Done. Switch to Gaming Mode and touch a text field. If Steam was already running, restart Steam once.
To undo: bash uninstall.sh. To turn Gaming Mode off but keep the Desktop keyboard: run this installer again and choose 1.
TEXT
exit "$EXIT_CODE"
