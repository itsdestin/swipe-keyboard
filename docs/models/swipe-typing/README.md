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
- name: swipe-typing (T1)
  results:
  - task:
      type: other
      name: Swipe decoding, English, first guess right (in the keyboard, previous words known)
    dataset:
      name: bench-2000, the 1,946 swipes FUTO's scorer counts (real English swipes, FUTO MIT test split)
      type: custom
    metrics:
    - type: accuracy
      value: 91.47
      name: first guess right
  - task:
      type: other
      name: Swipe decoding, English, right word in top 3 (in the keyboard, previous words known)
    dataset:
      name: bench-2000, the 1,946 swipes FUTO's scorer counts (real English swipes, FUTO MIT test split)
      type: custom
    metrics:
    - type: accuracy
      value: 94.55
      name: right word in top 3
---

# youcoded/swipe-typing ("T1"): swipe-typing decoder for English, Spanish, French and German

> **Headline:** in the keyboard, on 1,946 real English swipes typed inside sentences, the first guess is right 91.47 % of the time against 93.32 % for FUTO's engine measured the same way: 1.85 points behind (top 3: 94.55 % against 96.56 %). In the next release, where a small learned re-ranker (with the suggestion bar's sentence model as one of its inputs) orders each swipe's candidate words, the same swipes give 92.86 % (0.46 points behind) and top 3 94.91 % (1.65 behind). No Gboard swipe comparison was made (Gboard swipe typing was not measured).

A small neural network that turns the path of a finger across an on-screen keyboard into a ranked list of words, for the Swipe Keyboard: an open-source swipe-typing and word-prediction add-on for KDE Plasma's on-screen keyboard. One model serves all four languages and any layout, because the keys are given to it as positions.

Models trained and released by Destin Moss via youcoded.ai, 2026.

Code, scripts and tests: https://github.com/itsdestin/swipe-keyboard. How every number below was measured: [methods page](../../methods.md) (published copy: https://github.com/itsdestin/swipe-keyboard/blob/main/docs/methods.md).

**Status: the keyboard's only swipe engine.** FUTO's engine is no longer part of the keyboard; every FUTO comparison below is a measurement against the engine it replaced. On English this model is **not yet as accurate** as that engine (numbers below). If the model file is missing or fails its checksum, the keyboard decodes by key geometry alone.

## At a glance

- **What it is:** a small neural network (2.1 MB, about 529,000 numbers) that turns a finger path over a keyboard into ranked words, for English, Spanish, French and German. One file serves all four languages and any key layout.
- **How to use it:** `swipe-model.bin`, read by `engine/src/swipe_net.cpp` (plain C++, no machine-learning library). Most integrators should call `swipekb::LanguageEngine` (`engine/include/swipekb/language_engine.hpp`), which adds the word list, sentence prior and geometric fallback. You supply the finger path, the key rectangles and a word list.
- **Licence:** MIT. Trained on real swipes from FUTO's swipe.futo.org dataset (MIT; its licence text must travel with the model) plus our synthetic swipes. No FUTO code or model weights.
- **How good it is:** English, in the keyboard with the earlier words of the sentence known, 1,946 real swipes: first guess right 91.47 % (previous FUTO engine 93.32 %), right word in the top 3 94.55 % (96.56 %). Real HeliBoard swipes with no sentence, with the shipped word lists: Spanish 75.9 %, French 68.2 %, German 64.7 % first guess (the FUTO keyboard of version 0.4.1: 77.7 %, 64.6 %, 69.7 %). No Gboard swipe comparison exists. Every figure: `bench/swipe-scoreboard.tsv`.
- **Limits:** English is about 2 points behind the engine it replaced, Spanish 2 and German 5 (French is 3.6 ahead); a word missing from the word list can never be returned; non-English training data is partly synthetic.

## What it is, and what it is not

- A 1-D convolution stack followed by a 2-layer bidirectional GRU, trained with the CTC loss (Graves et al., 2006) on the sequence of keys the finger passes over. Read from the shipped file's header: 2 convolution layers of width 96 (kernel 5), a 2-layer bidirectional GRU with hidden size 128, a pointer head of width 32, 64 time steps of 8 features, float32 weights. That is about **529,000 parameters** and a **2.1 MB** file (2,115,260 bytes).
- Layout independent: the network does not have a fixed output per key. For every time step it scores each key from the finger's offset to that key (in key widths) plus the key's size, with one small shared scoring network ("pointer head"). A QWERTY, AZERTY, QWERTZ or Spanish keyboard with an n-tilde key therefore use the same weights.
- It outputs probabilities over keys, not words. Words come from a **lexicon beam search** over a word list (a trie of key sequences), so every answer is a word in the list, scored as network evidence + word frequency + the n-gram sentence prior when previous words are known + a **geometric cross-check** (a separate, classical shape-matching decoder, blended in with weight 40).
- **Fallback:** with no model file, a damaged one, or an empty result, the geometric decoder alone answers; swiping never depends on the network.
- It is **not** a handwriting or general gesture recogniser, and it cannot output a word that is not in the word list (words missing from the list are always misses; see limitations).

## Intended use

On-device swipe typing. The keyboard supplies: the finger path (x, y, time), the live rectangles of the keys, a verified word list for the language, and, when available, the previous words of the sentence. Settings used for all reported results: frequency weight 0.06, geometric cross-check weight 40, beam width 48.

Speed on one CPU thread: about 3.5 ms per swipe (measured on a loaded desktop, plain `-O3` build).

Not intended for: security-sensitive input, identifying people, or any use that stores or sends a user's swipes. The keyboard does not record swipes unless a user switches on a separate opt-in recorder.

## File format

File `swipe-model.bin`, read by our own plain C++ runner (`engine/src/swipe_net.cpp`, no machine-learning library). Magic `SKNN`, version 1, little-endian.

| Part | Content |
|---|---|
| Magic | the 4 bytes `SKNN` |
| Header (u32 each) | version (1); data type (0 = float32, 1 = float16); time steps (64); input features per step (8); key features (4); number of convolution layers; kernel size; GRU hidden size; GRU layers; pointer-head width |
| Convolution widths | one u32 per convolution layer |
| Tensors | in order: for each convolution layer weight [out][in][kernel] and bias; for each GRU layer and direction (forward, then reverse) input weights, recurrent weights and both biases (PyTorch gate order r, z, n); pointer head: A (weight, bias), B (weight, bias), w (weight, bias); blank head (weight, bias) |

Because the layer sizes are stored in the file, a bigger or smaller network needs no code change. The loader validates every header field against fixed ranges and refuses a file that is too short for the tensors it declares. **There is no checksum inside this file**, unlike the next-word models; integrity comes from the SHA-256 the keyboard pins (`941272a7017229a73e07d1c0bd766757e739174db3a42fb622c26256b8c46110`), checked before the engine loads it. A mismatch means geometry-only mode.

Input per step (64 steps resampled by arc length): x, y, dx, dy, log elapsed time, fraction of the path done, step length, log path length.

## Training data and licences

| Source | What was taken | Licence |
|---|---|---|
| FUTO's public swipe collections (swipe.futo.org: `swipe-1` training split, `swipe-2` to `swipe-5`) | real swipes, English QWERTY; `swipe-5` also holds real German, French and Spanish swipes on their own layouts | MIT (Copyright (c) 2025 FUTO; text kept in `FUTO-SWIPE-DATA-LICENSE.txt`) |
| Our own synthetic swipes | 100,000 each for Spanish, French and German, generated by `scripts/swipe/synth_swipes.py` on approximate layouts; no synthetic English in T1 | ours; MIT |
| Word lists (used at run time, not part of the model file) | the benchmarks used 260,000-word lists counted from FineWeb / FineWeb-2 and Tatoeba; the keyboard plugin loads the language profile's verified `.combined` list instead (top 100,000 words for es/fr/de) | ODC-By 1.0 and CC BY 2.0 FR, see the language-pack notices |

What exactly went into T1: about 317,000 swipes from a session-level 35 % subset of `swipe-1` to `swipe-4`, `dev.jsonl` (54,000 English swipes from FUTO's MIT recordings; about 47,600 left for training after 10 % of its sessions were held out for validation), all `swipe-5` groups (French, German and Spanish slices repeated three times, each on its own layout), plus the synthetic swipes. 30 epochs, batch 512.

**Never trained on:**
- `bench-2000`, `test-extra-10000` and `test.jsonl` (FUTO's MIT test split) and the validation sessions: an overlap check dropped any swipe whose id or session appeared in them; no overlapping swipe or session was found in any collection (56,005 swipe keys and 768 sessions checked).
- The HeliBoard gesture test sets (CC BY-SA): used for **testing only**. No HeliBoard data is in the training set, which is why the model's licence is not share-alike.
- No FUTO model weights were used or fine-tuned; the architecture and code are our own, written from published literature (CTC, lexicon-constrained gesture decoding, SHARK2, dynamic time warping).

## Training procedure and hardware

- Script: `scripts/swipe/train.py` (PyTorch, CTC loss on the collapsed key sequence; AdamW, one-cycle learning rate). Recipe "R2": dropout 0.2, board-geometry augmentation and speed augmentation, random whole-trace shifts of about 0.2 key.
- Hardware: one desktop NVIDIA RTX 4070 Super. **Training time for T1: 16 minutes** for 30 epochs.
- Model selection: search settings (frequency weight, cross-check weight, beam) were fixed on the clean validation split before scoring. `bench-2000` and `test-extra-10000` were scored once for the earlier model F1 and a second time for T1, the final model only; T1 was chosen using other data (trv8k, `swipe-5` slices, clean3000), see below.
- Not recorded in the repository: the exact command line of the T1 run and the random seed (the script default is 1).

## Evaluation

Every figure on this page is a row of `bench/swipe-scoreboard.tsv` (date, build, test set, number of swipes, whether the previous words were given, whether a sentence pack ranked the guesses, first guess and top 3).

All figures are percentages. "First guess" = the top suggestion is the target word; "top 3" = it is among the first three. **Denominators matter:** "all swipes" counts every swipe, including those whose target word is not in the word list (those can never be right); "in-dictionary" counts only swipes whose target is in the word list.

### In the keyboard: English real swipes with the files that ship (final 0.5.0 test build, 2026-10-24)

Through `swipekb::LanguageEngine` with the shipped dictionary, sentence pack (`en.ngram`) and this model. "Previous words" = the earlier words of the sentence are given, as when typing inside a sentence; "none" = no earlier words (with the sentence pack, every word is scored as the first word of a sentence); "no sentence pack" = word frequency only. "FUTO-scorable" = the swipes FUTO's scorer counts (targets of 2 or more letters, more than 2 trace points), so the FUTO figure compares only with those rows.

| Test set | Swipes | Previous words: first / top 3 | None: first / top 3 | No sentence pack: first / top 3 |
|---|---|---|---|---|
| bench-2000, all | 2,000 | 90.10 / 93.10 | 88.15 / 92.70 | 89.20 / 93.10 |
| bench-2000, FUTO-scorable | 1,946 | 91.47 / 94.55 | 89.47 / 94.14 | 90.54 / 94.55 |
| test-extra-10000, all | 10,000 | 90.45 / 93.57 | 88.10 / 93.00 | 89.25 / 93.25 |
| test-extra-10000, FUTO-scorable | 9,719 | 92.03 / 95.21 | 89.61 / 94.63 | 90.80 / 94.89 |

**Next release (unreleased, 2026-10-30):** a swipe with previous words is also re-ranked by the neural next-word model
(`ContextRescore`; weights tuned on tuning swipes only). "Previous words", first / top 3: bench-2000 all 90.95 / 93.25,
FUTO-scorable 92.34 / 94.71; test-extra-10000 all 91.09 / 93.71, FUTO-scorable 92.68 / 95.35. The other columns are
unchanged (no previous words, or no sentence pack: no re-ranking). Details: `docs/methods.md`.
Joint-model step 3 (same day, unreleased): a small learned re-ranker (`SwipeReranker`, 2.4 KB of weights built into
the engine, trained on 10,002 FUTO English swipes this model never trained on) replaces those fixed weights and also
orders swipes with no previous words. "Previous words", first / top 3: bench-2000 all 91.45 / 93.45, FUTO-scorable
92.86 / 94.91; test-extra-10000 all 91.48 / 93.73, FUTO-scorable 93.08 / 95.35 (after the review fix of places 2-3,
`docs/methods.md`; before it the top 3 was 93.30, 94.76, 93.64 and 95.26). No previous words: bench-2000 all
89.45 / 93.05 (was 88.15 / 92.70). The "no sentence pack" column was not re-measured (the learned model also acts there; untested, and no shipped language lacks a pack).

### The model alone (its own benchmark tool and word list, 2026-10-23; not the shipped setup)

`tools/swipekb-swipenet-bench`, frequency prior, no previous words.

| Test set | Swipes | All swipes: first / top 3 | In-dictionary: first / top 3 |
|---|---|---|---|
| bench-2000 | 2,000 (1,958 in-dictionary) | 90.40 / 94.95 | 92.34 / 96.99 |
| test-extra-10000 | 10,000 (9,781 in-dictionary) | 90.77 / 94.97 | 92.80 / 97.10 |
| clean3000 (3,000 swipes, diagnostic) | 3,000 | not stated in the source | 93.13 / 97.37 |

### Against FUTO's engine (English)

| Set | FUTO engine, first / top 3 | This model, first / top 3 | Same setting? |
|---|---|---|---|
| bench-2000, the 1,946 FUTO-scorable swipes, previous words given | 93.32 / 96.56 | 91.47 / 94.55 (in the keyboard, shipped files) | yes: same swipes, previous words given to both |
| clean3000 | 92.5 / 96.3 (2,909 swipes FUTO's tool can score) | 93.13 / 97.37 (model alone, in-dictionary only, 97.5 % of the 3,000) | no |

Plainly: like-for-like, the keyboard is **1.85 points behind FUTO on first guess and 2.01 behind on top 3** (91.47 vs 93.32; 94.55 vs 96.56). FUTO's 93.32 / 96.56 was measured with the previous words given and counts only 1,946 of the 2,000 swipes (its scorer skips one-letter targets), so it is not comparable with the "all 2,000" or "no previous words" rows. The stated target (match FUTO's 93.3 / 96.6) was **not reached**.

### Other languages: real swipes from HeliBoard (test only)

HeliBoard gesture test sets: about 5,000 real swipes per language (no sentences), case-insensitive, accents kept, per-swipe real board geometry. First guess / top 3 over the swipes each tool could evaluate. "In the keyboard" uses the shipped word lists (100,000 words for es/fr/de) and the keyboard's own profile (`tools/swipekb-language-bench`); the FUTO keyboard 0.4.1 column ran on the same swipes with its own, older word packs.

| Language | In the keyboard, shipped files (2026-10-24) | FUTO keyboard 0.4.1 (old packs) | Model alone, 260,000-word lists (2026-10-23) |
|---|---|---|---|
| English | 80.5 / 89.1 (4,576 qwerty swipes, words in the dictionary) | 84.5 / 93.4 | 82.1 / 91.5 (all 5,000) |
| Spanish | 75.9 / 84.7 (4,790) | 77.7 / 85.1 | 81.1 / 89.8 |
| French | 68.2 / 82.0 (4,999) | 64.6 / 76.9 | 71.7 / 85.9 |
| German | 64.7 / 73.1 (4,980) | 69.7 / 76.4 | 73.3 / 82.3 |

The shipped word lists contain 90.3 % (Spanish), 92.2 % (French) and 80.0 % (German) of these test words, which caps accuracy; the 260,000-word lists cover Spanish 97.5 %, French 98.2 %, German 91.9 %, which is why the last column is higher. Larger shipped lists are the measured lever for Spanish and German.

Two common HeliBoard English boards with near-identical geometry differ by 5 to 7 points for every decoder tested, so the spread between rows is partly the swipers, not the layout handling.

### Real multilingual swipes from FUTO's `swipe-5`, on their own layouts (validation slices)

| Model | French | German | German (QWERTZ) |
|---|---|---|---|
| F1 (previous model, English + synthetic only) | 79.8 | 84.7 | 81.6 |
| T1 (this model) | 83.2 | 90.2 | 83.2 |

First guess. The source calls these "val slices" of the collection; it does not describe how they were separated from training in that table, so treat them as a development comparison, not a final test.

### Synthetic Spanish, French and German (not human)

The synthetic noisy sets (`bench/languages/synthetic-noisy-v1-*.json`, 400 words each) were scored with the **earlier keyboard that used FUTO's engine**, not with this model: Spanish 89.00 / 98.25, French 83.25 / 93.00, German 87.75 / 97.00 (first / top 3). We have **no synthetic-set score for T1 in the sources**. Synthetic English calibration: real 92.8 / 96.4 against synthetic 92.3 / 96.0. These sets only cover dictionary words and assume people swipe other languages like English.

### Sentence-context prior (experiment)

When the keyboard knows the previous words, the English n-gram prior lifts T1 on a 7,198-swipe set (trv8k) from 92.19 / 95.79 to 93.10 / 95.92 (first / top 3, all swipes; in-dictionary 95.28 / 98.17). That is +0.9 first guess. The set is a held-back 5 % slice of FUTO's `train.jsonl` that still has sentence text. bench-2000 also records each swipe's sentence; in the keyboard the previous words add 1.95 points there (88.15 to 90.10, all 2,000; table above).

### Gboard

Gboard's swipe typing was **not measured** (any version, learning on or off).

## Known limitations

- **English is about 2 points behind FUTO's engine** like-for-like (91.47 against 93.32 first guess on the same 1,946 swipes with previous words); the target was not met. More real English data barely helped: six times more real English swipes moved the clean English numbers by +0.4 on one set and 0 on another, so we stopped (agreed stopping rule).
- **French and German are lower than English** on HeliBoard (in the keyboard 68.2 and 64.7 first guess vs 80.5), and Spanish and German are below the FUTO keyboard of 0.4.1. The sources do not isolate the cause. One measured factor is dictionary coverage of the targets (German 80.0 % with the shipped list, 91.9 % even with the 260,000-word list; compounds are the likely reason, not verified). The spread between swipers on the same layout is also large (5 to 7 points).
- **Accent twins** (Spanish que / qué, como / cómo; French ou / où) share one swipe path. The shape cannot decide between them; the sentence prior has to, and without previous words the choice is frequency only.
- **Short words (3 letters or fewer)** were the other common miss when the earlier keyboard (FUTO's engine) was scored on synthetic Spanish, French and German swipes; we have not repeated that breakdown for T1.
- **Out-of-dictionary words are always misses.** About 2.5 % of English targets are not in the word list (bench-2000: 42 of 2,000).
- **Spanish, French and German training is partly synthetic** (100,000 each) plus the real `swipe-5` slices. Only English has large real recordings. Real volunteers' swipes per language are still missing, so non-English numbers are less certain than English ones.
- **Layout handling is approximate where we had no real key rectangles.** The AZERTY/QWERTZ/Spanish layouts used for synthetic training are approximations (`make_layouts.py`); the real keyboard supplies live rectangles at run time. Key pitch, not drawn key width, is the unit, because boards with gaps between keys were 20 % off otherwise.
- **An earlier validation error was found and corrected** (see the incident below). Every number on this page comes from the corrected, clean sets.
- No bias or fairness evaluation was done. The training swipers are volunteers on FUTO's collection; their handedness, device and language mix are not documented here.

### The inflated-validation incident (2026-10-22)

For a day, the validation sets named `devhold-5000`, `tune-2000` and `tune-10000` were believed to be held out. They were not: all of their swipes were contained in `dev.jsonl`, the training set. Every "devhold" figure was therefore measured on training swipes, including 91.5 % for the first CPU model and the 93.02 / 96.07 that looked like a tie with FUTO for the first GPU model ("gpu1"). **Those numbers are retracted.** The swipe helper found it; the coordinator verified it (100 % of ids and sessions overlapped); the fix was to hold out whole recording sessions from `dev.jsonl` as validation (`scripts/swipe/split_by_session.py`) and to select only on that, keeping `bench-2000` and `test-extra-10000` as final-only sets. The honest first read on fresh swipes put the model 2 to 4 points behind FUTO, not level.

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

The exact flag values of the shipped T1 run were not recorded; the sizes above follow the run table in `docs/active/specs/2026-10-23-own-swipe-engine.md`, and the geometry and speed augmentation strengths should be taken from the script's help text. Expect to need a few attempts to match the published numbers. More commands, and the Gboard and FUTO measurements, are on the [methods page](../../methods.md).

## Licence of the model file

`swipe-model.bin`: **MIT**. Training data: real swipes from FUTO's swipe.futo.org collections (MIT, Copyright (c) 2025 FUTO) plus our own synthetic swipes; the file contains no FUTO model weights.

Required credits when redistributing:

- **Models trained and released by Destin Moss via youcoded.ai, 2026**
- The MIT licence text of FUTO's swipe data (`FUTO-SWIPE-DATA-LICENSE.txt`, shipped with the model).
- The release notice `MODELS-NOTICE.md` (release `models-2026-10-23`).

The word lists the model is used with carry their own attribution terms (FineWeb / FineWeb-2: ODC-By 1.0; Tatoeba: CC BY 2.0 France; for English also the Android LatinIME word list, Apache-2.0). They are separate files and not part of the model.

### The learned re-ranker built into the engine

`engine/src/swipe_rerank_weights.inc` (about 250 numbers, 2.4 KB of text, compiled into the engine; read by
`SwipeReranker`): **MIT**. It orders the 30 words this model and the search find for a swipe (`docs/methods.md`, "Swipe in
the keyboard with the learned re-ranker"). Trained 2026-10-30 by `scripts/swipe/train_rerank.py` on 10,002 real English
swipes from FUTO's swipe.futo.org data that this model never trained on (the held-back `trv8k` slice of `swipe-1` and the
`dev.jsonl` sessions held out for validation), validated on 3,833 more from other sessions; FUTO's swipe-5 French and
German validation slices were only checked. No HeliBoard data, no test set, no synthetic swipes, no FUTO model weights.
The training data is FUTO's (MIT, Copyright (c) 2025 FUTO), so the same credits as for the model file apply: keep
FUTO's MIT notice (`FUTO-SWIPE-DATA-LICENSE.txt`, installed in the licence folder) with these weights. Sources:
`docs/models/SOURCES.md`, "Learned swipe re-ranker".

## Credit

Models trained and released by Destin Moss via youcoded.ai, 2026.
