#!/usr/bin/env bash
# Swipe Keyboard, Steam Deck Gaming Mode: installer. Runs as you, in Desktop Mode, in a terminal. No sudo.
# Plan: docs/active/plans/2026-10-22-gaming-mode.md, Section 5.
#
#   bash install.sh              show what will change, ask, then do it
#   bash install.sh --dry-run    show what would change, change nothing, ask nothing
#   bash install.sh --yes        do not ask (for scripted use; you have still been shown the text)
#   bash install.sh --skip-flatpaks   do not install the two apps (they are already installed)
#   bash install.sh --force      allow installing an OLDER version over a newer one
#
# Safe to run again: a second run changes nothing that is already right, and an upgrade is simply running the
# newer download's install.sh. Everything is undone by uninstall.sh (in the same download).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN_ID=org.itsdestin.SwipeKeyboard
HELPER_ID=org.itsdestin.SwipeKeyboard.GamingHelper
MARKER_NAME=.cef-enable-remote-debugging
UNITS=(swipekb-engine.service swipekb-helper.service)
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
UNIT_DIR="$CONFIG_HOME/systemd/user"
RECORD_DIR="$DATA_HOME/swipe-keyboard-gaming"
RECORD="$RECORD_DIR/install-record.json"

DRY_RUN=0; ASSUME_YES=0; SKIP_FLATPAKS=0; FORCE=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --yes) ASSUME_YES=1 ;;
        --skip-flatpaks) SKIP_FLATPAKS=1 ;;
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "Unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done

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
        json_list created_markers "${CREATED_MARKERS[@]+"${CREATED_MARKERS[@]}"}"
        json_list existing_markers "${EXISTING_MARKERS[@]+"${EXISTING_MARKERS[@]}"}"
        json_list created_units "${CREATED_UNITS[@]+"${CREATED_UNITS[@]}"}"
        json_list installed_flatpaks "${INSTALLED_APPS[@]+"${INSTALLED_APPS[@]}"}"
        echo '  "note": "Written by install.sh. uninstall.sh removes only what is listed as created here."'
        echo '}'
    } > "$RECORD.new"
    mv "$RECORD.new" "$RECORD"
}

# --- 1. Checks that change nothing -----------------------------------------------------------------
command -v flatpak >/dev/null 2>&1 || die "'flatpak' was not found. This installer is for SteamOS, where it comes with the system."
command -v systemctl >/dev/null 2>&1 || die "'systemctl' was not found. This installer is for SteamOS, where it comes with the system."

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
for unit in "${UNITS[@]}"; do [ -f "$HERE/units/$unit" ] || die "units/$unit missing from the download."; done

# Where Steam keeps its folder. ~/.steam/steam is normally a link to ~/.local/share/Steam; resolve both so the
# switch file is made once per real folder.
STEAM_DIRS=()
for candidate in "$HOME/.steam/steam" "$HOME/.local/share/Steam"; do
    [ -d "$candidate" ] || continue
    real="$(readlink -f "$candidate")"
    in_list "$real" "${STEAM_DIRS[@]+"${STEAM_DIRS[@]}"}" || STEAM_DIRS+=("$real")
done
[ "${#STEAM_DIRS[@]}" -gt 0 ] || die "Steam's folder was not found (looked for ~/.steam/steam and ~/.local/share/Steam). Start Steam once in Desktop Mode or Gaming Mode, then run this again."

# WHY refuse a same-named file that is not ours: it may be yours; replacing it would break something we know nothing about.
for unit in "${UNITS[@]}"; do
    if [ -e "$UNIT_DIR/$unit" ] && [ "$(head -n 1 "$UNIT_DIR/$unit")" != '# swipekb-gaming-managed' ]; then
        die "$UNIT_DIR/$unit already exists and was not made by this installer. Move it away or rename it, then run this again. Nothing was changed."
    fi
done

OLD_VERSION="$(record_value version)"
if [ -n "$OLD_VERSION" ] && [ "$OLD_VERSION" != "$VERSION" ] && [ "$FORCE" = 0 ]; then
    newest="$(printf '%s\n%s\n' "$OLD_VERSION" "$VERSION" | sort -V | tail -n 1)"
    [ "$newest" = "$VERSION" ] || die "Version $OLD_VERSION is installed and this download ($VERSION) is older. Use --force to go back. Nothing was changed."
fi

# --- 2. Say exactly what will happen ---------------------------------------------------------------
cat <<TEXT
Swipe Keyboard for Gaming Mode, version $VERSION

What this does, in plain words:

  * Gaming Mode support works by attaching to Steam's own on-screen keyboard through a developer switch that
    Valve provides in Steam. It adds swipe typing and word suggestions on top and leaves Steam's keyboard as it is.
    If anything goes wrong, you simply have Steam's normal keyboard.

  * Turning the developer switch on lets other programs on your Deck, and other devices on your Wi-Fi or home
    network, control Steam's screens while Steam is running. SteamOS itself opens a second door for this
    (port 8081, open to the whole network) and this installer cannot close it. Do not use this on a network
    you do not trust. You can turn it all off at any time with uninstall.sh.

  * Steam's Subscriber Agreement says you may not modify or create derivative works of software accessed through
    Steam without Valve's written consent. Whether this counts is unclear. We change no Steam file, we do not touch
    games or anti-cheat, and a popular community tool (Decky Loader) uses the same switch, but Valve
    has not approved this and could stop it or object at any time. This is not legal advice.

  * Passwords: inside Steam's own password boxes the keyboard layer does nothing. Over games and other programs
    Steam does not say whether a box is a password box, so nothing is learned or saved there.

The exact changes (all inside your home folder; no administrator password; nothing outside it is touched):
TEXT
for dir in "${STEAM_DIRS[@]}"; do
    if [ -e "$dir/$MARKER_NAME" ]; then
        echo "  - $dir/$MARKER_NAME already exists; it is left exactly as it is (and is never removed by uninstall)."
    else
        echo "  - create the empty file $dir/$MARKER_NAME (the developer switch; Steam reads it when it starts)."
    fi
done
if [ "$SKIP_FLATPAKS" = 0 ]; then
    echo "  - add the Flathub source for your user if missing, and install two apps for your user only:"
    echo "      $MAIN_ID  (the word-guessing engine; it has no network access)"
    echo "      $HELPER_ID  (the small program that talks to Steam on this device)"
    echo "    Flatpak may download its supporting components (the KDE runtime) from Flathub once, if not already here."
fi
for unit in "${UNITS[@]}"; do echo "  - write $UNIT_DIR/$unit and start it (it starts by itself after every restart)."; done
echo "  - write a note of what was created to $RECORD, so uninstall.sh removes only that."
cat <<TEXT

Nothing is sent to the internet by Swipe Keyboard. After this, switch to Gaming Mode. If Steam was already
running, restart Steam once so it notices the developer switch. This installer never restarts Steam for you.
To undo everything: bash uninstall.sh (in this same download).
TEXT

if [ "$DRY_RUN" = 1 ]; then
    echo
    echo "Dry run: nothing was changed."
    exit 0
fi

# --- 3. Ask ----------------------------------------------------------------------------------------
if [ "$ASSUME_YES" = 0 ]; then
    printf '\nContinue? [y/N] '
    answer=""
    read -r answer || answer=""
    case "$answer" in
        y|Y|yes|YES|Yes) ;;
        *) echo "Nothing was changed."; exit 1 ;;
    esac
fi

# --- 4. Do it, recording each step as it happens ---------------------------------------------------
CREATED_MARKERS=(); EXISTING_MARKERS=(); CREATED_UNITS=(); INSTALLED_APPS=()
while IFS= read -r line; do [ -n "$line" ] && CREATED_MARKERS+=("$line"); done < <(record_list created_markers)
while IFS= read -r line; do [ -n "$line" ] && EXISTING_MARKERS+=("$line"); done < <(record_list existing_markers)
while IFS= read -r line; do [ -n "$line" ] && CREATED_UNITS+=("$line"); done < <(record_list created_units)
while IFS= read -r line; do [ -n "$line" ] && INSTALLED_APPS+=("$line"); done < <(record_list installed_flatpaks)
INSTALLED_AT="$(record_value installed_at)"; [ -n "$INSTALLED_AT" ] || INSTALLED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
UPGRADE=0; [ -f "$RECORD" ] && UPGRADE=1

install_app() { # install_app ID BUNDLE
    local id="$1" bundle="$2" already=0
    flatpak info --user "$id" >/dev/null 2>&1 && already=1
    flatpak install --user --noninteractive -y "$bundle" || die "Installing $id failed. If it asked to download something, check your internet connection and run this again."
    # Only an app that was NOT here before is ours to remove later.
    if [ "$already" = 0 ] && ! in_list "$id" "${INSTALLED_APPS[@]+"${INSTALLED_APPS[@]}"}"; then
        INSTALLED_APPS+=("$id")
    fi
    write_record
}

# WHY the apps come first: it is the one step that needs the internet and can fail, so if it does we have
# not yet switched anything on.
if [ "$SKIP_FLATPAKS" = 0 ]; then
    flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
    install_app "$MAIN_ID" "${MAIN_BUNDLE[0]}"
    install_app "$HELPER_ID" "${HELPER_BUNDLE[0]}"
fi

# OWNER DECISION PENDING (plan Section 15, question 18): SteamOS opens the developer port on every network
# interface (8081) and this installer cannot close it without administrator rights. Options: (a) say so plainly
# [done: the text above], (b) offer an optional firewall step, (c) ship off by default. (b) and (c) are NOT
# built. If the owner chooses them, the code goes in this function, which runs just before the switch is made.
network_exposure_hook() {
    :
}
network_exposure_hook

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
    fi
    write_record
done

mkdir -p "$UNIT_DIR"
for unit in "${UNITS[@]}"; do
    # WHY the version is written into the file: it shows which download put it there, and re-running with a
    # newer download rewrites it.
    sed "s/@VERSION@/$VERSION/g" "$HERE/units/$unit" > "$UNIT_DIR/$unit.new"
    mv "$UNIT_DIR/$unit.new" "$UNIT_DIR/$unit"
    in_list "$unit" "${CREATED_UNITS[@]+"${CREATED_UNITS[@]}"}" || CREATED_UNITS+=("$unit")
done
write_record

systemctl --user daemon-reload
systemctl --user enable --now "${UNITS[@]}"
# WHY restart on an upgrade: "enable --now" leaves an already-running old copy running.
[ "$UPGRADE" = 0 ] || systemctl --user restart "${UNITS[@]}"

cat <<TEXT

Done. Switch to Gaming Mode and touch a text field. If Steam was already running, restart Steam once.
To undo: bash uninstall.sh
TEXT
