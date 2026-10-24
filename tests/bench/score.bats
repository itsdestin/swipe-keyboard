#!/usr/bin/env bats

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# bench/score.sh: reads swipekb-bench's summary, records it, and (--check) refuses scores below the best so far.
# Uses a fake swipekb-bench so these tests need no models.
setup() {
  ROOT="$BATS_TEST_DIRNAME/../.."
  export SWIPEKB_ASSETS="$BATS_TEST_TMPDIR/assets" BENCH_N=4 BENCH_HISTORY="$BATS_TEST_TMPDIR/history.tsv"
  mkdir -p "$SWIPEKB_ASSETS/eval"
  for i in 1 2 3 4 5 6 7 8; do echo "{\"id\":$i}"; done > "$SWIPEKB_ASSETS/eval/test.jsonl"
  export FAKE_TOP1=90.5 FAKE_TOP3=97.25
  export SWIPEKB_BENCH="$BATS_TEST_TMPDIR/bench"
  cat > "$SWIPEKB_BENCH" <<'SH'
#!/usr/bin/env bash
echo "$@" > "$BATS_TEST_TMPDIR/bargs"
printf 'Results: 4 samples\n  top-1: 3 / 4 = %s%%\n  top-3: 4 / 4 = %s%%\nall 4: first x | 1.2345 ms/swipe | own\n' "$FAKE_TOP1" "$FAKE_TOP3"
SH
  chmod +x "$SWIPEKB_BENCH"
}
@test "records a row and prints both scores" {
  run "$ROOT/bench/score.sh" --label first
  [ "$status" -eq 0 ]
  [[ "$output" == *"first guess right: 90.5%"* ]]; [[ "$output" == *"top 3: 97.25%"* ]]
  [ "$(wc -l < "$BENCH_HISTORY")" -eq 2 ]
  tail -1 "$BENCH_HISTORY" | grep -q $'\t\\[engine\\] first\t4\t90.5\t97.25\t1234.5$'
  [ -s "$SWIPEKB_ASSETS/eval/bench-4.jsonl" ]
}
@test "--check passes when scores tie or beat the best" {
  "$ROOT/bench/score.sh" --label base
  run "$ROOT/bench/score.sh" --label same --check; [ "$status" -eq 0 ]
  FAKE_TOP1=91 run "$ROOT/bench/score.sh" --label better --check; [ "$status" -eq 0 ]
}
@test "--check fails when either score drops" {
  "$ROOT/bench/score.sh" --label base
  FAKE_TOP3=96 run "$ROOT/bench/score.sh" --label worse --check
  [ "$status" -eq 1 ]; [[ "$output" == *"FAIL"* ]]
}
@test "older rows of other engines never decide --check" {
  printf 'date\tcommit\tlabel\tn\ttop1\ttop3\tavg_us\n2026-01-01\tx\tbaseline\t4\t99\t99\t1\n' > "$BENCH_HISTORY"
  run "$ROOT/bench/score.sh" --label first --check
  [ "$status" -eq 0 ]
}
@test "unreadable swipekb-bench output is an error, nothing recorded" {
  printf '#!/usr/bin/env bash\necho boom\n' > "$SWIPEKB_BENCH"
  run "$ROOT/bench/score.sh"
  [ "$status" -eq 1 ]; [ ! -f "$BENCH_HISTORY" ]
}
@test "SWIPEKB_DICT and SWIPEKB_BENCH_FLAGS reach swipekb-bench" {
  # WHY: the dictionary experiment and ablation rows rely on these overrides; the fake engine logs its arguments.
  SWIPEKB_DICT=/some/other.combined SWIPEKB_BENCH_FLAGS="--ngram /x.ngram" run "$ROOT/bench/score.sh" --label other
  [ "$status" -eq 0 ]
  grep -q -- "--dict /some/other.combined" "$BATS_TEST_TMPDIR/bargs"
  grep -q -- "--ngram /x.ngram $SWIPEKB_ASSETS/eval/bench-4.jsonl" "$BATS_TEST_TMPDIR/bargs"
  run "$ROOT/bench/score.sh" --label default
  grep -q -- "--dict $SWIPEKB_ASSETS/dict/en_swipe.combined" "$BATS_TEST_TMPDIR/bargs"
}
@test "unknown options are refused" {
  run "$ROOT/bench/score.sh" --runner nope
  [ "$status" -eq 2 ]
}
