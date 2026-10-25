#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# bench/swap-score.sh — the swap scoreboard (card #500). Replays real recorded swipes through build/tools/swipekb-swap-bench
# and records, for the swipes whose first guess was wrong, how often the right word is the first swap option and how
# often it is among three — right after the swipe, on tapping back with the swipe's guesses, and on tapping back without
# them — plus junk options and the time to build a tap-back list. One row per measure in bench/swap-scoreboard.tsv.
# Usage: bench/swap-score.sh --set NAME [--mode baseline|new] [--label TEXT] [--limit N] [--check BASELINE_LABEL]
#   --set    bench-2000 | test-extra-10000 (real English swipes, public MIT test split, never tuned on)
#            | heliboard-es|fr|de|en (HeliBoard Gesture Data, CC BY-SA: test only, no sentences, per-swipe key boxes)
#            | trv8k (FUTO English TUNING slice: its rows are labelled "[tuning]" and are never quoted as results)
#   --check  exit 1 when any in3 is below the newest row with that label for the same set and measure.
# Env: SWIPEKB_ASSETS (assets/; language files in its language-files/), SWIPEKB_EVAL (assets/eval),
#      HELI (~/.cache/swipe-keyboard-train/heliboard/out), SWIPEKB_TUNE_EN (~/.cache/swipe-keyboard-train/shape/futo/trv8k.jsonl),
#      SWIPEKB_LAYOUT (scripts/swipe/layouts/qwerty.tsv), SWAP_SCOREBOARD (bench/swap-scoreboard.tsv),
#      SWIPEKB_SWAP_BENCH (build/tools/swipekb-swap-bench), SWIPEKB_SWAP_FLAGS (extra flags, e.g. "--params FILE"; say so in the label)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="${SWIPEKB_ASSETS:-$ROOT/assets}"
L="$A/language-files"
EVAL="${SWIPEKB_EVAL:-$A/eval}"
HELI="${HELI:-$HOME/.cache/swipe-keyboard-train/heliboard/out}"
TUNE_EN="${SWIPEKB_TUNE_EN:-$HOME/.cache/swipe-keyboard-train/shape/futo/trv8k.jsonl}"
LAYOUT="${SWIPEKB_LAYOUT:-$ROOT/scripts/swipe/layouts/qwerty.tsv}"
BOARD="${SWAP_SCOREBOARD:-$ROOT/bench/swap-scoreboard.tsv}"
BENCH="${SWIPEKB_SWAP_BENCH:-$ROOT/build/tools/swipekb-swap-bench}"
usage="usage: bench/swap-score.sh --set NAME [--mode baseline|new] [--label TEXT] [--limit N] [--check BASELINE_LABEL]"
set_name="" mode=baseline label=manual check="" limit=""
while [ $# -gt 0 ]; do
    case "$1" in
        --set) set_name="$2"; shift 2 ;;
        --mode) mode="$2"; shift 2 ;;
        --label) label="$2"; shift 2 ;;
        --limit) limit="$2"; shift 2 ;;
        --check) check="$2"; shift 2 ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
done

# Each set: its swipes, its language files, its keys, how words are compared, and whether the sentence is known.
case "$set_name" in
    bench-2000|test-extra-10000|trv8k)
        lang=en; locale=en_US
        input="$EVAL/$set_name.jsonl"; [ "$set_name" = trv8k ] && input="$TUNE_EN"
        where=(--layout "$LAYOUT" --match ascii --context) ;;
    heliboard-es|heliboard-fr|heliboard-de|heliboard-en)
        lang="${set_name#heliboard-}"
        case "$lang" in es) locale=es_ES ;; fr) locale=fr_FR ;; de) locale=de_DE ;; en) locale=en_US ;; esac
        input="$HELI/$lang/test.jsonl"
        # WHY no --context: HeliBoard records hold single words, no sentence.
        where=(--layouts-dir "$HELI/layouts" --match lookup) ;;
    *) echo "$usage" >&2; exit 2 ;;
esac
dict="$L/$lang.combined"; [ "$lang" = en ] && dict="$L/en_swipe.combined"
# WHY "[tuning]" forced on: tuning rows must never be quoted as results.
if [ "$set_name" = trv8k ] && [[ "$label" != *tuning* ]]; then label="[tuning] $label"; fi
[ -s "$input" ] || { echo "swap-score.sh: missing swipes $input" >&2; exit 1; }

lim=(); [ -n "$limit" ] && lim=(--limit "$limit")
# shellcheck disable=SC2086  # WHY unquoted: SWIPEKB_SWAP_FLAGS holds several flags.
out=$("$BENCH" --mode "$mode" --dict "$dict" --model "$L/swipe-model.bin" --ngram "$L/$lang.ngram" \
      --vocab "$L/$lang.vocab.tsv" --nwp "$L/$lang.nwp.bin" --language "$lang" --locale "$locale" "${where[@]}" \
      "${lim[@]}" ${SWIPEKB_SWAP_FLAGS:-} "$input" 2>&1) || { echo "swap-score.sh: swipekb-swap-bench failed:" >&2; tail -20 <<<"$out" >&2; exit 1; }
n=$(sed -nE 's/^setup: .* n=([0-9]+).*/\1/p' <<<"$out")
[ -n "$n" ] || { echo "swap-score.sh: could not read swipekb-swap-bench output:" >&2; tail -20 <<<"$out" >&2; exit 1; }

if [ ! -f "$BOARD" ]; then
    cat > "$BOARD" <<'EOF'
# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
# SPDX-License-Identifier: MIT
# Swap options scoreboard (card #500), written by bench/swap-score.sh. One row per measure per run.
# set: bench-2000 / test-extra-10000 = real English swipes (public MIT test split, never trained or tuned on);
#   heliboard-LANG = HeliBoard Gesture Data (CC BY-SA, test only, no sentences); trv8k = FUTO English TUNING slice
#   (labels start "[tuning]"; never quote those rows as results).
# mode: baseline = today's keyboard (the shipped WordSwap::build); new = the improved order.
# n = swipes replayed. measure: after-swipe = the guess bar's other places right after the swipe; tapback-trace = the
#   list on tapping back while the swipe's guesses are remembered; tapback-notrace = the same without them.
#   For these three, only swipes whose FIRST guess was wrong count (their number is "cases" in extra); first = the right
#   word is the first option, in3 = among the first three (best-first order, not the bar's middle-first layout).
# junk: first guess right; extra = share of the bar's other places that are stretched, rare (frequency under 80 of
#   255) or rare names (capitalised, under 120) — the swipe scoreboard's rule. speed: extra = ms to build one tap-back list.
date	commit	label	set	mode	n	measure	first	in3	extra
EOF
fi
commit=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo none)
day=$(date +%F)
awk -v d="$day" -v c="$commit" -v l="$label" -v s="$set_name" -v m="$mode" -v n="$n" '
    /^(after-swipe|tapback-trace|tapback-notrace): / { f=$5; i=$7; gsub("%","",f); gsub("%","",i)
        printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\tcases %s\n", d, c, l, s, m, n, substr($1,1,length($1)-1), f, i, $3 }
    /^junk: / { j=$7; gsub("%","",j); printf "%s\t%s\t%s\t%s\t%s\t%s\tjunk\t\t\t%s\n", d, c, l, s, m, n, j }
    /^speed: / { printf "%s\t%s\t%s\t%s\t%s\t%s\tspeed\t\t\t%s\n", d, c, l, s, m, n, $2 }' <<<"$out" | tee -a "$BOARD"

if [ -n "$check" ]; then
    # WHY the newest row with that label: a baseline re-recorded later (same label) replaces the older one.
    if ! awk -F'\t' -v lab="$check" -v s="$set_name" -v c="$commit" -v d="$day" -v l="$label" '
        !/^#/ && $1 != "date" && $3 == lab && $4 == s && $9 != "" { base[$7] = $9 }
        !/^#/ && $3 == l && $4 == s && $9 != "" { now[$7] = $9 }
        END { bad = 0; seen = 0
              for (k in base) if (k in now) { seen = 1; if (now[k] + 0 < base[k] + 0) { printf "FAIL: %s in3 %s%% below %s%%\n", k, now[k], base[k]; bad = 1 } }
              if (!seen) { print "FAIL: no row labelled \"" lab "\" for " s; bad = 1 }
              exit bad }' "$BOARD"; then exit 1; fi
fi
