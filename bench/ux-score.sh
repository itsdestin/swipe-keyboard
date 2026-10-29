#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# bench/ux-score.sh — the user-experience scoreboard: what a typist feels (docs/methods.md, "What a typist feels").
#   Swiping: for EVERY swipe, did it need no tap (first guess right), one tap (the word on the bar right after the
#     swipe), two taps (tapping the word later, with the swipe still remembered), or a retype (split: the word is not
#     in the word list / it is)? Plus taps per swiped word, a retype counted as the word's letters + 1. Replayed by
#     build/tools/swipekb-swap-bench through bench/swap-score.sh (the same sets, files and flags as the swap
#     scoreboard), for today's 0.5.0 keyboard (--mode baseline) and the tap-to-swap keyboard (--mode new).
#   Typing: each test sentence typed letter by letter on the keyboard's bar (build/tools/swipekb-typing-bench): the share
#     of taps saved against typing every letter plus a space, and when the bar offered the word.
# Appends rows to bench/ux-scoreboard.tsv and prints a one-screen summary.
# Usage: bench/ux-score.sh [--label TEXT] [--swipe-sets "bench-2000 ..."] [--modes "baseline new"] [--limit N]
#                          [--typing-langs "en es fr de"] [--sentences N] [--jobs N]
#        bench/ux-score.sh --recordings FILE --language L [--locale L_C] [--modes "baseline new"]
#   --swipe-sets  default: bench-2000 test-extra-10000 heliboard-es heliboard-fr heliboard-de heliboard-en ("" = none)
#   --typing-langs default: en es fr de ("" = none); sets everyday, web, tatoeba of each
#   --sentences   test sentences per typing set (default 300, the typing scoreboard's; WHY not more: sentences 1000 and
#                 beyond of these sets are used for blend tuning)
#   --jobs        programs run at once (default 4; WHY: the machine is a tablet in daily use)
#   --recordings  the owner's own opt-in recordings (traces/swipes.jsonl). Read only; prints counts to the screen and
#                 writes NO row (personal data never goes into the repository).
# Env: SWIPEKB_ASSETS, SWIPEKB_EVAL, HELI as bench/swap-score.sh; SWIPEKB_TESTSETS (folder of LANG/test/SET.txt, default
#      ~/.cache/swipe-keyboard-train), SWIPEKB_TYPING_BENCH, SWIPEKB_SWAP_BENCH, UX_SCOREBOARD (output file).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="${SWIPEKB_ASSETS:-$ROOT/assets}"
L="$A/language-files"
TESTSETS="${SWIPEKB_TESTSETS:-$HOME/.cache/swipe-keyboard-train}"
TYPING="${SWIPEKB_TYPING_BENCH:-$ROOT/build/tools/swipekb-typing-bench}"
SWAPBENCH="${SWIPEKB_SWAP_BENCH:-$ROOT/build/tools/swipekb-swap-bench}"
BOARD="${UX_SCOREBOARD:-$ROOT/bench/ux-scoreboard.tsv}"
usage='usage: bench/ux-score.sh [--label TEXT] [--swipe-sets "..."] [--modes "baseline new"] [--limit N] [--typing-langs "..."] [--sentences N] [--jobs N]
       bench/ux-score.sh --recordings FILE --language L [--locale L_C] [--modes "baseline new"]'
label=manual sets="bench-2000 test-extra-10000 heliboard-es heliboard-fr heliboard-de heliboard-en" modes="baseline new"
langs="en es fr de" n=300 jobs=4 limit="" recordings="" rec_lang="" rec_locale=""
while [ $# -gt 0 ]; do
    case "$1" in
        --label) label="$2"; shift 2 ;;
        --swipe-sets) sets="$2"; shift 2 ;;
        --modes) modes="$2"; shift 2 ;;
        --limit) limit="$2"; shift 2 ;;
        --typing-langs) langs="$2"; shift 2 ;;
        --sentences) n="$2"; shift 2 ;;
        --jobs) jobs="$2"; shift 2 ;;
        --recordings) recordings="$2"; shift 2 ;;
        --language) rec_lang="$2"; shift 2 ;;
        --locale) rec_locale="$2"; shift 2 ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
done
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# Swipe summary from one bench output: "label right0% right1% fixed2% notinlist% inlist% avgtaps swipes".
ux_line() {
    sed -nE 's/^ux: (.*)/\1/p' "$1" | awk '{ for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
        s = v["swipes"]; if (s == 0) s = 1
        printf "%.2f %.2f %.2f %.2f %.2f %.3f %d\n", 100*v["right0"]/s, 100*v["right1"]/s, 100*v["fixed2"]/s,
               100*v["retype_notinlist"]/s, 100*v["retype_inlist"]/s, v["taps"]/s, v["swipes"] }'
}
swipe_header() {
    printf '%-26s %-8s %7s %7s %7s %10s %8s %9s %7s\n' "Swiping (every swipe)" mode "0 taps" "1 tap" "2 taps" \
        "retype:not" "retype:in" "avg taps" swipes
    printf '%-26s %-8s %7s %7s %7s %10s %8s %9s %7s\n' "" "" "" "" "" "in list" "list" "per word" ""
}

# --- the owner's recordings: screen only ------------------------------------------------------------------------------
if [ -n "$recordings" ]; then
    [ -n "$rec_lang" ] || { echo "ux-score.sh: --recordings needs --language" >&2; exit 2; }
    [ -x "$SWAPBENCH" ] || { echo "ux-score.sh: $SWAPBENCH not built" >&2; exit 2; }
    dict="$L/$rec_lang.combined"; [ "$rec_lang" = en ] && dict="$L/en_swipe.combined"
    swipe_header
    for m in $modes; do
        "$SWAPBENCH" --mode "$m" --dict "$dict" --model "$L/swipe-model.bin" --ngram "$L/$rec_lang.ngram" \
            --vocab "$L/$rec_lang.vocab.tsv" --nwp "$L/$rec_lang.nwp.bin" --language "$rec_lang" \
            --locale "${rec_locale:-$rec_lang}" --context --recordings "$recordings" > "$tmp/rec-$m" 2>&1 \
            || { echo "ux-score.sh: swipekb-swap-bench failed:" >&2; tail -5 "$tmp/rec-$m" >&2; exit 1; }
        read -r r0 r1 f2 ni ii taps sw < <(ux_line "$tmp/rec-$m")
        printf '%-26s %-8s %6s%% %6s%% %6s%% %9s%% %7s%% %9s %7s\n' "your recordings (replayed)" "$m" "$r0" "$r1" "$f2" "$ni" "$ii" "$taps" "$sw"
    done
    # What happened on the keyboard at the time, from the outcome lines (counts only).
    sed -nE 's/^recorded: (.*)/\1/p' "$tmp/rec-$(set -- $modes; echo "$1")" | awk '{ for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
        s = v["swipes"]; d = s ? s : 1
        printf "\nAs recorded (%d swipes): no tap (kept, assumed right) %.2f%%, one tap (picked on the bar) %.2f%%,\n", s, 100*v["kept"]/d, 100*v["picked"]/d
        printf "  deleted (erased, word unknown) %.2f%%, no outcome (swapped later, moved away...) %.2f%%, nothing typed %.2f%%.\n", 100*v["erased"]/d, 100*v["unknown"]/d, 100*v["empty"]/d
        printf "Replayed: only picked and kept swipes have a known word, so the replay leaves out the deleted ones (hardest).\n" }'
    echo "(Recordings: nothing was written; these lines are not saved anywhere.)"
    exit 0
fi

# --- jobs ---------------------------------------------------------------------------------------------------------------
: > "$tmp/jobs"
for s in $sets; do
    for m in $modes; do
        lim=""; [ -n "$limit" ] && lim="--limit $limit"
        # WHY through swap-score.sh: it holds each set's files and flags; its swap rows go to a scratch file.
        printf 'SWAP_SCOREBOARD=%q SWAP_BENCH_OUT=%q nice -n 10 %q --set %q --mode %q --label ux %s > /dev/null 2> %q\n' \
            "$tmp/swap-$s-$m.tsv" "$tmp/swipe-$s-$m" "$ROOT/bench/swap-score.sh" "$s" "$m" "$lim" "$tmp/swipe-$s-$m.err" >> "$tmp/jobs"
    done
done
for l in $langs; do
    dict="$L/$l.combined"; [ "$l" = en ] && dict="$L/en_swipe.combined"
    case "$l" in es) loc=es_ES ;; fr) loc=fr_FR ;; de) loc=de_DE ;; *) loc=en_US ;; esac
    for set in everyday web tatoeba; do
        f="$TESTSETS/$l/test/$set.txt"
        [ -s "$f" ] || { echo "ux-score.sh: missing $f" >&2; exit 2; }
        printf 'nice -n 10 %q --dict %q --ngram %q --vocab %q --nwp %q --language %q --locale %q --sentences %q --limit %q > %q 2>&1\n' \
            "$TYPING" "$dict" "$L/$l.ngram" "$L/$l.vocab.tsv" "$L/$l.nwp.bin" "$l" "$loc" "$f" "$n" "$tmp/typing-$l-$set" >> "$tmp/jobs"
    done
done
[ -n "$sets" ] && { [ -x "$SWAPBENCH" ] || { echo "ux-score.sh: $SWAPBENCH not built" >&2; exit 2; }; }
[ -n "$langs" ] && { [ -x "$TYPING" ] || { echo "ux-score.sh: $TYPING not built" >&2; exit 2; }; }
echo "ux-score.sh: $(wc -l < "$tmp/jobs") runs, $jobs at a time (minutes; test-extra-10000 is the longest)" >&2
failed=0
xargs -P "$jobs" -I{} -d '\n' bash -c {} < "$tmp/jobs" || failed=1
if [ "$failed" = 1 ]; then
    for e in "$tmp"/*.err; do [ -s "$e" ] && { echo "== $e" >&2; tail -5 "$e" >&2; }; done
    for t in "$tmp"/typing-*; do grep -q '^typing: ' "$t" || { echo "== $t" >&2; tail -5 "$t" >&2; }; done
    echo "ux-score.sh: a run failed" >&2; exit 1
fi

# --- rows ---------------------------------------------------------------------------------------------------------------
commit=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo none)
# WHY a "-dirty" mark: a row measured with uncommitted changes to tracked files is not what that commit holds.
if [ "$commit" != none ] && [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no -- . ':(exclude)bench/*.tsv' 2>/dev/null)" ]; then
    commit="$commit-dirty"
fi
day=$(date +%F)
if [ ! -f "$BOARD" ]; then
    cat > "$BOARD" <<'EOF'
# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
# SPDX-License-Identifier: MIT
# User-experience scoreboard ("what a typist feels", docs/methods.md), written by bench/ux-score.sh. One row per measure.
# kind swipe: set = bench-2000 / test-extra-10000 (real English swipes, FUTO test split, with their sentences; never
#   tuned on) or heliboard-LANG (HeliBoard Gesture Data, CC BY-SA, test only, single words with no sentence).
#   mode: baseline = today's 0.5.0 keyboard (WordSwap::build), new = the tap-to-swap keyboard (card #500).
#   n = swipes counted (swipes with no layout file are left out). Every swipe is in exactly one of:
#   right0 = first guess right (no tap); right1 = the word was on the bar right after the swipe (one tap);
#   fixed2 = it was in the list on tapping the word later, the swipe still remembered (two taps);
#   retype_notinlist / retype_inlist = none of these: deleted and typed again; the word is not / is in the word list.
#   avg_taps = taps after the swipe per swiped word, a retype counted as the word's letters + 1 (one delete).
#   Values are percent of n, except avg_taps.
# kind typing: set = LANG-everyday / LANG-web / LANG-tatoeba (the first `sentences` sentences of each next-word test set;
#   web and tatoeba are partly home ground for models trained on web text and on the other nine tenths of Tatoeba).
#   mode = -; n = words typed (numbers and punctuation left out). Each sentence is typed letter by letter on the
#   keyboard's bar (tools/swipekb-typing-bench); a word on the bar is tapped (one tap, the space comes with it).
#   saved = percent of taps saved against every letter plus a space; bar0 / bar1 / bar2 / bar3plus = percent of words
#   taken from the bar before any letter / after 1 / 2 / 3 or more letters; typedout = percent typed to the end.
date	commit	label	kind	set	mode	n	measure	value
EOF
fi
rows="$tmp/rows"; : > "$rows"
for s in $sets; do
    for m in $modes; do
        out="$tmp/swipe-$s-$m"
        grep -q '^ux: ' "$out" || { echo "ux-score.sh: no ux line for $s $m" >&2; exit 1; }
        read -r r0 r1 f2 ni ii taps sw < <(ux_line "$out")
        for kv in "right0 $r0" "right1 $r1" "fixed2 $f2" "retype_notinlist $ni" "retype_inlist $ii" "avg_taps $taps"; do
            set -- $kv
            printf '%s\t%s\t%s\tswipe\t%s\t%s\t%s\t%s\t%s\n' "$day" "$commit" "$label" "$s" "$m" "$sw" "$1" "$2" >> "$rows"
        done
    done
done
for l in $langs; do
    for set in everyday web tatoeba; do
        out="$tmp/typing-$l-$set"
        sed -nE 's/^typing: (.*)/\1/p' "$out" | awk -v d="$day" -v c="$commit" -v lab="$label" -v s="$l-$set" '{
            for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
            w = v["words"] ? v["words"] : 1; b = v["baseline"] ? v["baseline"] : 1
            printf "%s\t%s\t%s\ttyping\t%s\t-\t%d\tsaved\t%.2f\n", d, c, lab, s, v["words"], 100*(v["baseline"]-v["taps"])/b
            split("bar0 bar1 bar2 bar3plus typedout", ks, " ")
            for (k = 1; k <= 5; k++) printf "%s\t%s\t%s\ttyping\t%s\t-\t%d\t%s\t%.2f\n", d, c, lab, s, v["words"], ks[k], 100*v[ks[k]]/w }' >> "$rows"
    done
done
cat "$rows" >> "$BOARD"

# --- summary ------------------------------------------------------------------------------------------------------------
echo
echo "What a typist feels — $label ($commit)"
if [ -n "$sets" ]; then
    swipe_header
    for s in $sets; do
        for m in $modes; do
            read -r r0 r1 f2 ni ii taps sw < <(ux_line "$tmp/swipe-$s-$m")
            printf '%-26s %-8s %6s%% %6s%% %6s%% %9s%% %7s%% %9s %7s\n' "$s" "$m" "$r0" "$r1" "$f2" "$ni" "$ii" "$taps" "$sw"
        done
    done
    echo "  (a retype counts the word's letters + 1; HeliBoard sets have no sentence around the word)"
fi
if [ -n "$langs" ]; then
    echo
    printf '%-26s %8s   %-34s %9s %6s\n' "Typing letter by letter" "taps" "word taken from the bar after" "typed to" words
    printf '%-26s %8s   %8s %8s %8s %8s %9s\n' "" saved "0 lett." "1" "2" "3+" "the end"
    awk -F'\t' -v c="$commit" -v lab="$label" '$2 == c && $3 == lab && $4 == "typing" { v[$5, $8] = $9; n[$5] = $7; if (!($5 in seen)) { seen[$5] = 1; order[++k] = $5 } }
        END { for (i = 1; i <= k; i++) { s = order[i]
            printf "%-26s %7s%%   %7s%% %7s%% %7s%% %7s%% %8s%% %6s\n", s, v[s, "saved"], v[s, "bar0"], v[s, "bar1"], v[s, "bar2"], v[s, "bar3plus"], v[s, "typedout"], n[s] } }' "$rows"
    echo "  (web and tatoeba are partly home ground; punctuation and numbers are left out of both counts)"
fi
