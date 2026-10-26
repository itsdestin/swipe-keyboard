#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# bench/swap-score.sh — the swap scoreboard (card #500). Replays real recorded swipes through build/tools/swipekb-swap-bench
# and records, for the swipes whose first guess was wrong, how often the right word is the first swap option and how
# often it is among three — right after the swipe, on tapping back with the swipe's guesses, and on tapping back without
# them — plus junk options (in the bar after a swipe and in tap-back lists) and the time to build a tap-back list and
# the bar after a swipe. One row per measure in bench/swap-scoreboard.tsv.
# Usage: bench/swap-score.sh --set NAME [--mode baseline|new] [--label TEXT] [--limit N] [--check BASELINE_LABEL]
#   --set    bench-2000 | test-extra-10000 (real English swipes, public MIT test split, never tuned on)
#            | heliboard-es|fr|de|en (HeliBoard Gesture Data, CC BY-SA: test only, no sentences, per-swipe key boxes)
#            | trv8k (FUTO English TUNING slice: its rows are labelled "[tuning]" and are never quoted as results)
#            | trv8k-nocontext (the same tuning swipes with their sentence removed, as HeliBoard records and the first
#              word of a message look; "[tuning]" too)
#   --check  exit 1 against the newest rows with that label for the same set and n (any mode, so a new-mode run is
#            judged against the baseline): any first or in3 lower, junk in the bar after a swipe more than 1 point
#            higher, junk in a tap-back list more than 3 points higher, or a different number of skipped swipes.
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
    trv8k-nocontext)
        # WHY its own set: the no-sentence weights (SwapParams ncPath/ncBefore/ncNeural) are tuned on these swipes.
        lang=en; locale=en_US; input="$TUNE_EN"
        where=(--layout "$LAYOUT" --match ascii --no-context) ;;
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
if [[ "$set_name" = trv8k* ]] && [[ "$label" != *tuning* ]]; then label="[tuning] $label"; fi
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
# junk: first guess right; extra = share of the bar's other places that are stretched, rare (not among the language's
#   top 31,456 words, German 48,000; for English that is frequency under 80 of 255) or rare names (capitalised, not
#   among the top 4,388; English: under 120) — the swipe scoreboard's rule (tools/benchdata.h JunkRule, from
#   commit 43b9e56; earlier rows measured es/fr/de junk as "under 80 / under 120"; English is the same either way).
#   junk-tapback-trace / junk-tapback-notrace: the same share among the three tap-back options (with / without the
#   swipe's guesses).
# speed: extra = ms to build one tap-back list; speed-after-swipe (new mode): average ms to build the bar after a swipe
#   (input thread). skipped: swipes left out for want of a layout file.
date	commit	label	set	mode	n	measure	first	in3	extra
EOF
fi
# WHY exactly these lines: a changed bench output must stop the run instead of quietly writing a short scoreboard.
# Six measure lines (the three swap measures, the two junk lines, tap-back speed), plus the after-swipe list time in new mode.
rows=$(grep -cE '^(after-swipe|tapback-trace|tapback-notrace|junk|junk-tapback|speed): ' <<<"$out" || true)
want=6; [ "$mode" = new ] && want=7 && rows=$((rows + $(grep -c '^after-swipe list time: ' <<<"$out" || true)))
[ "$rows" = "$want" ] || { echo "swap-score.sh: expected $want measure lines from swipekb-swap-bench, got $rows:" >&2; tail -20 <<<"$out" >&2; exit 1; }
# WHY recorded: swipes without a layout are left out of every measure, so the numbers cover fewer swipes than n says.
skipped=$(sed -nE 's/^\(([0-9]+) swipes had no readable layout.*/\1/p' <<<"$out")
commit=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo none)
# WHY a "-dirty" mark: a row measured with uncommitted changes to tracked files is not what that commit holds. The
# scoreboard itself is left out (every run appends to it).
if [ "$commit" != none ] && [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no -- . ':(exclude)bench/swap-scoreboard.tsv' 2>/dev/null)" ]; then
    commit="$commit-dirty"
fi
day=$(date +%F)
awk -v d="$day" -v c="$commit" -v l="$label" -v s="$set_name" -v m="$mode" -v n="$n" '
    /^(after-swipe|tapback-trace|tapback-notrace): / { f=$5; i=$7; gsub("%","",f); gsub("%","",i)
        printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\tcases %s\n", d, c, l, s, m, n, substr($1,1,length($1)-1), f, i, $3 }
    /^junk: / { j=$7; gsub("%","",j); printf "%s\t%s\t%s\t%s\t%s\t%s\tjunk\t\t\t%s\n", d, c, l, s, m, n, j }
    # WHY only when options were shown: a run with no tap-back lists would otherwise record a meaningless 0 %.
    /^junk-tapback: / { t=$6; u=$11; gsub("%","",t); gsub("%","",u)
        if ($4 + 0 > 0) printf "%s\t%s\t%s\t%s\t%s\t%s\tjunk-tapback-trace\t\t\t%s\n", d, c, l, s, m, n, t
        if ($9 + 0 > 0) printf "%s\t%s\t%s\t%s\t%s\t%s\tjunk-tapback-notrace\t\t\t%s\n", d, c, l, s, m, n, u }
    /^speed: / { printf "%s\t%s\t%s\t%s\t%s\t%s\tspeed\t\t\t%s\n", d, c, l, s, m, n, $2 }
    # WHY its own row: the bar after a swipe is built on the input thread after every swipe (budget 20 ms).
    /^after-swipe list time: / { printf "%s\t%s\t%s\t%s\t%s\t%s\tspeed-after-swipe\t\t\t%s\n", d, c, l, s, m, n, $5 }' <<<"$out" | tee -a "$BOARD"
if [ -n "$skipped" ]; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\tskipped\t\t\t%s\n' "$day" "$commit" "$label" "$set_name" "$mode" "$n" "$skipped" | tee -a "$BOARD"
fi

if [ -n "$check" ]; then
    # WHY the newest rows with that label: a baseline re-recorded later (same label) replaces the older one.
    # WHY same set and n but any mode: a 200-swipe run must not be judged against a 2000-swipe baseline, while the
    # point of a new-mode run is to be judged against today's keyboard (baseline mode).
    if ! awk -F'\t' -v lab="$check" -v s="$set_name" -v l="$label" -v mo="$mode" -v nn="$n" '
        !/^#/ && $1 != "date" && $4 == s && $6 == nn {
            if ($3 == lab) { seen = 1; bf[$7] = $8; bi[$7] = $9; bx[$7] = $10 }
            if ($3 == l && $5 == mo) { nf[$7] = $8; ni[$7] = $9; nx[$7] = $10 }
        }
        function worse(what, now, base, slack) { if (now - base > slack + 1e-9) { printf "FAIL: %s %s%% is more than %s points above %s%%\n", what, now, slack, base; bad = 1 } }
        END { bad = 0
              if (!seen) { print "FAIL: no row labelled \"" lab "\" for " s " with n=" nn; exit 1 }
              split("after-swipe tapback-trace tapback-notrace", ms, " ")
              for (i = 1; i <= 3; i++) { k = ms[i]
                  if (!(k in bi)) continue
                  if (!(k in ni)) { printf "FAIL: no %s row in this run\n", k; bad = 1; continue }
                  # WHY first and in3 apart: the first option is what one tap gives; never let in3 hide a worse first.
                  if (nf[k] + 0 < bf[k] + 0) { printf "FAIL: %s first %s%% below %s%%\n", k, nf[k], bf[k]; bad = 1 }
                  if (ni[k] + 0 < bi[k] + 0) { printf "FAIL: %s in3 %s%% below %s%%\n", k, ni[k], bi[k]; bad = 1 } }
              # Junk limits agreed for card #500: the bar after a swipe +1 point, tap-back lists +3 points.
              if ("junk" in bx && "junk" in nx) worse("junk", nx["junk"], bx["junk"], 1)
              split("junk-tapback-trace junk-tapback-notrace", js, " ")
              for (i = 1; i <= 2; i++) { k = js[i]
                  if (k in bx && k in nx) worse(k, nx[k], bx[k], 3)
                  else if (k in nx) printf "note: no %s row labelled \"%s\" to compare with (re-record it to check this)\n", k, lab }
              # WHY: swipes skipped for want of a layout change which swipes the numbers cover.
              if (nx["skipped"] + 0 != bx["skipped"] + 0) { printf "FAIL: %d swipes skipped, the baseline skipped %d\n", nx["skipped"], bx["skipped"]; bad = 1 }
              exit bad }' "$BOARD"; then exit 1; fi
fi
