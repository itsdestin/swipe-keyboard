#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# bench/score.sh — the swipe scoreboard. Replays a fixed slice of REAL human swipes (a public MIT-licensed test split,
# never trained on) through the keyboard's own engine (build/tools/swipekb-bench, swipekb::LanguageEngine) and records
# first-guess % and top-3 % in bench/history.tsv.
# Usage: bench/score.sh [--label TEXT] [--check]
#   --check  exit 1 if either score is below the best earlier [engine] row with the same slice size — the design's
#            rule that no change may lower the score.
# Env: SWIPEKB_ASSETS (assets/), SWIPEKB_DICT (assets/dict/en_swipe.combined), SWIPEKB_MODEL (the pinned swipe model in
#      assets/language-files/), SWIPEKB_LAYOUT (scripts/swipe/layouts/qwerty.tsv), BENCH_N (2000),
#      BENCH_HISTORY (bench/history.tsv), SWIPEKB_BENCH (build/tools/swipekb-bench),
#      SWIPEKB_BENCH_FLAGS (extra swipekb-bench flags, e.g. "--ngram FILE"; WHY: before/after and ablation rows without
#      editing code; say so in the label)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="${SWIPEKB_ASSETS:-$ROOT/assets}"
N="${BENCH_N:-2000}"
# WHY SWIPEKB_DICT: lets the scoreboard compare candidate dictionaries (scripts/dict/) without moving files;
# label each such run so history.tsv says which dictionary produced the score.
DICT="${SWIPEKB_DICT:-$A/dict/en_swipe.combined}"
MODEL="${SWIPEKB_MODEL:-$A/language-files/swipe-model.bin}"
LAYOUT="${SWIPEKB_LAYOUT:-$ROOT/scripts/swipe/layouts/qwerty.tsv}"
HIST="${BENCH_HISTORY:-$ROOT/bench/history.tsv}"
SWIPEKB_BENCH="${SWIPEKB_BENCH:-$ROOT/build/tools/swipekb-bench}"
label=manual check=0
while [ $# -gt 0 ]; do
    case "$1" in
        --label) label="$2"; shift 2 ;;
        --check) check=1; shift ;;
        *) echo "usage: bench/score.sh [--label TEXT] [--check]" >&2; exit 2 ;;
    esac
done

slice="$A/eval/bench-$N.jsonl"
[ -s "$slice" ] || python3 -I "$ROOT/bench/make-slice.py" "$A/eval/test.jsonl" "$N" "$slice"

# shellcheck disable=SC2086  # WHY unquoted: SWIPEKB_BENCH_FLAGS holds several flags.
out=$("$SWIPEKB_BENCH" --dict "$DICT" --model "$MODEL" --layout "$LAYOUT" ${SWIPEKB_BENCH_FLAGS:-} "$slice" 2>&1) || true
top1=$(awk '/^  top-1:/ { gsub("%", "", $NF); print $NF }' <<<"$out")
top3=$(awk '/^  top-3:/ { gsub("%", "", $NF); print $NF }' <<<"$out")
ms=$(sed -nE 's/.*\| ([0-9.]+) ms\/swipe.*/\1/p' <<<"$out")
if [ -z "$top1" ] || [ -z "$top3" ]; then
    { echo "score.sh: could not read swipekb-bench output:"; tail -20 <<<"$out"; } >&2
    exit 1
fi

# WHY an "[engine] " prefix: history.tsv also holds older rows of other engines (no prefix, "[recognizer]"); only rows
# of this engine are compared with each other, and the columns stay as they were (avg_us = microseconds per swipe).
label="[engine] $label"
us=$(awk -v m="${ms:-}" 'BEGIN { if (m == "") print "?"; else printf "%.1f", m * 1000 }')
[ -f "$HIST" ] || printf 'date\tcommit\tlabel\tn\ttop1\ttop3\tavg_us\n' > "$HIST"
read -r best1 best3 < <(awk -F'\t' -v n="$N" 'NR > 1 && $4 == n && $3 ~ /^\[engine\]/ { if ($5 > b1) b1 = $5; if ($6 > b3) b3 = $6 }
                                             END { print (b1 == "" ? -1 : b1), (b3 == "" ? -1 : b3) }' "$HIST")
commit=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo none)
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%F)" "$commit" "$label" "$N" "$top1" "$top3" "$us" >> "$HIST"
echo "first guess right: $top1%   right word in top 3: $top3%   ($N real swipes, avg $us µs)"
grep -E '^all ' <<<"$out" || true

if [ "$check" = 1 ] && awk -v a="$top1" -v b="$best1" -v c="$top3" -v d="$best3" 'BEGIN { exit !(a < b || c < d) }'; then
    echo "FAIL: below the best so far (first guess $best1%, top 3 $best3%)"
    exit 1
fi
