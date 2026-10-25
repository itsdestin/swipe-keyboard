---
status: approved design (owner, 2026-10-24)
---
# One word brain: shared ranking for swipe, completion, next word and tap-to-swap

## Why

The keyboard has four word-pickers that rank words in four different ways:

| Feature | Ranks with | Uses the neural next-word model |
|---|---|---|
| Next word | `Predictor`: n-gram pack + `LANG.nwp.bin` | yes |
| Completion (typed prefix) | `Predictor` with a prefix | yes |
| Swipe | `SwipeEngine` + `NgramWordPrior` (n-gram only) | no |
| Tap-to-swap (`plugin/wordswap.cpp`) | up to 2 remembered swipe runner-ups, then one-edit neighbours (`LanguageEngine::similar`) by frequency, partly reordered by a `Predictor` query on the first letter | no |

Next word works well; swap does not. The swap list is mostly one-letter edits sorted by how common they are, but the real
swipe mix-ups are words with a similar *path* ("good" / "food" / "hood"). The swipe runner-ups are lost once the
text around the word changes. The words after the tapped word are never used. Nothing measures swap quality.

Two structural problems make this worse:
- The n-gram pack is loaded twice per language (once by `LanguageEngine`, once by `Predictor` in `NextWordWorker`).
- Ranking rules (learning boost, regional ranking, junk filter, other spellings) are applied in about 13 places in
  `plugin/swipeinputmethod.cpp`, and separately in `frontend-plasma69/` and `gaming/daemon/`.

## Design

### 1. Swap scoreboard (built first)

A new `tools/swipekb-swap-bench` scores swap lists through the code that ships. Its results go into
`bench/swap-scoreboard.tsv` via `bench/score.sh`.

**Data:**
- **English:** the real-swipe test sets (bench-2000, test-extra-10000). Each record's `sentence` + `word_idx` gives the
  words before and after.
- **Spanish, French, German:** HeliBoard test sets (CC BY-SA, test only, never trained on). Path measures only, since
  they have no sentences.
- **Tuning:** `trv8k` (held-back slice of the training split) only. Final numbers come only from the test sets.

**Measures.** Each is reported as "first option" and "in the 3", separately:
1. **After a swipe** (path saved, words before known): when the first guess was wrong, the target is in the swap list.
2. **Tap-back with saved path:** as 1, with the words after also known.
3. **Tap-back without path:** the wrong first guess is in the text with no trace; the target is in the list.
4. **Junk rate:** when the first guess was right, the share of shown options that are junk. Uses the same definition as
   `tools/swipekb-bench`.
5. **Speed:** milliseconds per list.

The first row is today's keyboard (baseline).

### 2. The brain

`swipekb::Ranker` (engine, MIT) scores candidate words:

```
score(w) = a·evidence(w)        how well w fits the path or the typed letters
         + b·before(w)          next-word model + n-gram, given the words before (one model run scores all candidates)
         + c·after(w)           n-gram (and the model for the top few) for the words after w; tap-back only
         + d·personal(w)        learning boost
```

- The weights (a, b, c, d) are tuned per mode (swipe, complete, next, swap) on the tuning data.
- If the model file is missing, `before` uses the n-gram; if that is missing too, it uses word frequency.
- A ranking never comes back empty when candidates exist.

**Gatherers** (each produces candidates plus their evidence, never the order):
- **Swipe:** the `SwipeEngine` beam, widened to ~30 candidates with their path scores.
- **Complete:** prefix matches + one-edit corrections (today's `complete()` logic, as evidence).
- **Next:** the model's / n-gram's top words (evidence = 0).
- **Swap:**
  - with a saved trace: re-decode the saved trace (runner-ups with path scores);
  - without one: path look-alikes, i.e. words whose ideal key path (key centres, via `swipe_geo`) is close to the
    word's own ideal path;
  - in both cases, other spellings (`spelling.cpp`).
  - The plugin's `RecentSwipes` keeps the trace instead of the candidate list.

**Tidy-up** (one place, after ranking): capitals and display spelling, other spellings, regional ranking, the junk filter,
and the offensive-word rule. Today's rules move here unchanged.

**Plug-in point:** `Ranker` is an interface. A future joint model (section 4) implements the same interface and is
judged by the same scoreboards.

### 3. One language object

- `swipekb::Language` owns the word list, n-gram pack, next-word model and swipe model, each loaded once.
- It answers `swipe(trace, keys, before)`, `complete(prefix, before)`, `next(before)` and
  `swap(word, before, after, trace?)`.
- Each call returns the final, tidied list.
- Thread rule as today: one instance per thread. The plugin's worker owns it, and the input method asks through the
  worker.
- All three front ends (`plugin/`, `frontend-plasma69/`, `gaming/daemon/`) call these four methods and keep only
  display and editor logic.

### Rollout

Each step is one PR, and the scoreboards run on each:
1. Swap scoreboard + baseline row.
2. `Ranker` and `Language` alongside the old code, compared on the scoreboards.
3. Swap on the new path; test build for the owner.
4. Completion and next word on the new path. No measure on `bench/scoreboard.tsv` may fall beyond noise (±1 point at
   300 sentences, stated per row).
5. Swipe on the new path. English first guess on bench-2000-futo-scorable with context must be at least 91.47.
6. Old code removed (`similar()`-based swap, the second n-gram load, the scattered ranking calls).

### 4. Improving accuracy, then the joint-model experiment

- **Tuning order:** swap, completion, next word (guard only), swipe (model in the prior; path vs. sentence balance).
- **Joint model experiment, time-boxed to 2–3 days after step 6:** a small model reading the path and the previous
  words, behind the `Ranker` interface. It is kept only if it wins on the scoreboards without unacceptable size,
  memory or speed. Otherwise it is written up and dropped.

## Testing

- Engine unit tests: each gatherer, the ranker's fallbacks (missing model, missing n-gram), and swap with and without a
  trace.
- Plugin tests carried over from tap-to-swap (`wordswap`).
- `scripts/full-verify.sh` before each PR, a VM run (Plasma 6.8 and 6.9), and a test build for the owner after steps 3
  and 6.

## Out of scope

Bigger word lists (0.5.1, on hold), new languages, and changes to the suggestion bar's look (it stays at 3 options).
