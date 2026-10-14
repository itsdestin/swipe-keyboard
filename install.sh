#!/usr/bin/env bash
# install.sh — build swipe-keyboard, run its tests, and install it where Plasma Keyboard loads it:
#   ~/.local/share/swipe-keyboard/{qml/SwipeKeyboard, models, dict}. touch-profile's layout + wrapper pick it up
#   (their hook does nothing when this folder is missing), then the on-screen keyboard is restarted.
# Usage: ./install.sh              build + test + install + restart
#        ./install.sh --no-restart  same, but leave the running keyboard alone (for testing, or installing
#                                   without disturbing a keyboard you are typing on)
#        ./install.sh --uninstall   remove qml/models/dict (personal data like traces/ is kept) + restart
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/swipe-keyboard"

restart_keyboard() {
    # WHY: KWin owns the keyboard process and starts a fresh one after it exits; the new one loads the new files.
    local old pid p
    # WHY: `|| true` — pidof exits 1 when no keyboard is running, which must not abort the script under set -e.
    old=$(pidof plasma-keyboard || true)
    # $old is deliberately unquoted: there may be several pids, and kill needs them as separate arguments.
    # WHY: `|| true` — a process can exit between pidof and kill; that is fine, not a failure.
    [ -n "$old" ] && { kill $old 2>/dev/null || true; }
    for _ in 1 2 3 4 5 6; do
        sleep 1
        # WHY: only a pid that was NOT in the old set counts, else we would report the dying old process as "restarted".
        for pid in $(pidof plasma-keyboard || true); do
            case " $old " in *" $pid "*) continue ;; esac
            echo "on-screen keyboard restarted"; return 0
        done
    done
    echo "on-screen keyboard not running — KWin starts it the next time a text field needs it" >&2
}

case "${1:-}" in
    --uninstall)
        rm -rf "$DEST/qml" "$DEST/models" "$DEST/dict" "$DEST/.qml.new" "$DEST/.models.new" "$DEST/.dict.new"
        restart_keyboard
        echo "swipe typing removed (keyboard back to plain tapping)"
        exit 0 ;;
    --no-restart|"") ;;
    *) echo "usage: ./install.sh [--no-restart|--uninstall]" >&2; exit 2 ;;
esac

"$ROOT/scripts/fetch-assets.sh"
[ -f "$ROOT/third_party/swipe-library/third_party/executorch/cmake-out-host/libexecutorch.a" ] || "$ROOT/scripts/build-futo.sh"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" -j"$(nproc)"
(cd "$ROOT/build" && ctest --output-on-failure)

mkdir -p "$DEST"
# WHY: a failed copy must never leave a half-installed keyboard (e.g. new qml but no models). So copy all three
# into staging dirs first; only when every copy has succeeded (set -e stops us otherwise) swap them in back to back.
rm -rf "$DEST/.qml.new" "$DEST/.models.new" "$DEST/.dict.new"
cp -r "$ROOT/build/qml" "$DEST/.qml.new"
cp -r "$ROOT/assets/models" "$DEST/.models.new"
cp -r "$ROOT/assets/dict" "$DEST/.dict.new"
for d in qml models dict; do rm -rf "$DEST/$d" && mv "$DEST/.$d.new" "$DEST/$d"; done
echo "installed to $DEST"
[ "${1:-}" = --no-restart ] || restart_keyboard
