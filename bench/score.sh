#!/usr/bin/env bash
# bench/score.sh — the scoreboard. Replays a fixed slice of REAL human swipes (FUTO's test split, never
# trained on) through FUTO's infer_jsonl and records first-guess % and top-3 % in bench/history.tsv.
# Usage: bench/score.sh [--label TEXT] [--check]
#   --check  exit 1 if either score is below the best earlier row with the same slice size — the design's
#            rule that no change may lower the score.
# Env: INFER_JSONL, SWIPEKB_ASSETS (assets/), BENCH_N (2000), BENCH_HISTORY (bench/history.tsv)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="${SWIPEKB_ASSETS:-$ROOT/assets}"
N="${BENCH_N:-2000}"
INFER="${INFER_JSONL:-$ROOT/third_party/swipe-library/build/infer_jsonl}"
HIST="${BENCH_HISTORY:-$ROOT/bench/history.tsv}"
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

out=$("$INFER" --encoder "$A/models/honorable_sturgeon/model_fp32.pte" \
        --decoder "$A/models/magic_macaw/model_fp32.pte" \
        --lm-model "$A/models/hungry_jellyfish/context_lm.pte" --lm-vocab "$A/models/hungry_jellyfish/vocab.txt" \
        --vocab "$A/dict/en_US_wordlist.combined" \
        --layout "$ROOT/third_party/swipe-library/models/layouts/en_qwerty.json" \
        --jsonl "$slice" --top-k 3 --beam-width 100 --expansion 2>&1) || true
top1=$(awk '/^  top-1:/ { gsub("%", "", $NF); print $NF }' <<<"$out")
# WHY "top-4": infer_jsonl always labels its "in the top-k" line top-4; with --top-k 3 it is the top-3 score.
top3=$(awk '/^  top-4:/ { gsub("%", "", $NF); print $NF }' <<<"$out")
us=$(awk '/^  total:/ { print $NF }' <<<"$out")
if [ -z "$top1" ] || [ -z "$top3" ]; then
    { echo "score.sh: could not read infer_jsonl output:"; tail -20 <<<"$out"; } >&2
    exit 1
fi

[ -f "$HIST" ] || printf 'date\tcommit\tlabel\tn\ttop1\ttop3\tavg_us\n' > "$HIST"
read -r best1 best3 < <(awk -F'\t' -v n="$N" 'NR > 1 && $4 == n { if ($5 > b1) b1 = $5; if ($6 > b3) b3 = $6 }
                                             END { print (b1 == "" ? -1 : b1), (b3 == "" ? -1 : b3) }' "$HIST")
commit=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo none)
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%F)" "$commit" "$label" "$N" "$top1" "$top3" "${us:-?}" >> "$HIST"
echo "first guess right: $top1%   right word in top 3: $top3%   ($N real swipes, avg ${us:-?} µs)"

if [ "$check" = 1 ] && awk -v a="$top1" -v b="$best1" -v c="$top3" -v d="$best3" 'BEGIN { exit !(a < b || c < d) }'; then
    echo "FAIL: below the best so far (first guess $best1%, top 3 $best3%)"
    exit 1
fi
