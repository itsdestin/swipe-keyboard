#!/usr/bin/env bash
# bench/scoreboard.sh — one table for every typing measure, every language (card #170 roadmap, Phase 0.4).
# Next word, completion after 1 and 3 letters (through the keyboard's own predictor code), the same against
# Gboard's recorded answers (bench/gboard/reference), speed and pack size; English swipe when the swipe bench
# is built. Appends to bench/scoreboard.tsv.
# Usage: bench/scoreboard.sh [--label TEXT] [--check] [--langs "es fr de"] [--sentences N]
#   --check      exit 1 if any accuracy measure fell more than 1 point below the previous run with the same
#                --sentences (WHY 1 point: ~2,000 scored words per measure; smaller moves are noise).
#   --sentences  test sentences per set (default 300; WHY not all 2,000: keeps a full run to a few minutes).
# Env: SWIPEKB_NEXTWORD_EVAL (the swipekb-nextword-eval program), SWIPEKB_TESTSETS (folder holding
#      LANG/test/{everyday,web,tatoeba}.txt from scripts/lm/make-nextword-testsets.py),
#      SWIPEKB_PACK_<lang> (a folder with context.ngram + vocab.tsv to score instead of the installed pack),
#      SWIPEKB_PACKS (installed packs, default /usr/share/swipe-keyboard/language-packs), SWIPEKB_BENCH
#      (swipe bench program; bench/score.sh is run when it exists), SCOREBOARD (output file).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EVAL="${SWIPEKB_NEXTWORD_EVAL:-$ROOT/build/tools/swipekb-nextword-eval}"
TESTSETS="${SWIPEKB_TESTSETS:-$HOME/.cache/swipe-keyboard-train}"
PACKS="${SWIPEKB_PACKS:-/usr/share/swipe-keyboard/language-packs}"
OUT="${SCOREBOARD:-$ROOT/bench/scoreboard.tsv}"
label=manual check=0 langs="en es fr de" n=300
while [ $# -gt 0 ]; do
    case "$1" in
        --label) label="$2"; shift 2 ;;
        --check) check=1; shift ;;
        --langs) langs="$2"; shift 2 ;;
        --sentences) n="$2"; shift 2 ;;
        *) echo "usage: bench/scoreboard.sh [--label TEXT] [--check] [--langs \"es fr de\"] [--sentences N]" >&2; exit 2 ;;
    esac
done
[ -x "$EVAL" ] || { echo "scoreboard: $EVAL not built (set SWIPEKB_NEXTWORD_EVAL)" >&2; exit 2; }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pack_of() {   # prints "model vocab" for a language
    local var="SWIPEKB_PACK_$1"
    if [ -n "${!var:-}" ]; then echo "${!var}/context.ngram ${!var}/vocab.tsv"
    else echo "$PACKS/$1.ngram $PACKS/$1.vocab.tsv"; fi
}

# WHY one background job per (language, set, letters): each is single-threaded; together a run takes minutes.
for l in $langs; do
    read -r model vocab < <(pack_of "$l")
    [ -s "$model" ] && [ -s "$vocab" ] || { echo "scoreboard: no pack for $l ($model)" >&2; exit 2; }
    echo "$l pack_mb $(du -m --apparent-size "$model" | cut -f1)" > "$tmp/$l-size"
    for set in everyday web tatoeba; do
        sentences="$TESTSETS/$l/test/$set.txt"
        [ -s "$sentences" ] || { echo "scoreboard: missing $sentences" >&2; exit 2; }
        for k in 0 1 3; do
            "$EVAL" --model "$model" --vocab "$vocab" --sentences "$sentences" --limit "$n" --letters "$k" \
                --label x > "$tmp/$l-$set-$k" &
        done
    done
    for set in everyday web; do
        ref="$ROOT/bench/gboard/reference/$l-$set.tsv"
        [ -s "$ref" ] && "$EVAL" --model "$model" --vocab "$vocab" --gboard "$ref" --label x > "$tmp/$l-gboard-$set" &
    done
done
wait

# Rows: lang measure value. Columns of the eval row: 4 top1, 5 top3, 10 ms, 11 top2 (see the program's header).
rows="$tmp/rows"; : > "$rows"
for l in $langs; do
    cat "$tmp/$l-size" >> "$rows"
    for set in everyday web tatoeba; do
        # WHY top3 before typing and top2 after: once letters are typed one of the 3 slots shows them.
        awk -F'\t' -v l="$l" -v s="$set" 'NR==2 { print l, "next_" s "_top3", $5; print l, "ms_per_suggestion_" s, $10 }' "$tmp/$l-$set-0" >> "$rows"
        for k in 1 3; do
            awk -F'\t' -v l="$l" -v s="$set" -v k="$k" 'NR==2 { print l, "complete" k "_" s "_top2", $11 }' "$tmp/$l-$set-$k" >> "$rows"
        done
    done
    for set in everyday web; do
        [ -s "$tmp/$l-gboard-$set" ] || continue
        # Points ahead of Gboard (negative = behind) on its exact queries; same fair-slot rule.
        awk -F'\t' -v l="$l" -v s="$set" 'NR>1 { ours = ($2 == 0 ? $8 : $7); printf "%s vs_gboard_%s_%sletters %.2f\n", l, s, $2, ours - $5 }' \
            "$tmp/$l-gboard-$set" >> "$rows"
    done
done
SWIPEKB_BENCH="${SWIPEKB_BENCH:-$ROOT/build/tools/swipekb-bench}"
if [ -x "$SWIPEKB_BENCH" ]; then
    swipe=$(BENCH_HISTORY="$tmp/swipe-history.tsv" "$ROOT/bench/score.sh" --runner recognizer --label "$label" | head -1)
    sed -E 's/^first guess right: ([0-9.]+)%.*/en swipe_top1 \1/' <<<"$swipe" >> "$rows"
else
    echo "(swipe not scored: $SWIPEKB_BENCH not built)" >&2
fi

commit=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo none)
[ -f "$OUT" ] || printf 'date\tcommit\tlabel\tsentences\tlang\tmeasure\tvalue\n' > "$OUT"
failed=0
while read -r l m v; do
    prev=$(awk -F'\t' -v l="$l" -v m="$m" -v n="$n" 'NR > 1 && $5 == l && $6 == m && $4 == n { p = $7 } END { print p }' "$OUT")
    note=""
    if [ -n "$prev" ] && [[ "$m" != ms_* && "$m" != pack_mb ]] && awk -v a="$v" -v b="$prev" 'BEGIN { exit !(a < b - 1.0) }'; then
        note="   <-- down from $prev"; failed=1
    fi
    printf '%-3s %-32s %8s%s\n' "$l" "$m" "$v" "$note"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%F)" "$commit" "$label" "$n" "$l" "$m" "$v" >> "$OUT"
done < "$rows"
if [ "$check" = 1 ] && [ "$failed" = 1 ]; then
    echo "FAIL: a measure fell more than 1 point below the previous run"
    exit 1
fi
