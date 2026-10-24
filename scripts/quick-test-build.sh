#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# quick-test-build.sh — the fast lane (card #200): from a code change to a test package the owner can install, in a
# few minutes. Nothing is installed; the last line printed is the install command.
#
#   1. build     incremental Ninja build of this checkout (a first run configures and builds everything, ~1 min)
#   2. tests     only the CTest groups for the parts that changed since the last passing quick build
#   3. stage     install into a clean stage, then the same payload checks CI runs on the real package
#                (package-check.py and check-stock-layout.py against the installed layout, not the build tree)
#   4. package   the throw-away Arch package (scripts/stage-test-install.sh package), next free test number
#   5. bench     only with --bench, AFTER the package is handed over, at the lowest priority
#
# WHY these and not more: compile ~30 s, all 49 tests ~2 min and packaging ~1 min on this laptop; what made a test
# build take an hour was running every benchmark, both KDE front ends and the extra suites each round, all at once.
# Those run once, before a release: scripts/full-verify.sh. What this lane can let through: a change in one part
# that breaks another part's tests (engine changes run everything for that reason), a regression only on the other
# KDE version, and accuracy changes (unless --bench).
#
# Usage: scripts/quick-test-build.sh [--base REF] [--all-tests] [--bench] [--rel N]
#   --base REF    compare against REF instead of the last passing quick build (default: that build, else origin/main)
#   --all-tests   run all CTest groups
#   --bench       after handing over the package, replay the English real-swipe slice (bench/score.sh) and, when
#                 a language's files changed, that language's next-word scoreboard; results go to the work folder
#   --rel N       package release number (default: one above the installed or last built test number)
# Env: SWIPEKB_QUICK_WORK  build + work folder (default ~/.cache/swipe-keyboard-train/quick/<branch>)
#      SWIPEKB_ASSETS      asset folder to build with (default: the pinned files, fetched and checked into
#                          ~/.cache/swipe-keyboard-train/quick/assets). A custom folder is said so in the package line.
#      SWIPEKB_ROLLBACK_PKG package to roll back to (default: the one package in ~/.cache/swipe-keyboard-train/quick/rollback/)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$HOME/.cache/swipe-keyboard-train/quick"
BRANCH="$(git -C "$ROOT" rev-parse --abbrev-ref HEAD | tr '/' '-')"
WORK="${SWIPEKB_QUICK_WORK:-$CACHE/$BRANCH}"
STAGE="$WORK/stage"
base='' all_tests=0 bench=0 rel=''
while [ $# -gt 0 ]; do
    case "$1" in
        --base) base="$2"; shift 2 ;;
        --all-tests) all_tests=1; shift ;;
        --bench) bench=1; shift ;;
        --rel) rel="$2"; shift 2 ;;
        *) sed -n 7,31p "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
# WHY nice: the owner uses this laptop while it builds; a lower priority costs seconds, not minutes.
NICE='nice -n 10'
START=$(date +%s) PH='' T0=$START
phase() {   # per-phase times, so "under 10 minutes" is measured, not claimed
    local now; now=$(date +%s)
    [ -n "$PH" ] && printf '   %s took %ds\n' "$PH" $((now - T0))
    PH="$1" T0=$now
    [ -n "$1" ] && printf '== %s (%s)\n' "$1" "$(date +%T)"
    return 0
}

mkdir -p "$WORK"
# --- what changed --------------------------------------------------------------------------------------------------
LAST="$WORK/.quick-last-pass"
if [ -z "$base" ]; then
    if [ -f "$LAST" ] && git -C "$ROOT" cat-file -e "$(cat "$LAST")^{commit}" 2>/dev/null; then base=$(cat "$LAST")
    else base=$(git -C "$ROOT" merge-base HEAD origin/main); fi
fi
# Tracked changes against base (committed or not) plus new files.
mapfile -t changed < <({ git -C "$ROOT" diff --name-only "$base"; git -C "$ROOT" ls-files --others --exclude-standard; } | sort -u)
groups='' everything=$all_tests
add() { case " $groups " in *" $1 "*) ;; *) groups="$groups $1" ;; esac; }
for f in "${changed[@]}"; do
    case "$f" in
        *.md|docs/*|LICENSES/*|bench/*.tsv|CHANGELOG.md) ;;              # words only: no test can change
        plugin/*) add plugin ;;
        frontend-plasma69/*) add plasma69 ;;
        tools/*) add tools ;;
        gaming/*) add gaming ;;
        # WHY everything for the rest: the engine, build files, scripts, packaging and data are used by every part,
        # and an unknown path is treated the same way rather than guessed.
        *) everything=1 ;;
    esac
done
echo "base $(git -C "$ROOT" rev-parse --short "$base"); ${#changed[@]} changed file(s)"

# --- 1. build ------------------------------------------------------------------------------------------------------
phase build
VERSION="$(python3 -c "import json;print(json.load(open('$ROOT/packaging/release-targets.json'))['version'])")"
if [ -z "$rel" ]; then
    # Next free test number: one above the installed package (same version) and every package already built here.
    # WHY above the installed one: pacman treats a lower number as a downgrade.
    last=$( { pacman -Q swipe-keyboard 2>/dev/null | sed -n "s/^swipe-keyboard $VERSION-//p"
              ls "$WORK/pkg" 2>/dev/null | sed -n "s/^swipe-keyboard-$VERSION-\([0-9.]*\)-x86_64\.pkg\.tar\.zst$/\1/p"; } | sort -V | tail -1 || true)
    case "$last" in
        '') rel=0.1 ;;
        *.*) rel="${last%.*}.$(( ${last##*.} + 1 ))" ;;
        *) rel="$last.1" ;;
    esac
fi
custom_assets=''
if [ -n "${SWIPEKB_ASSETS:-}" ]; then ASSETS="$SWIPEKB_ASSETS" custom_assets=" (custom assets: $ASSETS)"
else
    ASSETS="$CACHE/assets"
    # WHY a stamp: fetch-assets.py also regenerates the es/fr/de packs (and wants 21 GiB free), so it runs only when
    # the pinned list or the generator changed. package-check.py still checks every staged file against the pins.
    stamp=$(cat "$ROOT/packaging/assets.lock.json" "$ROOT/scripts/fetch-assets.py" "$ROOT/scripts/dict/language_pack.py" | sha256sum | cut -c1-16)
    if [ "$(cat "$ASSETS/.quick-stamp" 2>/dev/null)" != "$stamp" ]; then
        echo "fetching pinned assets into $ASSETS (first run or pins changed)"
        $NICE python3 "$ROOT/scripts/fetch-assets.py" --assets "$ASSETS" >"$WORK/fetch-assets.log" 2>&1 \
            || { tail -5 "$WORK/fetch-assets.log"; echo "or point SWIPEKB_ASSETS at an asset folder" >&2; exit 1; }
        echo "$stamp" > "$ASSETS/.quick-stamp"
    fi
fi
if [ -f "$WORK/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$ROOT" "$WORK/CMakeCache.txt"; then
    echo "$WORK was configured for another checkout; set SWIPEKB_QUICK_WORK to a new folder" >&2; exit 1
fi
# WHY the two test asset paths: without them four language tests skip themselves silently.
# WHY -D every run: the version label inside the app must name this test number (earlier test builds kept saying 0.1).
$NICE cmake -S "$ROOT" -B "$WORK" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DSWIPEKB_ASSETS="$ASSETS" -DSWIPEKB_PACKAGE_VERSION="$VERSION-$rel" \
    -DSWIPEKB_TEST_ENGLISH_ASSETS="$ASSETS" -DSWIPEKB_TEST_LANGUAGE_PACKS="$ASSETS/language-packs" >"$WORK.configure.log" 2>&1 \
    || { tail -30 "$WORK.configure.log"; exit 1; }
$NICE cmake --build "$WORK" | tail -3

# --- 2. tests ------------------------------------------------------------------------------------------------------
phase tests
if [ "$everything" = 1 ]; then regex='.'; echo "running every test group"
elif [ -n "$groups" ]; then regex="^($(echo $groups | tr ' ' '|'))_"; echo "running test groups:$groups"
else regex=''; echo "only words changed: no tests to run"; fi
if [ -n "$regex" ]; then
    QT_QPA_PLATFORM=offscreen $NICE ctest --test-dir "$WORK" -R "$regex" -j "$(nproc)" --output-on-failure | tail -15
fi

# --- 3. stage + payload checks -------------------------------------------------------------------------------------
phase stage
# WHY a clean stage: files a change removed must not linger in the test package.
rm -rf "$STAGE"
DESTDIR="$STAGE" $NICE cmake --install "$WORK" >"$WORK/install.log"
# WHY this check here: it loads the INSTALLED layout, which the build-tree tests never see. In 0.5.0 it alone caught
# that the installed keyboard found no words (CI run 40130981841) while every laptop test passed.
QT_QPA_PLATFORM=offscreen $NICE python3 "$ROOT/scripts/check-stock-layout.py" "$WORK/plugin/tst_stocklayout" \
    --runtime-root "$STAGE/usr/share/swipe-keyboard" | tail -3

# --- 4. package ----------------------------------------------------------------------------------------------------
phase package
# stage-test-install.sh runs package-check.py on the stage before building.
SWIPEKB_FULLBUILD="$WORK" SWIPEKB_TEST_REL="$rel" "$ROOT/scripts/stage-test-install.sh" package | tail -2
git -C "$ROOT" rev-parse HEAD > "$LAST"
phase ''
ROLLBACK="${SWIPEKB_ROLLBACK_PKG:-$(ls "$CACHE"/rollback/*.pkg.tar.zst 2>/dev/null | head -1 || true)}"
printf '\nTest build %s-%s ready in %ds%s. Install (restarts the keyboard):\n' "$VERSION" "$rel" $(( $(date +%s) - START )) "$custom_assets"
if [ -n "$ROLLBACK" ]; then
    printf '  SWIPEKB_FULLBUILD=%s SWIPEKB_TEST_REL=%s SWIPEKB_ROLLBACK_PKG=%s %s install\n' "$WORK" "$rel" "$ROLLBACK" "$ROOT/scripts/stage-test-install.sh"
else
    echo "  (no rollback package: put a known-good one, with its SHA256SUMS, in $CACHE/rollback/ first)"
fi

# --- 5. optional benchmarks, after the hand-over ---------------------------------------------------------------------
[ "$bench" = 1 ] || exit 0
phase bench
# WHY copies of the history files: the scoreboards append rows; a quick build must not edit tracked files.
cp "$ROOT/bench/history.tsv" "$WORK/history.tsv"
SWIPEKB_ASSETS="$ASSETS" SWIPEKB_BENCH="$WORK/tools/swipekb-bench" BENCH_HISTORY="$WORK/history.tsv" \
    nice -n 19 "$ROOT/bench/score.sh" --label "quick $VERSION-$rel" | tail -3
langs=''
for l in en es fr de; do printf '%s\n' "${changed[@]}" | grep -q "/$l[/_.]" && langs="$langs $l"; done
if [ -n "$langs" ]; then
    cp "$ROOT/bench/scoreboard.tsv" "$WORK/scoreboard.tsv"
    SWIPEKB_NEXTWORD_EVAL="$WORK/tools/swipekb-nextword-eval" SWIPEKB_PACKS="$ASSETS/language-packs" SCOREBOARD="$WORK/scoreboard.tsv" \
        nice -n 19 "$ROOT/bench/scoreboard.sh" --label "quick $VERSION-$rel" --langs "${langs# }" | tail -8
fi
phase ''
