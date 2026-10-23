---
status: draft
cards: 179
---
# Own swipe engine: small CTC network + lexicon beam search + n-gram prior

Written without reference to FUTO code, weights or documents. Sources: published literature only — CTC (Graves et
al. 2006), gesture-typing with neural encoders + lexicon search (Alsharif et al. 2015 and follow-ups), SHARK2
(Kristensson & Zhai 2004), DTW — plus our own clean-room geometric spike (82.0% first / 90.0% top 3 on the 2,000 real
English test swipes; kept as fallback). Training data: FUTO's MIT-licensed real English swipe recordings (data only)
and our own synthetic swipes. Not used for training: the HeliBoard gesture set (CC BY-SA; test only, later).

Decision (owner, 2026-10-22): build the accurate architecture. Why not "geometric candidates + learned re-ranker":
measured candidate recall caps it near 93% first-guess even with a perfect re-ranker (see the recall note in the
session log), mostly swipes whose end or length the geometry cannot place.

## 1. Pipeline

1. **Trace -> input** (`SwipeNet::buildInput`, mirrored in `scripts/swipe/swipelib.py:trace_to_input`; a fixture test
   keeps them identical). Convert to key-width units, resample to **64 steps by arc length**, carrying elapsed time.
   Per step 8 features: x, y (0..1 of the letter area), dx, dy (key widths from the previous step), log(1+dt)/4
   (a pause shows as one step with a large dt), fraction of the path done, step length, log(1+path length)/3.
2. **Encoder.** Conv1d(8->64,k5) -> Conv1d(64->64,k5) (ReLU) -> 2-layer bidirectional GRU (hidden 96). ~290k
   parameters, 1.2 MB as float32 (0.6 MB as float16). Config (widths, layers) is stored in the file, so a bigger model
   needs no code change.
3. **Layout-independent output ("pointer head").** For step t and key k a small shared network scores
   `w . tanh(A h_t + B [dx, dy, key width, key height])`, where (dx, dy) is the finger's offset from the key centre in
   key widths and h_t the encoder state; the blank scores from h_t alone. Softmax over (blank + keys). Keys are inputs,
   not outputs, so AZERTY / QWERTZ / Spanish n-tilde / any key count use the same weights. (Fine-tuning per language
   is possible but not required.)
4. **Training.** CTC loss on the *collapsed* key sequence of the word (a double letter is one pass over a key; "see"
   and "se" share a label — the dictionary frequency and the sentence prior pick between them). AdamW, one-cycle LR,
   random whole-trace shifts (~0.2 key). Any target word works (no dictionary needed), so names/typos also train.
5. **Search** (`SwipeBeamSearch`). Dictionary words form a trie over key sequences. CTC prefix beam search over
   the trie: only prefixes of real words extend, so the output is always a dictionary word. Beam 48, only the 8 likeliest
   keys per step may start a letter. Pruning order = prefix probability + 0.03 nats * best word frequency below the
   prefix (+ a bonus for words the sentence context favours). At the end, each finished word scores
   `log P_ctc(keys) + prior(word)`.
6. **Prior** (`NgramWordPrior`, `swipe_lm.hpp`): natural-log probability from the project's own n-gram file given the
   previous words, falling back to the model's frequency calibration for words it lacks. Without a file the dictionary
   frequency (0..255 scale * 0.03 nats) is used. The previous-words hook is `setContext()`; `hints()` feeds the search.
7. **Fallback.** No model file, damaged/unknown-version file, or an empty result: the geometric decoder from the spike
   (banded DTW on key positions + letter coverage + frequency) answers instead. Swiping never depends on the network.
   (C++ port of the geometric decoder is not written yet; the spike is Python.)

## 2. Interfaces (engine/include/swipekb/)

- `swipe_layout.hpp` — `SwipeLayout` (keys as centre + half size in 0..1; `keyOf`, `wordKeys`; accents fold to the base
  key unless the layout has the accent as its own key; apostrophes, hyphens, digits are not keys).
- `swipe_net.hpp` — `SwipeNet::load/loadBytes` (strict validation), `buildInput`, `forward` (log-probs, blank = 0).
- `swipe_beam.hpp` — `SwipeLexicon` (`add`/`loadTsv`/`finish`; `word<TAB>freq`, pluggable list), `SwipeBeamSearch::decode`.
- `swipe_lm.hpp` — `NgramWordPrior` over `NgramLM`.
- Model file "SKNN" v1 (little endian): header (version, f32/f16, steps, feature counts, conv/GRU/pointer sizes), then
  tensors in the order written by `scripts/swipe/swipelib.py:export_model`. ~1.2 MB (f32) or ~0.6 MB (f16).

## 3. Tools and data

`scripts/swipe/`: `prep_data.py` (jsonl -> npz), `train.py` (CPU or CUDA), `synth_swipes.py` (our own generator;
differs from `bench/languages/make-noisy-swipes.py`, which stays the *test* generator), `make_layouts.py` (approximate
AZERTY/QWERTZ/Spanish layouts until real rectangles are taken from the keyboard), `make_fixture.py`.
`tools/swipekb-swipenet-bench.cpp` scores jsonl swipes (first guess, top 3, ms/swipe, single thread).
Splits: dev.jsonl (54k) trains; devhold-5000 selects; tune sets tune search weights; bench-2000 and test-extra-10000
are scored once at the end.

## 4. Other layouts and alphabets

Layout = key list. Accents map to base keys unless the layout has them (es n-tilde). Constants are in key-width units
(`unit = 1 / (2 * median half width)`). English is the only language with real recordings; for ES/FR/DE the plan is
(a) synthetic swipes on the real layout geometry for training/fine-tuning, (b) scoring on the project's synthetic set
and on real volunteer swipes when they exist. Accent twins (que/que-acute, ou/ou-grave) are a sentence-prior problem,
not a shape problem. The synthetic generator exists so the language gap can be closed without waiting for data.

## 5. Targets and budgets

English >= 93.3% first / 96.6% top 3 on bench-2000; <= ~5 ms/swipe single thread (measured 3.5 ms with the 290k
model in a loaded machine, -O3 without -march=native); model <= ~4 MB; lexicon trie ~533k nodes for 232k words.

## 6. Results (clean validation; updated 2026-10-23)

**Correction.** `devhold-5000.jsonl` and `tune-*.jsonl` are subsets of `dev.jsonl` (100% of ids and sessions), so
every earlier "devhold" figure (including 91.5% for the first CPU model and 93.0% for gpu1) was measured on training
swipes and is not a held-out number. Clean data: `bench-2000`, `test-extra-10000` and `test.jsonl` share no ids or
sessions with dev. Validation now = 10% of dev's *sessions* (`scripts/swipe/split_by_session.py`, 6,637 swipes, never
trained on); a second check is a 3,000-swipe diagnostic sample of test.jsonl (excluding bench/test-extra ids;
never trained on or tuned against). bench-2000 and test-extra-10000 are final scoring only.

Sweep on the clean split (first guess / top 3, all swipes; search: freq weight 0.06; "+geo" = geometric cross-check,
weight 30). Same training set for all: dev-train (47.6k real swipes, repeated 3x) + 120k synthetic English.

| run | recipe | params | val first/top3 | clean3000 first/top3 |
|---|---|---|---|---|
| R1 | h128 x2 layers, conv 96, no regularisation | 529k | 89.17 / 94.02 | 89.33 / 94.27 |
| R2 | R1 + dropout 0.2 + board-geometry aug + speed aug | 529k | 90.19 / 94.65 | 90.47 / 94.90 |
| R3 | R2, real x2, 240k synthetic, dropout 0.3 | 529k | 89.81 / 94.30 | 89.83 / 94.63 |
| R4 | R2 recipe, h96 x2, conv 64 (small) | 290k | 90.16 / 94.40 | 90.47 / 94.87 |
| R4 + geo | geometric cross-check weight 30 | 290k | 90.87 / 94.77 | 90.87 / 94.87 |

Findings: capacity is not the limit (290k = 529k); regularisation and augmentation are (+1.0); more synthetic data
at the cost of real share hurts (R3); the geometric cross-check adds +0.7 on clean validation and +3-4 points on
HeliBoard swipes (a different population). In-dictionary only (97.5% of clean3000): R4 + geo = 93.23 / 97.33.
The coordinator's FUTO figure on clean3000 is 92.5 / 96.3 over the 2,909 swipes it can score. Speed R4 + geo: 3.0 ms
per swipe on one thread (loaded laptop), model 1.16 MB (f32).

Notes: unit = key *pitch*, not drawn key width (boards with gaps between keys are 20% off otherwise). Two common
HeliBoard English boards with near-identical geometry differ by 5-7 points for *every* decoder (geometry only too):
the spread is the swipers, not the layout handling. The geometric decoder alone beats the network on HeliBoard top-3,
which is why the blend is part of the engine.

### Multilingual model and final scoring (2026-10-23)

One model for en/es/fr/de (keys as positions): M1 = R2 recipe + 100k synthetic swipes each for es (qwerty + n-tilde),
fr (azerty), de (qwertz) from `synth_swipes.py` on `release-v2/*.combined` words (case-folded labels, accents on base
keys). English did not drop: clean validation 90.60 / 94.74 (R2: 90.19 / 94.65), clean3000 90.63 / 94.57.
F1 = the same recipe trained on ALL of dev.jsonl (validation sessions included, last epoch kept), 2.1 MB, sha256
`287b531c...cce382b05`. Settings fixed before scoring (tuned on the clean validation): freq weight 0.06, geometric
cross-check weight 40, beam 48. Scored once, single run each:

| set | all swipes first / top-3 | in-dictionary only | ms/swipe |
|---|---|---|---|
| bench-2000 | 89.85 / 94.25 | 91.78 / 96.27 (1,958 swipes) | 3.5 (1 thread) |
| test-extra-10000 | 90.41 / 94.88 | 92.43 / 97.00 (9,781 swipes) | 3.6 |

FUTO reference on bench-2000: 93.32 / 96.56. Target (93.3 / 96.6) not reached; gap 3.5 first guess over all swipes,
1.5 over in-dictionary swipes, 0.3 on top-3 in-dictionary.

HeliBoard gesture test sets (test only, 5,000 each, case-insensitive, accents kept, per-swipe real board geometry),
F1 + geometric cross-check, 260k-word lists built from the open corpus counts (`words_from_counts.py`):
en 81.5 / 91.5 (words.tsv), es 81.1 / 89.7, fr 70.4 / 84.6, de 72.0 / 80.5 (dictionary coverage of targets
es 97.5%, fr 98.2%, de 91.9%). The 100k `.combined` lists cover only es 90.5%, fr 83.0%, de 80.0% of these targets,
which caps accuracy; with them F1-class models score es 77.9 / 85.4, fr 60.9 / 73.5, de 65.3 / 72.3. Today's FUTO
engine (old packs): en 84.5 / 93.4, es 77.7 / 85.1, fr 64.6 / 76.9, de 69.7 / 76.4 -- a different dictionary, so
compare with care.

### FUTO public collections (swipe-1 train, swipe-2..5; MIT data) and the final model T1 (2026-10-23)

Downloaded to the training desktop and filtered by `assemble_futo.py`: two-finger swipes and `distance` > 150 dropped
(invalid markers are 100001+). **Overlap check** against bench-2000, test-extra-10000, test.jsonl and the validation
sessions (56,005 swipe keys, 768 sessions): zero overlapping swipes or sessions in every collection (swipe-1 901,908
kept, swipe-2 23,774, swipe-3 34,769, swipe-4 43,909, swipe-5 50,507 -- no `dropped_*_overlap` counts occurred).
swipe-5 holds real multilingual swipes on 11 layouts (de/fr/es/en/pl/lt, dvorak, clearflow, kasroz...); its swipes are
usable for training (MIT). The collections store the sentence and each word's position, so previous words are known.

T1 = R2 recipe (529k parameters, 30 epochs, batch 512) on a ~317k-swipe session subset of swipe-1..4 (35% of
sessions) + dev-train + all swipe-5 groups (fr/de/es slices repeated 3x, own layouts) + 100k synthetic each es/fr/de.
No synthetic English. Trained once on the RTX 4070 Super: 16 min. Model 2.1 MB, sha256 prefix 941272a7.

| model | trv8k (7,198 clean swipes, new) | clean3000 | swipe-5 real fr / de / de-qwertz (own layouts, val slices) |
|---|---|---|---|
| F1 (dev-trained, previous) | 91.82 / 95.61 | 90.77 / 94.57 | 79.8 / 84.7 / 81.6 |
| T1 (+317k real, +real multilingual) | 92.19 / 95.79 | 90.77 / 94.90 | 83.2 / 90.2 / 83.2 |

Six times more real English barely moves the English clean numbers (+0.4 on trv8k, 0 on clean3000): English accuracy is
near what this model family reaches; the real multilingual swipes help the other languages a lot. Hence the full
~900k run was not done (agreed stopping rule).

Final scoring of T1 (frequency prior 0.06, geometric cross-check 40, beam 48; second scoring of these sets, for the
final chosen model only): bench-2000 90.40 / 94.95 all swipes, 92.34 / 96.99 in-dictionary (1,958); test-extra-10000
90.77 / 94.97 all, 92.80 / 97.10 in-dictionary (9,781); 3.5 ms/swipe, one thread. clean3000 in-dictionary 93.13 / 97.37
(FUTO 92.5 / 96.3 on the 2,909 swipes it scores). HeliBoard (test only, 260k lists; case-insensitive; all 5,000): en 82.1 / 91.5,
es 81.1 / 89.8, fr 71.7 / 85.9, de 73.3 / 82.3.

**Sentence-context prior** (previous words rebuilt from `sentence` + `word_idx`; `NgramWordPrior` with
`release-v2/en.ngram`, weight 1.0, `swipekb-swipenet-bench --ngram ... --context`): on trv8k T1 goes from 92.19 / 95.79
(frequency prior) to 93.10 / 95.92 first/top-3 (in-dictionary 95.28 / 98.17): +0.9 first guess; plain-unigram n-gram
prior without context is 0.3 worse than the frequency prior. This lever only exists when the keyboard knows the
previous words.

## 7. In the keyboard plugin (opt-in, 2026-10-23)

`plugin/ownswipe.{h,cpp}` is the glue; FUTO stays the default. On: an empty file `own-engine` in the keyboard data dir
(or env `SWIPEKB_ENGINE=own`, which wins, for tests); read once per profile resolve. Model: `swipe-model.bin` in the
model dir (override `SWIPEKB_SWIPE_MODEL`); unpinned, test-only. Inputs come from what the plugin already has: layout
from the live key rectangles of each swipe (letters sorted so key numbers stay stable; a resize only moves keys), words
from the profile's verified `.combined` (lower-cased except German), sentence prior from the profile's n-gram pack with
the same context words FUTO gets. No usable model = geometry-only; any refusal or exception = FUTO answers that swipe.
Candidates then go through the existing regional ranking, other-spelling and guess-bar code. Diagnostics:
`swipeEngine` (futo | own | own-geometry), `ownEngineSwipes`, `ownEngineFallbacks` (counts only).
