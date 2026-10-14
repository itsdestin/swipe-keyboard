#!/usr/bin/env bash
# build-en-swipe.sh — builds the shipped dictionary, assets/dict/en_swipe.combined:
#   AOSP's US English word list (Apache-2.0), every word and frequency kept, plus words from the `wordfreq`
#   package's English list (data CC-BY-SA 4.0) that AOSP lacks, minus likely misspellings, up to 250k words.
# WHY this mix: on the scoreboard it raised first-guess from 92.50% to 93.32% and top-3 from 95.43% to 96.56%
# (most misses were words AOSP simply does not have), without changing any result on the owner's own swipes.
# See .superpowers/sdd/dict-experiment-report.md for every candidate that was measured.
# Usage: scripts/dict/build-en-swipe.sh [ASSETS_DIR]   (skips the build if the output already exists)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
A="${1:-$HERE/../../assets}"
SRC="$A/dict-src"
OUT="$A/dict/en_swipe.combined"
[ -s "$OUT" ] && exit 0
AOSP=https://android.googlesource.com/platform/packages/inputmethods/LatinIME/+/refs/heads/main/dictionaries

aosp() { # name — AOSP word list, once. WHY base64: googlesource serves raw files only as base64 text.
    [ -s "$SRC/$1" ] && return 0
    mkdir -p "$SRC"
    curl -fsL "$AOSP/$1.gz?format=TEXT" | base64 -d | gunzip > "$SRC/$1.part"
    mv "$SRC/$1.part" "$SRC/$1"
}
aosp en_US_wordlist.combined   # the base: every word keeps AOSP's frequency
aosp en_wordlist.combined      # only used to copy capitals ("I'm", "London") onto wordfreq's lower-case words

# WHY a private venv (never a system-wide pip install), and a pinned version: the same wordfreq data gives the
# same dictionary every time, so a scoreboard number stays tied to one exact file.
VENV="$SRC/.venv"
if [ ! -x "$VENV/bin/python" ]; then
    if command -v uv >/dev/null; then
        uv venv -q "$VENV" && VIRTUAL_ENV="$VENV" uv pip install -q wordfreq==3.1.1
    else
        python3 -m venv "$VENV" && "$VENV/bin/pip" install -q wordfreq==3.1.1
    fi
fi

mkdir -p "$A/dict"
"$VENV/bin/python" -I "$HERE/wordfreq_combined.py" --out "$OUT.part" --max 250000 --typo-ratio 20 \
    --base "$SRC/en_US_wordlist.combined" --casing "$SRC/en_wordlist.combined"
mv "$OUT.part" "$OUT"
