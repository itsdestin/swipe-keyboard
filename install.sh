#!/usr/bin/env bash
# install.sh — build swipe-keyboard, run its tests, and install it where Plasma Keyboard loads it:
#   ~/.local/share/swipe-keyboard/{qml/SwipeKeyboard, models, dict, lm}. touch-profile's layout + wrapper pick it up
#   (their hook does nothing when this folder is missing), then the on-screen keyboard is restarted.
# Usage: ./install.sh              build + test + install + restart
#        ./install.sh --no-restart  same, but leave the running keyboard alone (for testing, or installing
#                                   without disturbing a keyboard you are typing on)
#        ./install.sh --uninstall   remove qml/models/dict/lm (personal data like traces/ is kept) + restart
# The n-gram ranking (lm/) is off unless ~/.local/share/swipe-keyboard/use-ngram exists (then restart the keyboard).
# The neural re-ranking (nlm/) is off unless ~/.local/share/swipe-keyboard/use-nlm exists (same).
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
        rm -rf "$DEST/qml" "$DEST/models" "$DEST/dict" "$DEST/lm" "$DEST/nlm" "$DEST/.qml.new" "$DEST/.models.new" \
               "$DEST/.dict.new" "$DEST/.lm.new" "$DEST/.nlm.new"
        restart_keyboard
        echo "swipe typing removed (keyboard back to plain tapping)"
        exit 0 ;;
    --no-restart|"") ;;
    *) echo "usage: ./install.sh [--no-restart|--uninstall]" >&2; exit 2 ;;
esac

"$ROOT/scripts/fetch-assets.sh"
# Developer-only opt-in: this optional build downloads ~13 GB. The keyboard
# works without it; retaining SWIPEKB_SKIP_LM gives existing callers a veto.
if [ "${SWIPEKB_BUILD_LM:-0}" = 1 ] && [ "${SWIPEKB_SKIP_LM:-0}" != 1 ] && [ ! -s "$ROOT/assets/lm/en_ngram.bin" ]; then
    echo "building the explicitly requested n-gram model (scripts/lm/build-lm.sh)..."
    "$ROOT/scripts/lm/build-lm.sh" || echo "warning: n-gram model not built; the keyboard keeps its old ranking" >&2
fi
[ -f "$ROOT/third_party/swipe-library/third_party/executorch/cmake-out-host/libexecutorch.a" ] || "$ROOT/scripts/build-futo.sh"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" -j"$(nproc)"
(cd "$ROOT/build" && ctest --output-on-failure)

mkdir -p "$DEST"
# WHY: a failed copy must never leave a half-installed keyboard (e.g. new qml but no models). So copy all three
# into staging dirs first; only when every copy has succeeded (set -e stops us otherwise) swap them in back to back.
rm -rf "$DEST/.qml.new" "$DEST/.models.new" "$DEST/.dict.new" "$DEST/.lm.new" "$DEST/.nlm.new"
cp -r "$ROOT/build/qml" "$DEST/.qml.new"
cp -r "$ROOT/assets/models" "$DEST/.models.new"
cp -r "$ROOT/assets/dict" "$DEST/.dict.new"
# The n-gram model is optional: copied when built; when it isn't, any older copy is removed so the installed
# model always matches the installed dictionary (a stale one would still load and rank with old word lists).
dirs="qml models dict"
if [ -s "$ROOT/assets/lm/en_ngram.bin" ]; then
    mkdir -p "$DEST/.lm.new"
    cp "$ROOT/assets/lm/en_ngram.bin" "$DEST/.lm.new/"
    dirs="$dirs lm"
else
    rm -rf "$DEST/lm"
fi
# The neural word model is optional too (only fetched with SWIPEKB_FETCH_DEVELOPER_EXTRAS=1).
# WHY -L: assets/nlm/en_nlm.gguf may be
# a link to one of several downloaded models; the installed copy must be the file itself.
if [ -s "$ROOT/assets/nlm/en_nlm.gguf" ]; then
    mkdir -p "$DEST/.nlm.new"
    cp -L "$ROOT/assets/nlm/en_nlm.gguf" "$DEST/.nlm.new/"
    dirs="$dirs nlm"
else
    rm -rf "$DEST/nlm"
fi
for d in $dirs; do rm -rf "$DEST/$d" && mv "$DEST/.$d.new" "$DEST/$d"; done
echo "installed to $DEST"
[ "${1:-}" = --no-restart ] || restart_keyboard
