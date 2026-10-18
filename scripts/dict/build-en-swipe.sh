#!/usr/bin/env bash
# build-en-swipe.sh — builds the shipped dictionary, assets/dict/en_swipe.combined:
#   AOSP's US English word list (Apache-2.0), every word and frequency kept, plus words from the `wordfreq`
#   package's English list (data CC-BY-SA 4.0) that AOSP lacks, minus likely misspellings, up to 250k words.
# WHY this mix: on the scoreboard it raised first-guess from 92.50% to 93.32% and top-3 from 95.43% to 96.56%
# (most misses were words AOSP simply does not have), without changing any result on the owner's own swipes.
# See .superpowers/sdd/dict-experiment-report.md for every candidate that was measured.
# Also (quick-fix round): AOSP's "not a word" lines ("im", "alot") are dropped, and in apostrophe pairs the
# spelling AOSP prefers gets the higher frequency ("let's" over "lets", "its" over "it's"); see
# order_apostrophe_twins in wordfreq_combined.py. To rebuild, delete the old OUT file first (it is skipped if present).
# Usage: scripts/dict/build-en-swipe.sh [ASSETS_DIR]   (skips the build if the output already exists)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
A="${1:-$HERE/../../assets}"
SRC="$A/dict-src"
OUT="$A/dict/en_swipe.combined"
# A fixed dictionary header makes package output independent of the build date.
export SOURCE_DATE_EPOCH=0
valid_dictionary() {
    python3 - "$HERE/../../packaging/assets.lock.json" "$1" <<'PY'
import hashlib, json, pathlib, sys
expected = json.load(open(sys.argv[1]))['dictionary_sha256']
path = pathlib.Path(sys.argv[2])
sys.exit(0 if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == expected else 1)
PY
}
valid_dictionary "$OUT" && exit 0
python3 "$HERE/../fetch-assets.py" --assets "$A"

aosp() { # Decode checksum-verified, immutable AOSP input.
    base64 -d "$SRC/$1.gz.b64" | gunzip > "$SRC/$1.part"
    mv "$SRC/$1.part" "$SRC/$1"
}
aosp en_US_wordlist.combined   # the base: every word keeps AOSP's frequency
aosp en_wordlist.combined      # only used to copy capitals ("I'm", "London") onto wordfreq's lower-case words

# WHY a private venv (never a system-wide pip install), and a pinned version: the same wordfreq data gives the
# same dictionary every time, so a scoreboard number stays tied to one exact file.
VENV="$SRC/.venv"
python3 "$HERE/../provision-build-deps.py" dictionary "$VENV"

mkdir -p "$A/dict"
"$VENV/bin/python" -I "$HERE/wordfreq_combined.py" --out "$OUT.part" --max 250000 --typo-ratio 20 \
    --base "$SRC/en_US_wordlist.combined" --casing "$SRC/en_wordlist.combined"
valid_dictionary "$OUT.part" || { echo 'Generated dictionary checksum mismatch' >&2; exit 1; }
mv "$OUT.part" "$OUT"
