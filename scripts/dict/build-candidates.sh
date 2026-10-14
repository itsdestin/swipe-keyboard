#!/usr/bin/env bash
# build-candidates.sh — rebuilds every dictionary measured in the dictionary experiment, into
# assets/dict-candidates/ (git-ignored), so each scoreboard row labelled "dict: ..." can be reproduced:
#   SWIPEKB_DICT=assets/dict-candidates/<file> bench/score.sh --label "dict: ..."
# WHY keep losers buildable too: the numbers in bench/history.tsv are only trustworthy if anyone can remake them.
#   a   AOSP en_US (Apache-2.0)                        assets/dict-src/en_US_wordlist.combined
#   a2  AOSP en, US+GB spellings (Apache-2.0)          assets/dict-src/en_wordlist.combined
#   b   FUTO's en_w2 (AOSP en, abbreviations removed;  b_futo_en_w2.combined   personal measurement only:
#       FUTO publishes it without a license statement)
#   c   wordfreq only (CC-BY-SA 4.0)                   c_wordfreq_{150k,250k,300k}.combined, c_wordfreq_250k_notypo
#   d   AOSP en_US + wordfreq                          d_aosp_us_wordfreq_250k.combined, ..._{200,250,300}k_notypo
# Usage: scripts/dict/build-candidates.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
A="$HERE/../../assets"
C="$A/dict-candidates"
"$HERE/build-en-swipe.sh" "$A"   # fetches the AOSP sources and the wordfreq venv
SRC="$A/dict-src"
PY=("$SRC/.venv/bin/python" -I "$HERE/wordfreq_combined.py")
mkdir -p "$C"
[ -s "$C/b_futo_en_w2.combined" ] || curl -fsL -o "$C/b_futo_en_w2.combined" https://dl.keyboard.futo.org/en_w2.combined
for n in 150 250 300; do
    "${PY[@]}" --out "$C/c_wordfreq_${n}k.combined" --max ${n}000 --typo-ratio 0 \
        --casing "$SRC/en_US_wordlist.combined" "$SRC/en_wordlist.combined"
done
"${PY[@]}" --out "$C/c_wordfreq_250k_notypo.combined" --max 250000 \
    --casing "$SRC/en_US_wordlist.combined" "$SRC/en_wordlist.combined"
"${PY[@]}" --out "$C/d_aosp_us_wordfreq_250k.combined" --max 250000 --typo-ratio 0 \
    --base "$SRC/en_US_wordlist.combined" --casing "$SRC/en_wordlist.combined"
for n in 200 250 300; do
    "${PY[@]}" --out "$C/d_aosp_us_wordfreq_${n}k_notypo.combined" --max ${n}000 \
        --base "$SRC/en_US_wordlist.combined" --casing "$SRC/en_wordlist.combined"
done
