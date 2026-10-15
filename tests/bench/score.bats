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
@test "SWIPEKB_DICT picks the dictionary handed to infer_jsonl" {
  # WHY: the dictionary experiment relies on this override; the fake engine logs its arguments.
  printf '#!/usr/bin/env bash\necho "$@" > "%s/args"\nprintf "  top-1: 1 / 1 = 50%%%%\\n  top-4: 1 / 1 = 60%%%%\\n"\n' "$BATS_TEST_TMPDIR" > "$INFER_JSONL"
  SWIPEKB_DICT=/some/other.combined run "$ROOT/bench/score.sh" --label other
  [ "$status" -eq 0 ]
  grep -q -- "--vocab /some/other.combined" "$BATS_TEST_TMPDIR/args"
  run "$ROOT/bench/score.sh" --label default
  grep -q -- "--vocab $SWIPEKB_ASSETS/dict/en_swipe.combined" "$BATS_TEST_TMPDIR/args"
}
@test "--runner recognizer calls swipekb-bench, labels the row, and keeps futo's best separate" {
  # WHY: fake swipekb-bench logs its arguments and prints infer_jsonl-format output plus the extra lines.
  export SWIPEKB_BENCH="$BATS_TEST_TMPDIR/bench"
  cat > "$SWIPEKB_BENCH" <<'SH'
#!/usr/bin/env bash
echo "$@" > "$BATS_TEST_TMPDIR/bargs"
printf '  top-1:  1 / 2 = 50%%\n  top-4:  2 / 2 = 80.5%%\n\nExtras:\n  recall@50:  2 / 2 = 99%%\n  first-word top-1:  1 / 2 = 70%%\nTimings (avg µs per sample):\n  total:    42.5\n'
SH
  chmod +x "$SWIPEKB_BENCH"
  SWIPEKB_DICT=/some/other.combined run "$ROOT/bench/score.sh" --runner recognizer --label rec
  [ "$status" -eq 0 ]
  [[ "$output" == *"first guess right: 50%"* ]]; [[ "$output" == *"recall@50"* ]]; [[ "$output" == *"first-word"* ]]
  grep -q -- "--data $SWIPEKB_ASSETS --dict /some/other.combined --jsonl $SWIPEKB_ASSETS/eval/bench-4.jsonl" "$BATS_TEST_TMPDIR/bargs"
  tail -1 "$BENCH_HISTORY" | grep -q $'\t\\[recognizer\\] rec\t4\t50\t80.5\t42.5$'
  # The futo runner's best (90.5) must not make a lower recognizer row fail --check, and vice versa.
  "$ROOT/bench/score.sh" --label futo-base
  run "$ROOT/bench/score.sh" --runner recognizer --label again --check
  [ "$status" -eq 0 ]
}
@test "--runner rejects unknown names" {
  run "$ROOT/bench/score.sh" --runner nope
  [ "$status" -eq 2 ]
}
@test "SWIPEKB_BENCH_FLAGS reaches swipekb-bench (recognizer runner)" {
  # WHY: before/after and ablation rows (e.g. --no-ngram, --use-ngram) are made with this; the fake logs its args.
  export SWIPEKB_BENCH="$BATS_TEST_TMPDIR/bench"
  printf '#!/usr/bin/env bash\necho "$@" > "%s/bargs"\nprintf "  top-1: 1 / 1 = 50%%%%\\n  top-4: 1 / 1 = 60%%%%\\n"\n' "$BATS_TEST_TMPDIR" > "$SWIPEKB_BENCH"
  chmod +x "$SWIPEKB_BENCH"
  SWIPEKB_BENCH_FLAGS="--use-ngram --alpha 2" run "$ROOT/bench/score.sh" --runner recognizer --label flags
  [ "$status" -eq 0 ]
  grep -q -- "--top-k 3 --use-ngram --alpha 2" "$BATS_TEST_TMPDIR/bargs"
}
