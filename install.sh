#!/usr/bin/env bash
# install.sh — build swipe-keyboard, run its tests, and install it where Plasma Keyboard loads it:
#   ~/.local/share/swipe-keyboard/{qml/SwipeKeyboard, models, dict}. touch-profile's layout + wrapper pick it up
#   (their hook does nothing when this folder is missing), then the on-screen keyboard is restarted.
# Usage: ./install.sh              build + test + install + restart
#        ./install.sh --no-restart  same, but leave the running keyboard alone
#        ./install.sh --uninstall   remove qml/models/dict (personal data like traces/ is kept) + restart
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/swipe-keyboard"

restart_keyboard() {
    # WHY: KWin owns the keyboard process and starts a fresh one after it exits; the new one loads the new files.
    local pid
    pid=$(pidof plasma-keyboard || true)
    [ -n "$pid" ] && kill $pid
    for _ in 1 2 3 4 5 6; do
        sleep 1
        if pidof plasma-keyboard >/dev/null; then echo "on-screen keyboard restarted"; return 0; fi
    done
    echo "on-screen keyboard not running — KWin starts it the next time a text field needs it" >&2
}

case "${1:-}" in
    --uninstall)
        rm -rf "$DEST/qml" "$DEST/models" "$DEST/dict"
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
# Stage then swap, so a half-copied module is never what the keyboard loads.
rm -rf "$DEST/.qml.new" && cp -r "$ROOT/build/qml" "$DEST/.qml.new" && rm -rf "$DEST/qml" && mv "$DEST/.qml.new" "$DEST/qml"
rm -rf "$DEST/models" "$DEST/dict"
cp -r "$ROOT/assets/models" "$DEST/models"
cp -r "$ROOT/assets/dict" "$DEST/dict"
echo "installed to $DEST"
[ "${1:-}" = --no-restart ] || restart_keyboard
