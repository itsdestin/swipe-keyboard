# Context-aware search — design (step 1 of the accuracy push)

**Date:** 2026-10-14 · **Status:** approved direction (Destin: "measurement first, then sentence-aware search")
**Inputs:** a Gboard study report (withdrawn from the project 2026-10-23 at Destin's request; no later work
uses it); design study of FUTO's code.

## Findings that set the priority

FUTO `infer_jsonl` on `bench-2000.jsonl` (1,946 scored):

| Run | Top-1 | Target anywhere in returned list |
|---|---|---|
| Today (LM re-rank, beam 100) | 93.32% | 97.12% |
| LM off (`--alpha 0`) | 91.78% | – |
| Beam 400 | 93.47% | 97.89% |
| In-dictionary targets only (2.06% of swipes have targets missing from the dictionary) | 95.28% | 99.16% |

- ~3.8 points of top-1 are **ranking** losses (word in beam, not first); ~0.84 points are **pruning** losses.
- FUTO's context LM (`hungry_jellyfish`) is a bag-of-embeddings with a **16-dim** context vector, ~32k exact vocab +
  hash buckets for the rest. It is weak next to Gboard's n-gram + neural LMs → a **stronger LM** is the main lever;
  using it during pruning is secondary.

## How FUTO applies context today (file:line in third_party/swipe-library)

- LM only when `context_words` non-empty and alpha ≠ 0 (`engine.cpp:665-666`); that also switches the scoring row
  (`engine.cpp:669-674`): no-context gamma .5949 / lambda .0134 / beta .7271 / alpha 0; with context gamma .1126 /
  lambda .0060 / beta 2.2138 / alpha .6387 / gamma_prune .1902 / beta_prune 1.2727 (`models/scoring.json`, compiled in).
- Re-rank after the beam: `decode_top_k = max(top_k, 200)` (`engine.cpp:735`) but the final beam holds ≤100
  (`beam_search.cpp:379-386`); one LM pass + dot product per candidate (`engine.cpp:771`, `context_lm.cpp:432-463`);
  `final += alpha * raw_logit` (`context_lm.cpp:211`); sort, cut, force best raw-CTC word into the last slot
  (`engine.cpp:777-794`).
- Frequency term only at word end (`beam_search.cpp:443-446`); pruning uses CTC, timing, length only.
- No pruning hook in `ITrie` (`itrie.h:27-88`) → pruning-time priors need a small patch.

## Plan (each step = one scoreboard row; ship only if scoreboard AND owner replay don't regress)

0. **`tools/swipekb-bench`**: runs our `Recognizer` over the FUTO jsonl (key rects from `en_qwerty.json`, trace and
   keys ×1000 px so normalisation is identity); moves `contextWords` into the engine (shared with the plugin); prints
   the same good/ok/bad summary + beam recall, first-word top-1, OOV-LM top-1, avg µs. `score.sh --runner recognizer`.
   Log a parity row vs 93.32 / 96.56.
1. **Stronger LM for ranking.** An n-gram LM (2–3-gram, pruned to fit ~50–150 MB) built from an openly licensed,
   **non-Wikipedia** corpus (benchmark sentences are Wikipedia; using it would leak). Candidate: FineWeb (ODC-BY).
   Build tooling in `scripts/lm/`, artefacts git-ignored, licence recorded. Combine per candidate:
   `final = spatial + alpha * lp(word | context)`, with FUTO's re-rank off; back-off for words outside the n-gram
   via dictionary unigram. Keep the "best raw-shape word always in the list" guard.
2. **Prior inside the beam** (FUTO patch set in `patches/futo/`, applied idempotently before `add_subdirectory`):
   P1 optional `get_prior` in `ITrieVTable`; P2 add `mu * prior(node)` at the prune sites and `alpha * lm` at word
   end (spatial score stays separate — the pair); look-ahead `la[node]` = max lp below node (one reverse pass; child
   index > parent). Sweep `mu`, then narrower beams.
3. **Compute the prior on context change** (not on lift); first-word-of-sentence prior.
4. Later: user bigrams blended into lp; previous-word correction (may require owning the decoder — alternative B).

## Alternatives considered

- A. Word-end only via `get_log_frequency` override — no fork, misses pruning gains.
- B. Own the decoder (~600 lines) — full control; revisit for previous-word correction.

## Risks

Benchmark over-fit (tune on a separate slice, confirm on owner replay); over-strong prior suppressing rare clear
words (floor + raw-shape guard); patch drift on submodule bumps; memory (trie already ~119 MB); FUTO off-by-one at
the last LM vocab id (`context_lm.cpp:413, 78-80, 184`).
