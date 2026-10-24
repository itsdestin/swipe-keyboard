#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# full-verify.sh — the full lane (card #200): everything that used to run on every test build, run ONCE before a
# release, one step after another on a quiet machine. Nothing is installed.
#
#   1. quick lane with every test group (build, all CTest, stage, CI's payload checks, test package)
#   2. the Python suites: packaging and licences, integration, Gaming Mode, language files
#   3. the Plasma 6.9 front end against each KDE source in SWIPEKB_KDE_SOURCES (both KDE versions)
#   4. accuracy: the English real-swipe slice and every language's next-word scoreboard, with --check, so a drop of
#      more than the noise level fails the run
#
# WHY one after another: run together, the benchmarks slowed the tests and each other (load 47-65) and their speed
# numbers became meaningless. A failed step does not stop the later ones; the summary lists every result and the
# exit code is non-zero if any failed.
#
# Usage: scripts/full-verify.sh [--skip-bench]
# Env: everything scripts/quick-test-build.sh reads, plus
#      SWIPEKB_KDE_SOURCES  KDE plasma-keyboard checkouts for step 3, space-separated
#                           (default: plasma-keyboard-master and pk69/plasma-keyboard-mr179 under ~/.cache/swipe-keyboard-train)
#      SWIPEKB_TESTSETS     next-word test sets (see bench/scoreboard.sh)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$HOME/.cache/swipe-keyboard-train"
BRANCH="$(git -C "$ROOT" rev-parse --abbrev-ref HEAD | tr '/' '-')"
WORK="${SWIPEKB_QUICK_WORK:-$CACHE/quick/$BRANCH}"
skip_bench=0
[ "${1:-}" = --skip-bench ] && skip_bench=1
results=() failed=0
step() {   # step NAME COMMAND...: run, time, record
    local name="$1" t0 rc; shift
    printf '\n######## %s (%s)\n' "$name" "$(date +%T)"
    t0=$(date +%s)
    "$@"; rc=$?
    results+=("$(printf '%-34s %-6s %4ds' "$name" "$([ $rc = 0 ] && echo pass || echo FAIL)" $(( $(date +%s) - t0 )))")
    [ $rc = 0 ] || failed=1
}
py_suite() { (cd "$ROOT" && nice -n 10 python3 -I -m unittest discover -s "$1" -p "${2:-test_*.py}" 2>&1 | tail -4; exit "${PIPESTATUS[0]}"); }

step "quick lane, every test" "$ROOT/scripts/quick-test-build.sh" --all-tests
step "packaging + licence tests" py_suite tests/packaging
step "integration tests" py_suite tests/integration
gaming_suite() {   # WHY two skipped: they need the built daemon and already run under CTest (same split as CI)
    local f rc=0
    for f in "$ROOT"/tests/gaming/test_*.py; do
        case "$(basename "$f")" in test_daemon.py|test_commit_e2e.py) continue ;; esac
        py_suite tests/gaming "$(basename "$f")" || rc=1
    done
    return $rc
}
step "Gaming Mode tests" gaming_suite
step "language file tests" py_suite tests

fe_check() {   # fe_check KDE_SOURCE: build and test the 6.9 front end against one KDE checkout
    local out="$WORK/fe-$(basename "$1")"
    nice -n 10 cmake -S "$ROOT/frontend-plasma69" -B "$out" -G Ninja -DSWIPEKB_PLASMA_KEYBOARD_SOURCE="$1" \
        -DSWIPEKB_TEST_MODEL_DATA="${SWIPEKB_ASSETS:-$CACHE/quick/assets}" >"$out.configure.log" 2>&1 || { tail -20 "$out.configure.log"; return 1; }
    nice -n 10 ninja -C "$out" | tail -1 && QT_QPA_PLATFORM=offscreen nice -n 10 ctest --test-dir "$out" -j "$(nproc)" --output-on-failure | tail -4
}
for src in ${SWIPEKB_KDE_SOURCES:-$CACHE/plasma-keyboard-master $CACHE/pk69/plasma-keyboard-mr179}; do
    if [ -d "$src" ]; then step "6.9 front end on $(basename "$src")" fe_check "$src"
    else results+=("$(printf '%-34s %-6s' "6.9 front end on $(basename "$src")" missing)"); failed=1; fi
done

if [ "$skip_bench" = 0 ]; then
    A="${SWIPEKB_ASSETS:-$CACHE/quick/assets}"
    # WHY copies: the scoreboards append rows; the lane must not edit tracked files. Copy rows back by hand
    # when a release records its numbers.
    cp "$ROOT/bench/history.tsv" "$WORK/history.tsv"; cp "$ROOT/bench/scoreboard.tsv" "$WORK/scoreboard.tsv"
    step "English real swipes (--check)" env SWIPEKB_ASSETS="$A" SWIPEKB_BENCH="$WORK/tools/swipekb-bench" \
        BENCH_HISTORY="$WORK/history.tsv" nice -n 10 "$ROOT/bench/score.sh" --label "full-verify $(git -C "$ROOT" rev-parse --short HEAD)" --check
    step "next word, all languages (--check)" env SWIPEKB_NEXTWORD_EVAL="$WORK/tools/swipekb-nextword-eval" \
        SWIPEKB_PACKS="$A/language-packs" SCOREBOARD="$WORK/scoreboard.tsv" \
        nice -n 10 "$ROOT/bench/scoreboard.sh" --label "full-verify $(git -C "$ROOT" rev-parse --short HEAD)" --check
fi

printf '\n######## summary\n'; printf '%s\n' "${results[@]}"
exit $failed
