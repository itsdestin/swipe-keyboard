---
status: approved design (owner, 2026-10-24), revised after independent review the same day
---
# One word brain: shared ranking for swipe, completion, next word and tap-to-swap

## Why

The keyboard has four word-pickers that rank words in four different ways:

| Feature | Ranks with | Uses the neural next-word model |
|---|---|---|
| Next word | `Predictor`: n-gram pack + `LANG.nwp.bin` | yes |
| Completion (typed prefix) | `Predictor` with a prefix | yes |
| Swipe | `SwipeEngine` + `NgramWordPrior` (n-gram only, `swipe_lm.hpp`) | no |
| Tap-to-swap (`plugin/wordswap.cpp`) | up to 2 remembered swipe runner-ups, then one-edit neighbours (`LanguageEngine::similar`) by frequency, partly reordered by a `Predictor` query on the first letter | no |

Next word works well. Swap does not, for three reasons:
- The swap list is mostly one-letter edits sorted by frequency, but the real swipe mix-ups are words with a similar
  *path* ("good" / "food" / "hood").
- The swipe runner-ups are lost once the text around the word changes, and the words after the tapped word are never used.
- Nothing measures swap quality.

The duplication is structural:
- `plugin/swipeinputmethod.cpp` applies the ranking rules (learning, regional ranking) in 12 places.
- `frontend-plasma69/src/swipecontroller.cpp` (around lines 860–960) holds a near copy of the swap flow.
- `gaming/daemon/server.cpp` (lines 512–563) has its own recognise/complete/regional ranking, and no swap.

The n-gram pack is opened twice per language (by `LanguageEngine` and by `Predictor`). The file is memory-mapped
(`engine/src/ngram.cpp:130`), so only the per-instance tables are duplicated. That is tidiness, not a memory win, and it
is not a goal.

## Design

### 1. Swap scoreboard (built first)

A new `tools/swipekb-swap-bench` scores swap lists through the shipped code. Its results go into
`bench/swap-scoreboard.tsv` via `bench/score.sh`.

**Data:**
- **English test:** bench-2000 and test-extra-10000 (real swipes; `sentence` + `word_idx` give the words before and after).
- **ES/FR/DE test:** HeliBoard (CC BY-SA, test only). It has no sentences, so only measures 1 and 3 apply there, without
  sentence context.
- **Tuning:**
  - English: `trv8k` (held-back training slice, with sentences).
  - French and German: FUTO `swipe-5` validation slices.
  - Spanish: shares the French/German weights.
  - The test sets, including the bench-2000-futo-scorable guard row, are never used for tuning.

**Measures.** Each is reported as "first option" and "in the 3" separately, with the number of cases (n):
1. **After a swipe** (trace and words before known): among swipes whose first guess was wrong, the target is in the list.
2. **Tap-back with saved trace:** as 1, with the words after also known. This applies to raw-bridge fields only;
   stock Plasma fields (OwnedHistory) never see the words after.
3. **Tap-back without trace:** the text holds the engine's actual wrong first guess and no trace is available; the target
   is in the list. These cases are by construction the engine's own misses, which are harder than a random wrong word.
4. **Junk rate:** when the first guess was right, the share of shown options that are junk (the definition in
   `tools/swipekb-bench`).
5. **Speed:** milliseconds per list.

**Baseline:**
- The baseline row calls today's `WordSwap::build` itself, not a re-implementation, with the candidate list that
  `recognize()` returns under the shipped alternative parameters.
- It keeps today's quirks: at most 2 swipe candidates, the first-letter reorder, and the middle-slot swap when there are 3
  options.

### 2. The brain: one ranking function inside `LanguageEngine`

No new `Ranker` interface or `Language` class. `swipekb::LanguageEngine` is already the one facade every front end uses,
so the brain is a ranking step inside it:

```
score(w) = path/letter evidence(w)   from the gatherer
         + before(w)                 next-word model + n-gram, given the words before (one model pass scores the whole vocabulary)
         + after(w)                  n-gram only, for the next 1–2 words; tap-back in raw-bridge fields only
         + personal(w)               learning boost (weight 0 for swap: today it only drops forgotten words)
```

- Weights are tuned per feature.
- Missing model: `before` uses the n-gram. Missing n-gram as well: word frequency.
- A ranking is never empty when candidates exist.
- `after` uses the n-gram only: the model would need one pass per candidate, and its cache does not carry across
  candidates (`nwp_predictor.cpp:79`). It is extended to the model only if a measurement shows a gain within the speed
  budget.

**Gatherers** (each produces candidates plus evidence, never the order):
- **Swipe:** the `SwipeEngine` beam with path scores (as today).
- **Complete:** prefix matches + one-edit corrections (today's `complete()`).
- **Next:** the model's / n-gram's top words.
- **Swap:**
  - With a saved trace, the beam re-decodes it.
  - Without one, the beam decodes a synthetic trace drawn through the word's key centres. This yields path look-alikes
    with no new dictionary index, and measures 1–3 go through one code path.
  - In both cases, `otherSpellings()` (its / it's) is added.
  - `RecentSwipes` keeps the trace instead of the candidate list.

**Threading:**
- One `LanguageEngine` per thread, as today. The worker thread already owns the model for next-word.
- Swipe stays synchronous on the input-method thread, so the inserted word never waits on the worker queue.
- Putting the model into swipe ranking (step 5) means a second model instance on the input thread (about 5 MB per
  language). It is done only if it keeps a full swipe under 15 ms and wins on the scoreboard. Otherwise swipe keeps the
  n-gram prior.

**Speed budgets:** swipe ≤ 15 ms, swap list ≤ 20 ms, completion/next as today (≈3–5 ms).

**Tidy-up** (one place, after ranking), with rules moved unchanged:
- `matchCase` (all caps, first capital, German nouns keep theirs);
- display spelling and capitals;
- apostrophe forms (`otherSpellings`, the model's apostrophe handling);
- regional ranking and the junk filter;
- the offensive-word rule, which takes the tapped word as input (`offeredUnprompted`: a person's own blocked word may
  have blocked neighbours).

### 3. One swap flow for both Plasma front ends

- `RecentSwipes`, `WordSwapSession` and `CursorMoveTracker` move from `plugin/wordswap.*` into a shared library used by
  both `plugin/` and `frontend-plasma69/`. The near copy in `swipecontroller.cpp` is deleted.
- The press-token protocol (`WordSwapSession::take`) stays: Plasma 6.9 has no preedit, so the list under the finger
  must be the one that was pressed.
- The privacy rules stay. In stock Plasma fields (OwnedHistory) swap offers only the keyboard's own words, and the
  sentence is the remembered one.
- The 12 scattered ranking calls in `swipeinputmethod.cpp` become calls to the engine's ranking.
- **Gaming Mode daemon:** its request/response protocol stays frozen. It switches to the engine's ranking, and gets no
  swap (it has no swap UI).

### Rollout (small steps, scoreboards on each)

1. Swap scoreboard + baseline row.
2. **Swap fix:** trace kept, beam-based look-alikes, ranking with `before` / `after`, shared swap flow for both Plasma
   front ends. Test build for the owner. This is the owner's priority and stands on its own.
3. Completion and next word through the same ranking step. No measure in `bench/scoreboard.tsv` may fall beyond noise
   (±1 point at 300 sentences).
4. Scattered ranking calls removed from the front ends; the daemon on the engine's ranking.
5. Swipe with the model in the prior: conditional, only if it meets the 15 ms budget and wins. English first guess on
   bench-2000-futo-scorable with context must stay ≥ 91.47.

### 4. Improving accuracy, then the joint-model experiment

- **Tuning order:** swap, completion, next word (guard only), swipe.
- **Joint model:** a small model reading the path and the previous words, time-boxed to 2–3 days after step 4. It sits
  behind the same `LanguageEngine` ranking step, so no interface is built for it in advance. It is kept only if it wins on
  the scoreboards with acceptable size, memory and speed.

## Testing

- **Engine unit tests:**
  - each gatherer, including the synthetic-trace look-alikes;
  - ranking fallbacks (missing model, missing n-gram);
  - swap with and without a trace;
  - the `after` term skipped when no words follow.
- **Plugin and Plasma 6.9 tests:** today's tap-to-swap tests run against the shared swap flow, including the press token,
  OwnedHistory, case matching and blocked words.
- **Daemon protocol tests** unchanged.
- `scripts/full-verify.sh` before each PR, a VM run (Plasma 6.8 and 6.9), and owner test builds after steps 2 and 4.

## Out of scope

Bigger word lists (0.5.1, on hold), new languages, the suggestion bar's look (it stays at 3 options), and swap in Gaming
Mode.
