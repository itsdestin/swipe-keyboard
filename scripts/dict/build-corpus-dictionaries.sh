#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# build-corpus-dictionaries.sh — the four swipe dictionaries, counted from openly licensed text (card #170, Phase 1).
#   es/fr/de: the 100k most frequent words of FineWeb-2 + Tatoeba, junk-filtered (corpus_dictionary.py).
#   en:       AOSP's US English list (Apache-2.0) kept exactly, plus FineWeb words AOSP lacks, up to 250k words.
# No wordfreq anywhere, so no CC BY-SA term applies. Replaces build-en-swipe.sh + language_pack.py's wheel path
# once the new files are published and pinned.
# Input:  COUNTS_DIR holding en/es/fr/de.counts.tsv (scripts/lm/count-words.py; these are the pinned inputs)
#         AOSP_DIR holding en_US_wordlist.combined and en_wordlist.combined (decoded as build-en-swipe.sh does)
# Output: OUT_DIR/{es,fr,de}.combined and OUT_DIR/en_swipe.combined
# Usage:  scripts/dict/build-corpus-dictionaries.sh COUNTS_DIR AOSP_DIR OUT_DIR
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
COUNTS="${1:?counts dir}"; AOSP="${2:?AOSP dir}"; OUT="${3:?output dir}"
# A fixed dictionary header makes the output independent of the build date.
export SOURCE_DATE_EPOCH=0
mkdir -p "$OUT"
PY=(python3 -I "$HERE/corpus_dictionary.py")
for l in es fr de en; do
    foreign=()
    # WHY every other language: a word far more typical of another language's text is page contamination.
    for o in en es fr de; do [ "$o" = "$l" ] || foreign+=("$COUNTS/$o.counts.tsv"); done
    if [ "$l" = en ]; then
        "${PY[@]}" --lang en --counts "$COUNTS/en.counts.tsv" --out "$OUT/en_swipe.combined.part" --max 250000 \
            --base "$AOSP/en_US_wordlist.combined" --casing "$AOSP/en_wordlist.combined" --foreign "${foreign[@]}"
        mv "$OUT/en_swipe.combined.part" "$OUT/en_swipe.combined"
    else
        "${PY[@]}" --lang "$l" --counts "$COUNTS/$l.counts.tsv" --out "$OUT/$l.combined.part" --max 100000 \
            --foreign "${foreign[@]}"
        mv "$OUT/$l.combined.part" "$OUT/$l.combined"
    fi
done
