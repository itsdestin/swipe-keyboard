#!/usr/bin/env bats
# bench/score.sh: reads infer_jsonl's summary, records it, and (--check) refuses scores below the best so far.
# Uses a fake infer_jsonl so these tests need no models.
setup() {
  ROOT="$BATS_TEST_DIRNAME/../.."
  export SWIPEKB_ASSETS="$BATS_TEST_TMPDIR/assets" BENCH_N=4 BENCH_HISTORY="$BATS_TEST_TMPDIR/history.tsv"
  mkdir -p "$SWIPEKB_ASSETS/eval"
  for i in 1 2 3 4 5 6 7 8; do echo "{\"id\":$i}"; done > "$SWIPEKB_ASSETS/eval/test.jsonl"
  export FAKE_TOP1=90.5 FAKE_TOP3=97.25
  export INFER_JSONL="$BATS_TEST_TMPDIR/infer"
  cat > "$INFER_JSONL" <<'SH'
#!/usr/bin/env bash
printf 'good 1 a:a:[]\n\nResults: 4 samples\n  top-1:  3 / 4 = %s%%\n  top-4:  4 / 4 = %s%%\n  miss:   0 / 4 = 0%%\n\nTimings (avg µs per sample):\n  total:    1234.5\n' "$FAKE_TOP1" "$FAKE_TOP3"
SH
  chmod +x "$INFER_JSONL"
}
@test "records a row and prints both scores" {
  run "$ROOT/bench/score.sh" --label first
  [ "$status" -eq 0 ]
  [[ "$output" == *"first guess right: 90.5%"* ]]; [[ "$output" == *"top 3: 97.25%"* ]]
  [ "$(wc -l < "$BENCH_HISTORY")" -eq 2 ]
  tail -1 "$BENCH_HISTORY" | grep -q $'\tfirst\t4\t90.5\t97.25\t1234.5$'
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
@test "unreadable infer_jsonl output is an error, nothing recorded" {
  printf '#!/usr/bin/env bash\necho boom\n' > "$INFER_JSONL"
  run "$ROOT/bench/score.sh"
  [ "$status" -eq 1 ]; [ ! -f "$BENCH_HISTORY" ]
}
