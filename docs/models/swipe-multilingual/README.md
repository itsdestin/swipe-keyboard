---
license: mit
language:
- en
- es
- fr
- de
library_name: custom
pipeline_tag: other
tags:
- swipe-typing
- gesture-typing
- keyboard
- on-device
- ctc
- gru
- swipe-keyboard
model-index:
- name: swipe-multilingual (T1)
  results:
  - task:
      type: other
      name: Swipe decoding, English, first guess right (all swipes)
    dataset:
      name: bench-2000 (2,000 real English swipes, FUTO MIT test split)
      type: custom
    metrics:
    - type: accuracy
      value: 90.40
      name: first guess right, all swipes
  - task:
      type: other
      name: Swipe decoding, English, right word in top 3 (all swipes)
    dataset:
      name: bench-2000 (2,000 real English swipes, FUTO MIT test split)
      type: custom
    metrics:
    - type: accuracy
      value: 94.95
      name: right word in top 3, all swipes
---

<!-- Draft. Not published. Numbers are traced to files in the swipe-keyboard repo in the HTML comments. -->

# swipe-multilingual ("T1"): swipe-typing decoder for English, Spanish, French and German

A small neural network that turns the path of a finger across an on-screen keyboard into a ranked list of words, for the Swipe Keyboard: an open-source swipe-typing and word-prediction add-on for KDE Plasma's on-screen keyboard. One model serves all four languages and any layout, because the keys are given to it as positions.

Models trained and released by Destin Moss via youcoded.ai, 2026.

Code, scripts and tests: https://github.com/itsdestin/swipe-keyboard. How every number below was measured: [methods page](../methods.md) (published copy: https://github.com/itsdestin/swipe-keyboard/blob/main/docs/methods.md).

**Status: an opt-in alternative engine.** In the keyboard it is used only when the user creates an `own-engine` file; FUTO's engine remains the default. On English it is **not yet as accurate** as FUTO's engine (see the numbers below). <!-- src: docs/active/specs/2026-10-23-own-swipe-engine.md section 7; docs/active/plans/2026-10-22-open-engine-roadmap.md "Phase 3 final scoring" -->

## What it is, and what it is not

- A 1-D convolution stack followed by a 2-layer bidirectional GRU, trained with the CTC loss (Graves et al., 2006) on the sequence of keys the finger passes over. Read from the shipped file's header: 2 convolution layers of width 96 (kernel 5), a 2-layer bidirectional GRU with hidden size 128, a pointer head of width 32, 64 time steps of 8 features, float32 weights. That is about **529,000 parameters** and a **2.1 MB** file (2,115,260 bytes). <!-- src: header of ~/.cache/swipe-keyboard-train/release-v3/swipe-model.bin read with struct.unpack (sha256 equals the pin in packaging/assets.lock.json); 529k from docs/active/specs/2026-10-23-own-swipe-engine.md section 6 (T1 = R2 recipe, 529k parameters); bytes from assets.lock.json trained_models.swipe.bytes -->
- Layout independent: the network does not have a fixed output per key. For every time step it scores each key from the finger's offset to that key (in key widths) plus the key's size, with one small shared scoring network ("pointer head"). A QWERTY, AZERTY, QWERTZ or Spanish keyboard with an n-tilde key therefore use the same weights.
- It outputs probabilities over keys, not words. Words come from a **lexicon beam search** over a word list (a trie of key sequences), so every answer is a word in the list, scored as network evidence + word frequency + the n-gram sentence prior when previous words are known + a **geometric cross-check** (a separate, classical shape-matching decoder, blended in with weight 40).
- **Fallback:** with no model file, a damaged one, or an empty result, the geometric decoder alone answers; swiping never depends on the network.
- It is **not** a handwriting or general gesture recogniser, and it cannot output a word that is not in the word list (words missing from the list are always misses; see limitations).

## Intended use

On-device swipe typing. The keyboard supplies: the finger path (x, y, time), the live rectangles of the keys, a verified word list for the language, and, when available, the previous words of the sentence. Settings used for all reported results: frequency weight 0.06, geometric cross-check weight 40, beam width 48. <!-- src: spec section 6 "Final scoring of T1 (frequency prior 0.06, geometric cross-check 40, beam 48 ..." -->

Speed on one CPU thread: about 3.5 ms per swipe (measured on a loaded desktop, plain `-O3` build). <!-- src: spec section 6, T1 final scoring "3.5 ms/swipe, one thread" -->

Not intended for: security-sensitive input, identifying people, or any use that stores or sends a user's swipes. The keyboard does not record swipes unless a user switches on a separate opt-in recorder.

## File format

File `swipe-model.bin`, read by our own plain C++ runner (`engine/src/swipe_net.cpp`, no machine-learning library). Magic `SKNN`, version 1, little-endian.

| Part | Content |
|---|---|
| Magic | the 4 bytes `SKNN` |
| Header (u32 each) | version (1); data type (0 = float32, 1 = float16); time steps (64); input features per step (8); key features (4); number of convolution layers; kernel size; GRU hidden size; GRU layers; pointer-head width |
| Convolution widths | one u32 per convolution layer |
| Tensors | in order: for each convolution layer weight [out][in][kernel] and bias; for each GRU layer and direction (forward, then reverse) input weights, recurrent weights and both biases (PyTorch gate order r, z, n); pointer head: A (weight, bias), B (weight, bias), w (weight, bias); blank head (weight, bias) |

Because the layer sizes are stored in the file, a bigger or smaller network needs no code change. The loader validates every header field against fixed ranges and refuses a file that is too short for the tensors it declares. **There is no checksum inside this file**, unlike the next-word models; integrity comes from the SHA-256 the keyboard pins (`941272a7017229a73e07d1c0bd766757e739174db3a42fb622c26256b8c46110`), checked before the engine loads it. A mismatch means geometry-only mode. <!-- src: engine/src/swipe_net.cpp loadBytes; scripts/swipe/swipelib.py export_model; packaging/assets.lock.json trained_models.swipe.sha256; packaging/language-models/README.md "Trained models" -->

Input per step (64 steps resampled by arc length): x, y, dx, dy, log elapsed time, fraction of the path done, step length, log path length. <!-- src: spec section 1 -->

## Training data and licences

| Source | What was taken | Licence |
|---|---|---|
| FUTO's public swipe collections (swipe.futo.org: `swipe-1` training split, `swipe-2` to `swipe-5`) | real swipes, English QWERTY; `swipe-5` also holds real German, French and Spanish swipes on their own layouts | MIT (Copyright (c) 2025 FUTO; text kept in `FUTO-SWIPE-DATA-LICENSE.txt`) |
| Our own synthetic swipes | 100,000 each for Spanish, French and German, generated by `scripts/swipe/synth_swipes.py` on approximate layouts; no synthetic English in T1 | ours; MIT |
| Word lists (used at run time, not part of the model file) | the benchmarks used 260,000-word lists counted from FineWeb / FineWeb-2 and Tatoeba; the keyboard plugin loads the language profile's verified `.combined` list instead (top 100,000 words for es/fr/de) | ODC-By 1.0 and CC BY 2.0 FR, see the language-pack notices |

What exactly went into T1: about 317,000 swipes from a session-level 35 % subset of `swipe-1` to `swipe-4`, `dev.jsonl` (54,000 English swipes from FUTO's MIT recordings; about 47,600 left for training after 10 % of its sessions were held out for validation), all `swipe-5` groups (French, German and Spanish slices repeated three times, each on its own layout), plus the synthetic swipes. 30 epochs, batch 512. <!-- src: spec section 6 "FUTO public collections ... and the final model T1" -->

**Never trained on:**
- `bench-2000`, `test-extra-10000` and `test.jsonl` (FUTO's MIT test split) and the validation sessions: an overlap check dropped any swipe whose id or session appeared in them; no overlapping swipe or session was found in any collection (56,005 swipe keys and 768 sessions checked). <!-- src: spec section 6 "Overlap check" -->
- The HeliBoard gesture test sets (CC BY-SA): used for **testing only**. No HeliBoard data is in the training set, which is why the model's licence is not share-alike.
- No FUTO model weights were used or fine-tuned; the architecture and code are our own, written from published literature (CTC, lexicon-constrained gesture decoding, SHARK2, dynamic time warping).

## Training procedure and hardware

- Script: `scripts/swipe/train.py` (PyTorch, CTC loss on the collapsed key sequence; AdamW, one-cycle learning rate). Recipe "R2": dropout 0.2, board-geometry augmentation and speed augmentation, random whole-trace shifts of about 0.2 key. <!-- src: spec section 1 item 4 and section 6 table (R2 = R1 + dropout 0.2 + board-geometry aug + speed aug) -->
- Hardware: one desktop NVIDIA RTX 4070 Super. **Training time for T1: 16 minutes** for 30 epochs. <!-- src: spec section 6 "Trained once on the RTX 4070 Super: 16 min" -->
- Model selection: search settings (frequency weight, cross-check weight, beam) were fixed on the clean validation split before scoring. `bench-2000` and `test-extra-10000` were scored once for the earlier model F1 and a second time for T1, the final model only; T1 was chosen using other data (trv8k, `swipe-5` slices, clean3000), see below. <!-- src: spec section 6 "second scoring of these sets, for the final chosen model only" -->
- Not recorded in the repository: the exact command line of the T1 run and the random seed (the script default is 1).

## Evaluation

All figures are percentages. "First guess" = the top suggestion is the target word; "top 3" = it is among the first three. **Denominators matter:** "all swipes" counts every swipe, including those whose target word is not in the word list (those can never be right); "in-dictionary" counts only swipes whose target is in the word list.

### English, real swipes (FUTO's MIT test split), scored once for the final model

| Test set | Swipes | All swipes: first / top 3 | In-dictionary: first / top 3 |
|---|---|---|---|
| bench-2000 | 2,000 (1,958 in-dictionary) | 90.40 / 94.95 | 92.34 / 96.99 |
| test-extra-10000 | 10,000 (9,781 in-dictionary) | 90.77 / 94.97 | 92.80 / 97.10 |
| clean3000 (3,000 swipes, diagnostic) | 3,000 | not stated in the source | 93.13 / 97.37 |

<!-- src: docs/active/specs/2026-10-23-own-swipe-engine.md section 6, "Final scoring of T1": bench-2000 90.40 / 94.95 all, 92.34 / 96.99 in-dictionary (1,958); test-extra-10000 90.77 / 94.97 all, 92.80 / 97.10 in-dictionary (9,781); clean3000 in-dictionary 93.13 / 97.37 -->

### Against FUTO's engine (English)

| Set | FUTO engine, first / top 3 | This model, first / top 3 | Denominator |
|---|---|---|---|
| bench-2000 | 93.32 / 96.56 | 90.40 / 94.95 | all 2,000 swipes (FUTO figure: all 2,000 through our recogniser harness) |
| bench-2000 | 93.32 / 96.56 | 92.34 / 96.99 | ours: in-dictionary swipes only; the FUTO figure is not restricted this way, so this row is not like-for-like |
| clean3000 | 92.5 / 96.3 (2,909 swipes FUTO's tool can score) | 93.13 / 97.37 | ours: in-dictionary only (97.5 % of the 3,000) |

<!-- src: FUTO bench-2000 93.32 / 96.56: spec section 6 "FUTO reference on bench-2000: 93.32 / 96.56" and bench/history.tsv row 370d6f5 '[recognizer] step 1: keyboard default' (93.3196 / 96.557). clean3000 FUTO 92.5 / 96.3: spec section 6 "The coordinator's FUTO figure on clean3000 is 92.5 / 96.3 over the 2,909 swipes it can score". ours: spec section 6 T1 final scoring -->

Plainly: on all swipes of bench-2000 the model is **2.9 points behind FUTO on first guess and 1.6 behind on top 3** (90.40 vs 93.32; 94.95 vs 96.56). On the swipes whose word is in the dictionary the gap on bench-2000 is about 1 point on first guess and 0 on top 3 (92.34 vs 93.32; 96.99 vs 96.56), but FUTO's number does not exclude out-of-dictionary swipes, so do not read that row as a win. The stated target (match FUTO's 93.3 / 96.6 on all swipes of bench-2000) was **not reached**. <!-- src: arithmetic on the numbers above -->

### Other languages: real swipes from HeliBoard (test only)

HeliBoard gesture test sets: 5,000 real swipes per language, case-insensitive, accents kept, per-swipe real board geometry, 260,000-word lists. First guess / top 3, all 5,000 swipes. "FUTO engine today" used the older, smaller word packs, so the two columns do **not** use the same dictionary and are not like-for-like.

| Language | This model (T1) | FUTO engine today (old packs) |
|---|---|---|
| English | 82.1 / 91.5 | 84.5 / 93.4 |
| Spanish | 81.1 / 89.8 | 77.7 / 85.1 |
| French | 71.7 / 85.9 | 64.6 / 76.9 |
| German | 73.3 / 82.3 | 69.7 / 76.4 |

<!-- src: spec section 6, T1 final scoring: "HeliBoard (test only, 260k lists; case-insensitive; all 5,000): en 82.1 / 91.5, es 81.1 / 89.8, fr 71.7 / 85.9, de 73.3 / 82.3". FUTO column: spec section 6 "Today's FUTO engine (old packs): en 84.5 / 93.4, es 77.7 / 85.1, fr 64.6 / 76.9, de 69.7 / 76.4 -- a different dictionary, so compare with care" -->

Dictionary coverage of the HeliBoard targets with the 260,000-word lists: Spanish 97.5 %, French 98.2 %, German 91.9 % (English: not stated for this list). With the older 100,000-word lists the coverage was Spanish 90.5 %, French 83.0 %, German 80.0 %, which caps accuracy. <!-- src: spec section 6 "HeliBoard gesture test sets" paragraph -->

Two common HeliBoard English boards with near-identical geometry differ by 5 to 7 points for every decoder tested, so the spread between rows is partly the swipers, not the layout handling. <!-- src: spec section 6 Notes -->

### Real multilingual swipes from FUTO's `swipe-5`, on their own layouts (validation slices)

| Model | French | German | German (QWERTZ) |
|---|---|---|---|
| F1 (previous model, English + synthetic only) | 79.8 | 84.7 | 81.6 |
| T1 (this model) | 83.2 | 90.2 | 83.2 |

First guess. <!-- src: spec section 6 table "F1 ... 79.8 / 84.7 / 81.6; T1 ... 83.2 / 90.2 / 83.2" --> The source calls these "val slices" of the collection; it does not describe how they were separated from training in that table, so treat them as a development comparison, not a final test.

### Synthetic Spanish, French and German (not human)

The synthetic noisy sets (`bench/languages/synthetic-noisy-v1-*.json`, 400 words each) were scored with the **older FUTO-engine keyboard**, not with this model: Spanish 89.00 / 98.25, French 83.25 / 93.00, German 87.75 / 97.00 (first / top 3). We have **no synthetic-set score for T1 in the sources**. Synthetic English calibration: real 92.8 / 96.4 against synthetic 92.3 / 96.0. These sets only cover dictionary words and assume people swipe other languages like English. <!-- src: bench/languages/README.md "Update 2026-10-22" and "Calibration" -->

### Sentence-context prior (experiment)

When the keyboard knows the previous words, the English n-gram prior lifts T1 on a 7,198-swipe set (trv8k) from 92.19 / 95.79 to 93.10 / 95.92 (first / top 3, all swipes; in-dictionary 95.28 / 98.17). That is +0.9 first guess. The set is a held-back 5 % slice of FUTO's `train.jsonl` that still has sentence text. <!-- src: spec section 6 "Sentence-context prior"; assemble_futo.py docstring ('trv' = 5 % validation with sentence text) --> Single-word swipes (bench-2000) carry no sentence, so the headline numbers above do not include this gain.

### Gboard

Gboard's swipe typing was **not measured**.

## Known limitations

- **English is about 1 to 3 points behind FUTO's engine** on single in-dictionary words and about 3 on all swipes; the target was not met. More real English data barely helped: six times more real English swipes moved the clean English numbers by +0.4 on one set and 0 on another, so we stopped (agreed stopping rule). <!-- src: spec section 6 T1 paragraph; roadmap Phase 3 final scoring -->
- **French and German are lower than English** on HeliBoard (71.7 and 73.3 first guess vs 82.1). The sources do not isolate the cause. One measured factor is dictionary coverage of the targets (German 91.9 % even with the 260,000-word list; compounds are the likely reason, not verified). The spread between swipers on the same layout is also large (5 to 7 points).
- **Accent twins** (Spanish que / qué, como / cómo; French ou / où) share one swipe path. The shape cannot decide between them; the sentence prior has to, and without previous words the choice is frequency only.
- **Short words (3 letters or fewer)** were the other common miss when the older FUTO-engine keyboard was scored on synthetic Spanish, French and German swipes; we have not repeated that breakdown for T1. <!-- src: roadmap Phase 0.5 results ("Main misses: accent twins ... and words of <=3 letters") -->
- **Out-of-dictionary words are always misses.** About 2.5 % of English targets are not in the word list (bench-2000: 42 of 2,000).
- **Spanish, French and German training is partly synthetic** (100,000 each) plus the real `swipe-5` slices. Only English has large real recordings. Real volunteers' swipes per language are still missing, so non-English numbers are less certain than English ones.
- **Layout handling is approximate where we had no real key rectangles.** The AZERTY/QWERTZ/Spanish layouts used for synthetic training are approximations (`make_layouts.py`); the real keyboard supplies live rectangles at run time. Key pitch, not drawn key width, is the unit, because boards with gaps between keys were 20 % off otherwise. <!-- src: spec section 4 and section 6 Notes -->
- **An earlier validation error was found and corrected** (see the incident below). Every number on this page comes from the corrected, clean sets.
- No bias or fairness evaluation was done. The training swipers are volunteers on FUTO's collection; their handedness, device and language mix are not documented here.

### The inflated-validation incident (2026-10-22)

For a day, the validation sets named `devhold-5000`, `tune-2000` and `tune-10000` were believed to be held out. They were not: all of their swipes were contained in `dev.jsonl`, the training set. Every "devhold" figure was therefore measured on training swipes, including 91.5 % for the first CPU model and the 93.02 / 96.07 that looked like a tie with FUTO for the first GPU model ("gpu1"). **Those numbers are retracted.** The swipe helper found it; the coordinator verified it (100 % of ids and sessions overlapped); the fix was to hold out whole recording sessions from `dev.jsonl` as validation (`scripts/swipe/split_by_session.py`) and to select only on that, keeping `bench-2000` and `test-extra-10000` as final-only sets. The honest first read on fresh swipes put the model 2 to 4 points behind FUTO, not level. <!-- src: docs/active/plans/2026-10-22-open-engine-roadmap.md "Correction (found by the swipe helper, verified 2026-10-22)"; spec section 6 "Correction"; scripts/swipe/split_by_session.py docstring -->

## How to reproduce

From a checkout of https://github.com/itsdestin/swipe-keyboard. You need FUTO's public swipe data (MIT, from swipe.futo.org) and a CUDA GPU; scoring needs the Qt build.

```
# 1. Collect the evaluation keys so training data can be checked against them
python -I scripts/swipe/holdout_ids.py holdout-final.json bench-2000.jsonl test-extra-10000.jsonl test.jsonl
python -I scripts/swipe/holdout_ids.py holdout-val.json <validation sessions>.jsonl
# 2. Filter FUTO's collections (drops two-finger and invalid swipes and anything overlapping the evaluation files)
python -I scripts/swipe/assemble_futo.py DATA_DIR OUT_DIR holdout-final.json holdout-val.json
# 3. Synthetic swipes for es / fr / de (own generator, different from the test generator)
python -I scripts/swipe/synth_swipes.py --words words.tsv --layout scripts/swipe/layouts/azerty.tsv --n 100000 --out synth-fr.jsonl
# 4. Convert and train (see the script header for all options; recipe R2 flags: --dropout 0.2 --geo-aug ... --time-aug ...)
python -I scripts/swipe/prep_data.py ...
python -I scripts/swipe/train.py --train real.npz --train synth-fr.npz:scripts/swipe/layouts/azerty.tsv --val val.npz \
    --out run --epochs 30 --batch 512 --conv 96,96 --hidden 128 --layers 2 --dropout 0.2 --device cuda
# 5. Score (first guess / top 3, ms per swipe)
build/tools/swipekb-swipenet-bench --model run/model.bin --words words.tsv --layout scripts/swipe/layouts/qwerty.tsv bench-2000.jsonl
```

The exact flag values of the shipped T1 run were not recorded; the sizes above follow the run table in `docs/active/specs/2026-10-23-own-swipe-engine.md`, and the geometry and speed augmentation strengths should be taken from the script's help text. Expect to need a few attempts to match the published numbers. More commands, and the Gboard and FUTO measurements, are on the [methods page](../methods.md).

## Licence of the model file

`swipe-model.bin`: **MIT**. Training data: real swipes from FUTO's swipe.futo.org collections (MIT, Copyright (c) 2025 FUTO) plus our own synthetic swipes; the file contains no FUTO model weights. <!-- src: packaging/assets.lock.json trained_models.swipe.license = MIT; packaging/NOTICE "Trained models" -->

Required credits when redistributing:

- **Models trained and released by Destin Moss via youcoded.ai, 2026**
- The MIT licence text of FUTO's swipe data (`FUTO-SWIPE-DATA-LICENSE.txt`, shipped with the model).
- The release notice `MODELS-NOTICE.md` (release `models-2026-10-23`).

The word lists the model is used with carry their own attribution terms (FineWeb / FineWeb-2: ODC-By 1.0; Tatoeba: CC BY 2.0 France; for English also the Android LatinIME word list, Apache-2.0). They are separate files and not part of the model.

## Credit

Models trained and released by Destin Moss via youcoded.ai, 2026.
