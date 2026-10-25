# One word brain, part 1: swap scoreboard and swap fix — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure tap-to-swap and post-swipe alternatives with a new scoreboard. Then replace today's swap list with
path look-alikes ranked by the words before and after.

**Architecture:**
- Candidates come from the swipe beam: the saved trace, or a synthetic trace through the word's key centres.
- They are scored in two phases:
  - On the input thread, inside `LanguageEngine::swapCandidates`: path + n-gram before/after.
  - On the next-word worker: the neural before-score, via a new `Predictor::scoreWords`.
- One pure function, `swipekb::rankSwap`, combines the scores. The plugin, the Plasma 6.9 front end and the bench all
  call the same function.

**Tech stack:** C++20, Qt 6 Core, GoogleTest (engine), QtTest (plugin/tools), CMake/Ninja, bash.

Spec: `docs/active/specs/2026-10-24-one-word-brain.md` (rollout steps 1 and 2).

## Global constraints

- Every new source file starts with the SPDX header used across the repo:
  ```
  SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
  SPDX-License-Identifier: MIT
  ```
  `~/.cache/swipe-keyboard-train/reuse-venv/bin/reuse lint` must pass.
- A **WHY comment** at every non-trivial edit, in the repo's style. The owner follows changes through them.
- Never touch the installed keyboard (`.claude/rules/live-keyboard-safety.md` in the workspace). Tests use temporary
  HOME/XDG folders.
- HeliBoard data is CC BY-SA: test only, never tuned or trained on. The test sets (bench-2000, test-extra-10000,
  HeliBoard) are never used for tuning.
- Tuning data:
  - English: `~/.cache/swipe-keyboard-train/shape/futo/trv8k.jsonl`.
  - French and German: FUTO swipe-5 validation slices (Task 8 finds them).
  - Spanish: shares the French/German weights.
- Speed budgets: a swap list ≤ 20 ms on the input thread; swipe unchanged.
- First option and in-3 are always reported separately, never rounded up.
- **Batch the slow work.**
  - During tasks, build only the touched targets (`cmake --build build --target <t>`) and run only the matching tests
    (`ctest --test-dir build -R <regex> -j4`).
  - The full suite, `scripts/full-verify.sh`, the scoreboards and the owner's test build run once, in Task 10.
  - One PR for the whole plan, so CI runs once.
- Local builds use at most half the cores (`-j4`): the machine is a tablet the owner is using.
- Stage explicit paths only. Commit messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.
  Push the branch after each task: `git push`, never `--no-verify`.

## Setup (once)

```bash
cd /home/destin/swipe-keyboard-dev/worktrees/sessions/one-brain/swipe-keyboard
python3 scripts/fetch-assets.py --assets ~/.cache/swipe-keyboard-train/one-brain-assets
export SWIPEKB_ASSETS=~/.cache/swipe-keyboard-train/one-brain-assets
export SWIPEKB_EVAL=/home/destin/swipe-keyboard-dev/swipe-keyboard/assets/eval      # bench-2000.jsonl, test-extra-10000.jsonl
export HELI=~/.cache/swipe-keyboard-train/heliboard/out                               # {es,fr,de,en}/test.jsonl, layouts/
cmake -S . -B build -G Ninja -DSWIPEKB_ASSETS="$SWIPEKB_ASSETS" && cmake --build build -j4
```

Expected: the build finishes. If the configure step asks for other options, copy them from
`scripts/quick-test-build.sh`.

## File map

| File | Responsibility |
|---|---|
| `tools/benchdata.h` (new) | Reading swipe jsonl + layout files for every bench tool: words before and after the target |
| `tools/swipekb-bench.cpp` | Uses `benchdata.h` (no behaviour change) |
| `tools/swapmeasures.h/.cpp` (new) | Pure accounting of the five swap measures (unit-tested) |
| `tools/swipekb-swap-bench.cpp` (new) | Replays swipes through today's swap path (`--mode baseline`) or the new one (`--mode new`) |
| `tools/tests/tst_swapmeasures.cpp` (new) | Tests the accounting |
| `bench/swap-score.sh` (new), `bench/swap-scoreboard.tsv` (new) | Runs the swap bench and appends rows |
| `engine/include/swipekb/prediction.hpp`, `engine/src/prediction.cpp`, `engine/src/nwp_predictor.cpp`, `engine/include/swipekb/nwp_predictor.hpp` | `Predictor::scoreWords` |
| `engine/include/swipekb/swap.hpp` (new), `engine/src/swap.cpp` (new) | `SwapCandidate`, `SwapParams`, `rankSwap`, `syntheticTrace` |
| `engine/include/swipekb/language_engine.hpp`, `engine/src/language_engine.cpp` | `LanguageEngine::swapCandidates` |
| `engine/tests/test_swap.cpp` (new) | Engine tests for the above |
| `plugin/wordswap.h/.cpp` | `RecentSwipes` keeps the trace and keys. `WordSwap::build` stays only for the bench baseline |
| `plugin/nextwordworker.h/.cpp` | A `scoreWords` job kind |
| `plugin/swipeinputmethod.cpp`, `frontend-plasma69/src/swipecontroller.cpp` | Swap and post-swipe alternatives from the new path |

---

### Task 1: Shared bench data reader

**Files:**
- Create: `tools/benchdata.h`
- Modify: `tools/swipekb-bench.cpp` (replace the local `Swipe`, `readLayout` and the jsonl loop at lines ~43–66 and
  ~118–140)

**Interfaces:**
- Produces:
  - `struct BenchSwipe { std::string word, board; float w, h; std::vector<swipekb::TracePoint> trace; std::vector<std::string> before, after; };`
  - `std::vector<swipekb::KeyRect> readLayout(const std::string &path, float w, float h);`
  - `std::vector<BenchSwipe> readSwipes(const std::string &jsonl, size_t limit, bool withSentence, std::string *error);`
  - `std::string norm(const std::string &s, const std::string &mode);`
  - All inline, in namespace `benchdata`.

- [ ] **Step 1:** Create `tools/benchdata.h`. Move `readLayout` and `norm` there verbatim from `swipekb-bench.cpp`, and the
  jsonl reading loop into `readSwipes`. `after` holds the sentence tokens after `word_idx`, at most 2. WHY 2: the
  `after` term uses the next two words. `before` holds the tokens before `word_idx`, as today's `context`. Header
  comment:
  ```cpp
  // benchdata.h — reads recorded swipes (jsonl: "word", "data" [{x,y,t}] 0..1 of the letter area, "canvas_width",
  // "canvas_height", optional "layout", "sentence" + "word_idx") and layout files for every bench tool, so the swipe
  // and swap scoreboards read the same swipes the same way.
  ```
- [ ] **Step 2:** Make `swipekb-bench.cpp` use it: `s.context` becomes `s.before`; the reading code is deleted.
- [ ] **Step 3:** Check there is no behaviour change. Run the old binary on the shared checkout's build and the new one on
  200 swipes, then diff:
  ```bash
  cmake --build build --target swipekb-bench -j4
  B=$SWIPEKB_ASSETS/language-files
  build/tools/swipekb-bench --dict $B/en_swipe.combined --model $B/swipe-model.bin --ngram $B/en.ngram --context --language en \
    --layout scripts/swipe/layouts/qwerty.tsv --limit 200 --list $SWIPEKB_EVAL/bench-2000.jsonl > /tmp/claude-1000/after.txt
  git stash push -m one-brain-t1 -- tools/swipekb-bench.cpp && cmake --build build --target swipekb-bench -j4 && \
    build/tools/swipekb-bench <same flags> > /tmp/claude-1000/before.txt; git stash pop
  diff <(grep -v ms/swipe /tmp/claude-1000/before.txt) <(grep -v ms/swipe /tmp/claude-1000/after.txt)
  ```
  Expected: no diff.
  - Use `git stash push -m one-brain-t1`, then find its SHA and `git stash apply <sha>` / drop, per the worktree rule.
  - Simpler alternative: build the old file from `git show HEAD:tools/swipekb-bench.cpp` into a temporary target.
- [ ] **Step 4:** Commit:
  `git add tools/benchdata.h tools/swipekb-bench.cpp && git commit -m "Bench tools: one shared swipe reader (words before and after)"`

### Task 2: Swap measures (pure accounting)

**Files:**
- Create: `tools/swapmeasures.h`, `tools/swapmeasures.cpp`, `tools/tests/tst_swapmeasures.cpp`
- Modify: `tools/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  namespace swapbench {
  enum class Measure { AfterSwipe, TapBackTrace, TapBackNoTrace };
  struct Tally { size_t cases = 0, first = 0, inThree = 0; };
  struct Report {
      Tally m[3];                      // indexed by Measure
      size_t rightFirst = 0, shownWhenRight = 0, junkWhenRight = 0;   // measure 4
      double msTotal = 0; size_t lists = 0;                           // measure 5
      // A case counts only when the first guess was wrong (`wrongFirst`); `shown` is the bar's options in order.
      void add(Measure k, bool wrongFirst, const std::string &target, const std::vector<std::string> &shown,
               const std::function<std::string(const std::string &)> &norm);
      void addJunk(size_t shownCount, size_t junkCount) { ++rightFirst; shownWhenRight += shownCount; junkWhenRight += junkCount; }
      std::string format() const;      // the lines bench/swap-score.sh reads
  };
  }
  ```
  `format()` prints exactly:
  ```
  after-swipe: cases N first X.XX% in3 Y.YY%
  tapback-trace: cases N first X.XX% in3 Y.YY%
  tapback-notrace: cases N first X.XX% in3 Y.YY%
  junk: right R shown S junk J.JJ%
  speed: Z.ZZ ms/list
  ```
  Percentages use 2 decimals and a zero denominator prints `0.00%`. "first" means `shown[0]` matches; "in3" means any of
  the first 3 matches. In the bar's display order the middle slot is the strongest, so "first" uses the *ranked* order
  and the caller passes the ranked list, not the display order.

- [ ] **Step 1: Write the failing test** `tools/tests/tst_swapmeasures.cpp` (QtTest, like `tests/tst_replay.cpp`):
  ```cpp
  #include <QTest>
  #include "../swapmeasures.h"
  using namespace swapbench;
  class TstSwapMeasures : public QObject { Q_OBJECT
  private slots:
      void countsOnlyWrongFirstGuesses() {
          Report r; auto id = [](const std::string &s) { return s; };
          r.add(Measure::AfterSwipe, true, "good", {"food", "good", "hood"}, id);   // in 3, not first
          r.add(Measure::AfterSwipe, true, "good", {"good"}, id);                   // first
          r.add(Measure::AfterSwipe, false, "good", {"x"}, id);                     // first guess was right: not a case
          r.add(Measure::AfterSwipe, true, "good", {"a", "b", "c", "good"}, id);    // 4th: a miss
          QCOMPARE(r.m[0].cases, size_t(3)); QCOMPARE(r.m[0].first, size_t(1)); QCOMPARE(r.m[0].inThree, size_t(2));
      }
      void formatsZeroCases() {
          Report r;
          QVERIFY(QString::fromStdString(r.format()).contains("tapback-notrace: cases 0 first 0.00% in3 0.00%"));
      }
      void junkShare() {
          Report r; r.addJunk(3, 1); r.addJunk(1, 0);
          QVERIFY(QString::fromStdString(r.format()).contains("junk: right 2 shown 4 junk 25.00%"));
      }
  };
  QTEST_APPLESS_MAIN(TstSwapMeasures)
  #include "tst_swapmeasures.moc"
  ```
- [ ] **Step 2:** Add to `tools/CMakeLists.txt`:
  ```cmake
  # Swap scoreboard (card #500): the five swap measures are plain counting, tested on their own.
  add_library(swapmeasures STATIC swapmeasures.cpp)
  target_include_directories(swapmeasures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(tst_swapmeasures tests/tst_swapmeasures.cpp)
  target_link_libraries(tst_swapmeasures PRIVATE swapmeasures Qt6::Core Qt6::Test)
  add_test(NAME tools_swapmeasures COMMAND tst_swapmeasures)
  ```
  Then build `tst_swapmeasures`. Expected: it fails to compile or link (`swapmeasures.cpp` is missing).
- [ ] **Step 3:** Implement `swapmeasures.h/.cpp` to the interface above.
- [ ] **Step 4:** `cmake --build build --target tst_swapmeasures -j4 && ctest --test-dir build -R tools_swapmeasures`.
  Expected: PASS.
- [ ] **Step 5:** Commit `tools/swapmeasures.h tools/swapmeasures.cpp tools/tests/tst_swapmeasures.cpp tools/CMakeLists.txt`.

### Task 3: Swap bench, baseline mode, and the scoreboard script

**Files:**
- Create: `tools/swipekb-swap-bench.cpp`, `bench/swap-score.sh`, `bench/swap-scoreboard.tsv`
- Modify: `tools/CMakeLists.txt`

**Interfaces:**
- Consumes: `benchdata::*` (Task 1), `swapbench::Report` (Task 2), `WordSwap::build` (`plugin/wordswap.h`, library
  `swipekb_session`), `swipekb::rankRegionalCandidates` / `swipekb::offeredUnprompted` (`plugin/languagetextrules.h`,
  library `swipekb_language`), `swipekb::Predictor`.
- Produces: the CLI below. Task 8 adds `--mode new`.
  ```
  swipekb-swap-bench --mode baseline|new --dict D --model M [--ngram N --vocab V --nwp W] [--context] --language L
                     (--layout F | --layouts-dir DIR) [--locale en_US] [--limit N] [--match ascii|lookup] [--params F] swipes.jsonl
  ```

**Baseline, per swipe.** This mirrors `SwipeInputMethod::recognizeAndCommit` (≈ lines 760–835) and `observeWordSwap`
(≈ 1183–1252):
1. `c = engine.recognize(trace, keys, before, 30)` → words. Then `rankRegionalCandidates(words, locale, 3, bestShapeWord())`.
   If `otherSpellings(words[0])` is non-empty, apply `SwipeSession::withOtherSpelling(words, others[0])`. This gives
   `candidates`; `candidates[0]` is the committed word.
2. `wrongFirst = norm(candidates[0]) != norm(target)`.
3. **AfterSwipe:** `shown = candidates[1..]`, the bar's other places in ranked order.
4. **Neighbours of the committed word `w`:**
   - `otherSpellings(lower(w))` + `similar(w, 12)`;
   - drop the words failing `offeredUnprompted`, but only when `offeredUnprompted(w)`;
   - then `rankRegionalCandidates(neighbours, locale, 12)`.
   - Skip `personalize`: no learning in the bench, and swap's boost is 0.
5. **ranked:** `predictor.predict(before, 64, w.left(1).toLower())` when an n-gram pack is given, else `{}`.
6. **TapBackTrace:** `list = WordSwap::build(w, candidates, neighbours, ranked, 3)`. Undo the display swap for scoring:
   when `list.size() == 3`, swap `list[0]` and `list[1]` back, because `build` moves the strongest into the middle.
7. **TapBackNoTrace:** the same with `swipeCandidates = {}`.
8. **Junk:** when `!wrongFirst`, `addJunk(shown.size(), junk count)` using the `junk()` rule copied from
   `swipekb-bench.cpp`, over the AfterSwipe list.
9. **Speed:** time steps 4–6 per swipe.

Print `Report::format()`, then a line `setup: <engine.mode()> ngram=<0|1> nwp=<0|1> n=<swipes>`.

- [ ] **Step 1:** Write `tools/swipekb-swap-bench.cpp` as above. Header comment: what it measures, that `--mode baseline`
  calls the shipped `WordSwap::build` itself, and that HeliBoard input has no sentence. WHY: measures 2 and 3 then
  have no words before/after. CMake:
  ```cmake
  # swipekb-swap-bench: the swap scoreboard's engine (card #500). Baseline mode runs the shipped WordSwap::build itself.
  add_executable(swipekb-swap-bench swipekb-swap-bench.cpp)
  target_link_libraries(swipekb-swap-bench PRIVATE swipekb_engine swipekb_session swipekb_language swapmeasures Qt6::Core)
  ```
  If `swipekb_session` is not visible from `tools/` (CMake order), fix the order in the top-level `CMakeLists.txt` so
  `plugin/` is added before `tools/`. Keep the WHY.
- [ ] **Step 2:** Smoke run on 200 swipes:
  ```bash
  cmake --build build --target swipekb-swap-bench -j4
  B=$SWIPEKB_ASSETS/language-files
  build/tools/swipekb-swap-bench --mode baseline --dict $B/en_swipe.combined --model $B/swipe-model.bin --ngram $B/en.ngram \
    --vocab $B/en.vocab.tsv --nwp $B/en.nwp.bin --context --language en --locale en_US \
    --layout scripts/swipe/layouts/qwerty.tsv --limit 200 $SWIPEKB_EVAL/bench-2000.jsonl
  ```
  Expected: five measure lines plus `setup:`. after-swipe cases ≈ 20 (≈ 10% wrong first guesses). The speed line is
  under 20 ms.
- [ ] **Step 3:** Write `bench/swap-score.sh`, modelled on `bench/score.sh`: same env names, with `--label TEXT`,
  `--set NAME` and `--mode`.
  - `--set` takes `bench-2000 | test-extra-10000 | trv8k | heliboard-es|fr|de|en`. HeliBoard uses `--layouts-dir $HELI/layouts`,
    `--match lookup`, and no `--context`.
  - It appends rows to `bench/swap-scoreboard.tsv` with the columns
    `date commit label set mode n measure first in3 extra`. There is one row per measure: junk rows put the junk % in
    `extra`; speed goes in `extra`.
  - Header comment lines `#` explain the columns, as in `bench/swipe-scoreboard.tsv`.
  - `--check BASELINE_LABEL` exits 1 when any `in3` is below that label's row for the same set and measure.
- [ ] **Step 4:** Record the baseline. Each command takes a few minutes:
  ```bash
  for s in bench-2000 test-extra-10000 heliboard-es heliboard-fr heliboard-de heliboard-en trv8k; do
    bench/swap-score.sh --mode baseline --set $s --label "baseline 0.5.0 (WordSwap::build)"; done
  ```
  - `trv8k` rows are labelled with `tuning` in the label automatically. WHY: tuning rows must never be quoted as results.
  - FR/DE tuning data is added in Task 8.
- [ ] **Step 5:** Commit `tools/swipekb-swap-bench.cpp tools/CMakeLists.txt bench/swap-score.sh bench/swap-scoreboard.tsv`
  (and `CMakeLists.txt` if changed). Message: "Swap scoreboard: bench tool, script, baseline rows".

### Task 4: `Predictor::scoreWords` (neural + n-gram score for given words)

**Files:**
- Modify: `engine/include/swipekb/prediction.hpp`, `engine/src/prediction.cpp`,
  `engine/include/swipekb/nwp_predictor.hpp`, `engine/src/nwp_predictor.cpp`,
  `engine/include/swipekb/ngram_predictor.hpp` / `.cpp` (word → vocabulary index lookup if missing)
- Test: `engine/tests/test_prediction.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // prediction.hpp, class Predictor:
  // log10 score of each word as the next word after `context`: the same blend predict() ranks by (neural model and
  // n-gram). Words the vocabulary lacks get the n-gram floor. Same length and order as `words`. Empty when not ready.
  std::vector<float> scoreWords(const std::vector<std::string> &context, const std::vector<std::string> &words);
  ```
  `NwpPredictor` gets the same method, and `Predictor::scoreWords` forwards to it.

- [ ] **Step 1: Failing test** in `engine/tests/test_prediction.cpp`. It uses the existing fixture pack the file already
  builds (read the file first and reuse its helper that writes a tiny n-gram pack):
  ```cpp
  TEST(Predictor, ScoreWordsMatchesPredictOrder) {
      // The top words predict() returns must also be the best-scored by scoreWords(), in the same order.
      Predictor p; ASSERT_TRUE(p.init(fixtureModel(), fixtureVocab()));
      const std::vector<std::string> ctx{"i", "think"};
      const auto top = p.predict(ctx, 3);
      ASSERT_GE(top.size(), 2u);
      const auto s = p.scoreWords(ctx, top);
      ASSERT_EQ(s.size(), top.size());
      for (size_t i = 1; i < s.size(); ++i) EXPECT_GE(s[i - 1], s[i]);
  }
  TEST(Predictor, ScoreWordsUnknownWordIsFloor) {
      Predictor p; ASSERT_TRUE(p.init(fixtureModel(), fixtureVocab()));
      const auto s = p.scoreWords({"i"}, {"zzzqqq"});
      ASSERT_EQ(s.size(), 1u); EXPECT_LT(s[0], -5.f);
  }
  ```
  (Use the file's actual fixture helper names; if it has none, add one that writes a 5-word pack with
  `NgramLM::write`.)
- [ ] **Step 2:** `cmake --build build --target test_prediction -j4 && ctest --test-dir build -R engine_prediction`.
  Expected: FAIL to compile (`scoreWords` is not declared).
- [ ] **Step 3: Implement.** In `NwpPredictor`, factor the `add` lambda's arithmetic (nwp_predictor.cpp ≈ lines 105–120)
  into a private `float blendScore(const NgramLM::Context &state, uint32_t index) const`. `predict()` keeps calling it,
  so behaviour is unchanged.
  - `scoreWords` brings the neural state up to date for `context`, using the same cache code as `predict()`; factor that
    block into `void prepare(const std::vector<std::string> &context) const`.
  - It then maps each word to its vocabulary index and returns `blendScore`, or `floorLogProb()` for unknown words.
  - Without the neural model (`!useLm_`), return the n-gram `score` only.
  - WHY comment: swap ranks given words, so it needs their scores, not a top list.
- [ ] **Step 4:** Run the test (PASS), plus `ctest -R "engine_prediction|engine_nwp" -j4`. Expected: PASS.
  `predict()` must be unchanged.
- [ ] **Step 5:** Commit.

### Task 5: Swap types, ranking function and synthetic trace

**Files:**
- Create: `engine/include/swipekb/swap.hpp`, `engine/src/swap.cpp`, `engine/tests/test_swap.cpp`
- Modify: `engine/CMakeLists.txt` (add `src/swap.cpp` to `swipekb_engine` and the test target `engine_swap`)

**Interfaces:**
- Produces:
  ```cpp
  namespace swipekb {
  struct SwapCandidate {
      std::string word;        // display spelling
      float path = 0;          // beam ctc (nats); the tapped word's own path = the best
      float before = 0;        // n-gram log10 P(word | words before)
      float after = 0;         // n-gram log10 P(next 1-2 words | ..., word); 0 when none follow
      float neural = 0;        // Predictor::scoreWords (log10); 0 until the worker answers
      bool hasNeural = false;
  };
  struct SwapParams {          // weights tuned on the tuning sets (Task 8); defaults are the tuned English values
      float wPath = 1.f, wBefore = 1.f, wAfter = 1.f, wNeural = 0.f;
      int pool = 24;           // beam candidates considered
      int minFrequency = 80;   // same junk rule as AlternativeParams
  };
  // Best-first, at most topK, never `tapped` itself (case-insensitive). Score = wPath*path + ln(10)*(wBefore*before +
  // wAfter*after + wNeural*neural); with hasNeural false, the neural term is left out (not scored as 0).
  std::vector<std::string> rankSwap(const std::string &tapped, const std::vector<SwapCandidate> &cands,
                                    const SwapParams &p, int topK = 3);
  // A finger path through the centres of `word`'s keys (doubled letters once), 12 points per key gap, 20 ms apart,
  // so the beam can find words whose path looks like this word's. Empty when a letter has no key.
  std::vector<TracePoint> syntheticTrace(const std::string &word, const std::vector<KeyRect> &keys);
  }
  ```
- [ ] **Step 1: Failing tests** `engine/tests/test_swap.cpp` (GoogleTest):
  ```cpp
  TEST(Swap, RankNeverReturnsTappedWordAndOrdersByScore) {
      std::vector<SwapCandidate> c{{"good", 0, -1}, {"food", -1, -1}, {"Good", 0, -1}, {"hood", -3, -1}};
      const auto r = rankSwap("good", c, SwapParams{}, 3);
      ASSERT_EQ(r, (std::vector<std::string>{"food", "hood"}));
  }
  TEST(Swap, AfterWordsChangeTheOrder) {
      // "their" vs "there" with the same path: the words after decide.
      std::vector<SwapCandidate> c{{"there", -1, -2, -4}, {"their", -1, -2, -1}};
      EXPECT_EQ(rankSwap("x", c, SwapParams{}, 1).front(), "their");
  }
  TEST(Swap, MissingNeuralIsNotZero) {
      SwapParams p; p.wNeural = 1;
      std::vector<SwapCandidate> c{{"a", -1, -1, 0, -3, true}, {"b", -1, -1, 0, 0, false}};
      EXPECT_EQ(rankSwap("x", c, p, 1).front(), "b");   // b is not penalised for having no neural score
  }
  TEST(Swap, SyntheticTraceVisitsKeyCentres) {
      const auto keys = qwertyRects(0, 0, 1);   // copy the helper from test_language_engine.cpp
      const auto t = syntheticTrace("see", keys);
      ASSERT_FALSE(t.empty());
      // qwertyRects(0,0,1): 's' = row 1 (shift 50), index 1 -> left 150, centre 200; 'e' = row 0, index 2 -> centre 250.
      EXPECT_NEAR(t.front().x, 200.f, 1); EXPECT_NEAR(t.back().x, 250.f, 1);
      EXPECT_NEAR(t.front().y, 150.f, 1); EXPECT_NEAR(t.back().y, 50.f, 1);
      EXPECT_TRUE(syntheticTrace("s1", keys).empty());
  }
  ```
- [ ] **Step 2:** Build `test_swap`. Expected: fail to compile.
- [ ] **Step 3: Implement** `swap.cpp`.
  - `rankSwap`: `stable_sort` by score. The case-insensitive comparison is a lowercase Qt compare (QString), and
    duplicates (case-insensitive) are dropped.
  - `syntheticTrace`: match letters on `KeyRect::letter` after lower-casing (`QChar::toLower`). Interpolate linearly
    between successive key centres.
- [ ] **Step 4:** `ctest --test-dir build -R engine_swap -j4`. Expected: PASS.
- [ ] **Step 5:** Commit.

### Task 6: `LanguageEngine::swapCandidates`

**Files:**
- Modify: `engine/include/swipekb/language_engine.hpp`, `engine/src/language_engine.cpp`
- Test: `engine/tests/test_swap.cpp` (add)

**Interfaces:**
- Consumes: `SwapCandidate`, `syntheticTrace` (Task 5).
- Produces:
  ```cpp
  // Candidates to swap `word` with, scored on path and the n-gram (before/after), not yet ranked (rankSwap()).
  // `trace`: the swipe that produced the word, empty for a typed word or forgotten swipe (a synthetic path through the
  // word's keys is used). `after` may be empty (no words follow, or the field may not be read past the word).
  std::vector<SwapCandidate> swapCandidates(const std::string &word, const std::vector<KeyRect> &keys,
                                            const std::vector<std::string> &before, const std::vector<std::string> &after,
                                            const std::vector<TracePoint> &trace, const SwapParams &p) const;
  ```
  It is `const` but uses the beam's scratch space. Follow how `recognize` does it: if `recognize` is non-const because of
  scratch space, make this non-const too and say why.

- [ ] **Step 1: Failing tests:**
  ```cpp
  TEST(LanguageEngineSwap, TypedWordGetsPathLookalikes) {
      // With the real English files (skip when SWIPEKB_ASSETS is unset, like test_language_engine.cpp does):
      // "good" typed, no trace: "food" or "hood" must be among the candidates, and "good" itself must not.
  }
  TEST(LanguageEngineSwap, SavedTraceGivesTheSwipesRunnerUps) {
      // traceThrough("were") through the engine; swapCandidates("were", keys, {"we"}, {"here"}, trace) contains "where"
      // or "we're" (other spelling), and every candidate has path <= the best path.
  }
  TEST(LanguageEngineSwap, AfterIsZeroWithoutFollowingWords) { /* every candidate's after == 0 when after is empty */ }
  ```
  Write them in full, using `traceThrough` / `qwertyRects` from `test_language_engine.cpp`. Move both helpers into
  `engine/tests/testkeys.h` so both test files share them.
- [ ] **Step 2:** Run the tests. Expected: fail to compile.
- [ ] **Step 3: Implement.**
  - `t = trace.empty() ? syntheticTrace(word, keys) : trace`.
  - Do the same prepare and normalise as `recognize`, using a shared private helper extracted from `recognize`
    (lines ~363–381) so the code is not copied.
  - `decode(pts, aspect, p.pool)` → for each found word except `word` itself, keep it when it passes
    `minFrequency` / stretched / name rules, using the same vouched rule as `recognize`; extract that into a helper.
  - Add `otherSpellings(word)` with `path` = the best path.
  - `before` = `lm.logProb(lm.context(beforeForms), form(w), freq)`.
  - `after` = `logProb(context([..before, w]), after[0]) + logProb(context([..before, w, after[0]]), after[1])`, using
    as many as exist. Without an n-gram, `before` = `freqWeight*freq / ln10` and `after` = 0.
  - Time it in the test with 1,000 words: budget ≤ 20 ms (assert ≤ 60 ms in the test to avoid flakiness on CI).
- [ ] **Step 4:** `ctest --test-dir build -R "engine_swap|engine_language" -j4`. Expected: PASS.
- [ ] **Step 5:** Commit.

### Task 7: Plugin data path — keep traces, a scoring job

**Files:**
- Modify: `plugin/wordswap.h/.cpp` (`RecentSwipes::remember/candidatesFor/ownedAt` carry
  `std::vector<swipekb::TracePoint> trace` and `std::vector<swipekb::KeyRect> keys` in place of `QStringList candidates`;
  `Owned` the same), `plugin/nextwordworker.h/.cpp`, `plugin/tests/tst_wordswap.cpp`
- Leave `WordSwap::build` in place: the bench baseline uses it. Mark it `// Baseline only (bench); the keyboard uses swipekb::rankSwap.`

**Interfaces:**
- Produces:
  - `RecentSwipes::Item { tail, whole, word, trace, keys, context }`; `candidatesFor` → `traceFor(...)` returning
    `std::optional<std::pair<trace, keys>>`.
  - `PredictionJob::scoreWords` (QStringList); when non-empty, the worker calls `Predictor::scoreWords(context, words)`
    and emits a new signal `wordScoresReady(PredictionStamp, QStringList words, QVector<float> scores)`.
- Memory: 24 items × ≤ 300 points × 12 bytes, which is negligible. Still memory-only, never written to disk.
  Keep the WHY comment.

- [ ] **Step 1:** Update `tst_wordswap.cpp`'s `RecentSwipes` tests to the trace form: remember with a 3-point trace,
  `traceFor` returns it, and anchoring rules are unchanged. Add a worker test in the existing worker test file
  (`plugin/tests/tst_nextwordworker.cpp`): a `scoreWords` job emits `wordScoresReady` with the same count as asked.
- [ ] **Step 2:** Run `ctest --test-dir build -R "wordswap|nextwordworker" -j4`. Expected: FAIL.
- [ ] **Step 3:** Implement. Callers in `swipeinputmethod.cpp` and `swipecontroller.cpp` are updated in Task 9; keep
  them compiling now by passing the trace and keys where `remember` is called. At `swipeinputmethod.cpp:832` the trace
  and `rects` are in scope; at the other two calls (typed words) pass empty ones.
- [ ] **Step 4:** Run the tests. Expected: PASS. Build the `plugin` and `frontend-plasma69` targets: they must compile.
- [ ] **Step 5:** Commit.

### Task 8: New mode in the bench, and tuning

**Files:**
- Modify: `tools/swipekb-swap-bench.cpp`; create `bench/swap-tune.py`, and `bench/swap-params-{en,eu}.json` if the
  tuned values differ from the defaults.
- Modify: `engine/include/swipekb/swap.hpp` (defaults = tuned English values; add a `static SwapParams forLanguage(const std::string &lang)`).

**New mode, per swipe:**
- **AfterSwipe:** keep `candidates[0]` from `recognize` (the committed word; the first guess is unchanged).
  `shown = rankSwap(w, swapCandidates(w, keys, before, {}, trace, p) + neural scores, p, 2)`. This is what the
  post-swipe bar's other two places will show.
- **TapBackTrace:** `swapCandidates(w, keys, before, after, trace, p)` + neural → `rankSwap(..., 3)`.
- **TapBackNoTrace:** the same with an empty trace.
- **Neural:** from `Predictor::scoreWords(before, words)` when `--nwp` is given.
- `--params FILE` reads JSON `{wPath, wBefore, wAfter, wNeural, pool, minFrequency}`.

- [ ] **Step 1:** Implement `--mode new` and `--params`. Smoke run with `--limit 200`. Expected: all lines print and
  speed ≤ 20 ms/list.
- [ ] **Step 2:** `bench/swap-tune.py` runs a coordinate search over the weights on the **tuning set only**:
  - starting from 1s, with each weight in `{0, .25, .5, 1, 2, 4}` and pool in `{16, 24, 32}`;
  - objective: mean of the three in-3 rates, with junk % rising more than 1 point over baseline as a hard reject.
  - It refuses any `--set` other than `trv8k` or the FR/DE tuning slices. WHY: the test sets are never tuned on.
  - FR/DE tuning data: find FUTO swipe-5 validation slices under `~/.cache/swipe-keyboard-train/` (`fd -e jsonl` / grep
    `swipe5` in `docs/methods.md` and `scripts/swipe/`). If none exist in jsonl form, reuse English weights for all
    languages and say so in the result note.
- [ ] **Step 3:** Run the tuning. It may take ~30–60 min, so run it in the background and keep `-j` modest. Put the
  winners in `SwapParams` defaults / `forLanguage`, with a WHY comment giving the tuning set and date.
- [ ] **Step 4:** Commit.

### Task 9: Keyboard uses the new swap path

**Files:**
- Modify: `plugin/swipeinputmethod.cpp`:
  - `observeWordSwap` ≈ 1183–1252;
  - the alternatives handler ≈ line 91–96;
  - the post-swipe candidates after `recognize` ≈ 772–790.
- Modify: `frontend-plasma69/src/swipecontroller.cpp` (`observeWordSwap` ≈ 830–960 and its handler ≈ 100–110).
- Modify: `plugin/wordswap.h/.cpp`: `WordSwapSession::begin` takes the final ranked list (`QStringList`) in place of
  `(swipeCandidates, neighbours)`; `setRanked` becomes `setList`. The press-token and `stillValid` logic is unchanged.
- Tests: `plugin/tests/tst_wordswap.cpp`, and the Plasma 6.9 controller tests (`frontend-plasma69/tests/`, grep `swap`).

**Behaviour:**
1. **Tap-back:**
   - Read `before` as today: the owned context in OwnedHistory fields, otherwise `textContextWords(text before span)`.
   - `after` = the first 2 words after the span from the text, **only for RawBridge fields**. OwnedHistory: `{}`.
     WHY: stock fields may only be read for the keyboard's own words.
   - `trace` = `traceFor(...)`, if any.
   - `cands = m_engine->swapCandidates(...)`.
   - Then the unchanged tidy-up:
     - offensive filter (with the tapped-word rule);
     - `personalize(..., boost 0)`;
     - `rankRegionalCandidates`;
     - `WordSwap::matchCase` per word.
   - `list = rankSwap(span word, cands, SwapParams::forLanguage(lang), 3)` → `m_swap.begin(text, cursor, list)`. The
     list shows immediately.
   - Then a worker `scoreWords` job for `cands`' words. On `wordScoresReady`, matching the stamp with no finger down
     (today's guard at line ~104), fill `neural`, re-rank, and `m_swap.setList(...)`.
2. **Post-swipe:**
   - The committed word stays `candidates[0]` as today.
   - The bar's other places come from `rankSwap(candidates[0], swapCandidates(candidates[0], rects, context, {}, trace, p), p, 2)`,
     passed through the same tidy-up.
   - `withOtherSpelling` still puts "it's" first when it applies (unchanged rule).
   - The 3-option display order (strongest in the middle) is unchanged.
3. The same in `swipecontroller.cpp`, with the same code. Factor the shared part into one free function in
   `plugin/wordswap.cpp`: `QStringList tidySwapList(...)`, taking the engine, the profile snapshot and the
   personalisation callback. Both front ends call it, so no copy is added.

- [ ] **Step 1:** Update the tests first.
  - `tst_wordswap`: `begin` with a list; `setList` keeps the token rule.
  - `tidySwapList` keeps:
    - the offensive rule (both directions);
    - `matchCase` ("Good" → "Food", "GOOD" → "FOOD");
    - German noun capitals.
  - Plasma 6.9 controller tests: an OwnedHistory tap passes no `after`.
- [ ] **Step 2:** Run `ctest --test-dir build -R "wordswap|plasma69|swipeinput" -j4`. Expected: FAIL.
- [ ] **Step 3:** Implement items 1–3.
- [ ] **Step 4:** Run the same tests. Expected: PASS.
- [ ] **Step 5:** Commit.

### Task 10: Batch verification, scoreboards, owner test build, PR

This is the only task that runs the slow checks.

- [ ] **Step 1:** Full test suite: `ctest --test-dir build -j4 --output-on-failure`. Expected: all pass. A failing or
  flaky test gets fixed now, even if it predates this work.
- [ ] **Step 2:** `reuse lint` passes. Then `scripts/full-verify.sh` (say first that it loads the CPU for ~10–20 min).
  Expected: green.
- [ ] **Step 3:** Scoreboards:
  ```bash
  for s in bench-2000 test-extra-10000 heliboard-es heliboard-fr heliboard-de heliboard-en; do
    bench/swap-score.sh --mode new --set $s --label "one brain swap (tuned $(date +%F))" --check "baseline 0.5.0 (WordSwap::build)"; done
  bench/score.sh --label "one brain swap branch" --check   # swipe first guess unchanged: >= best earlier row
  ```
  The next-word scoreboard is unaffected: no next-word code changed except the `scoreWords` refactor. Task 4's test
  proves `predict()` is unchanged.
  If a swap measure is below baseline, go back to Task 8 rather than shipping.
- [ ] **Step 4:** Write `docs/active/investigations/2026-10-24-swap-results.md`: baseline vs new for every set and
  measure (first / in-3 / junk / ms), the tuned weights, and what is not measured.
- [ ] **Step 5:** `scripts/quick-test-build.sh`. Give the owner the printed install command; do not run it.
- [ ] **Step 6:** Push; open the PR "Tap-to-swap: real look-alike words, ranked by the sentence (card #500)" with a
  plain-words body and the results table. Do not merge.
