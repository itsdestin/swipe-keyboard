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
