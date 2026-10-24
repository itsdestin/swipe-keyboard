# How we measured everything

Every number is traced to a file in the repository in an HTML comment next to it. Where a number does not exist in our records, this page says "not measured" instead of estimating one.

Models trained and released by Destin Moss via youcoded.ai, 2026. Project: https://github.com/itsdestin/swipe-keyboard. Model cards: [docs/models/](models/README.md).

## Why this page exists

We say our next-word and swipe models are good in some places and not in others. That is only believable if you can see exactly what was counted, on which text, against what, and what we got wrong along the way. This page is that account. It includes a measurement error we made and retracted ([the devhold incident](#the-devhold-contamination-incident)).

Plain rules we follow:

- Every figure says its denominator (how many sentences or swipes, and which ones).
- A change ships only if no language got worse on any scoreboard measure beyond noise: about 1 point on sets of roughly 700 words, wider sets where a decision hinges on a smaller gap.
- Test data never feeds training (see [safeguards](#data-leak-safeguards)).
- Results are reported with the places where we lose, not only where we win.

## The three tasks

| Task | What the user does | What we count |
|---|---|---|
| **Next word** | A sentence is partly typed; no letters of the next word yet. | The word the writer actually typed next is among the 3 suggestions ("top 3"). |
| **Completion after N letters** | The first N letters (1, 2 or 3) of the next word are typed. | The word is among the suggestions. We report **top 2** because the keyboard (and Gboard) show the typed letters themselves in one of the three slots. Words with N letters or fewer are skipped. |
| **Swipe decoding** | A finger path across the keyboard. | **First guess**: the first suggestion is the target word. **Top 3**: it is among the first three. |

Matching is case-insensitive for typing tasks (capitals are display, not prediction). For swipes, the ASCII benchmark compares letters only; the HeliBoard sets are case-insensitive with accents kept.

## Test sets

### Next word and completion

All three sets are built by `scripts/lm/make-nextword-testsets.py` with a fixed seed so every model is scored on the same text; 2,000 sentences each by default, 4 to 30 words per sentence, no URLs or code-like text. The routine scoreboard uses the **first 300 sentences** of each set (about 2,000 scored words per measure); blend tuning uses sentences 1000 and beyond so the reported ones stay untouched.

| Set | Source | Licence | Notes |
|---|---|---|---|
| **everyday** | Common Voice's community-written sentences, minus any that also appear in Tatoeba | CC0 | Read-aloud style, not texting. Never trained on. |
| **web** | Sentences from the FineWeb-2 official test split, at most one per web page (English: a FineWeb sample shard kept out of training) | ODC-By 1.0 | Partly "home ground" for models trained on web text. |
| **tatoeba** | Tatoeba sentences whose id ends in 0 | CC BY 2.0 FR | The tenth the packs never trained on. Home ground for models that train on the other nine tenths. |

No single source is neutral. A model built from web text should win on web text; we read a model as "better" only if it also wins on sets it was not built from. We say so next to each result.

### Swipes, English, real

FUTO's public swipe recordings (MIT licence; swipe.futo.org). Splits we use:

| Name | What it is | Use |
|---|---|---|
| `bench-2000` | 2,000 swipes spread evenly across FUTO's test split (every k-th line, so many different people; `bench/make-slice.py`) | final scoring only |
| `test-extra-10000` | 10,000 further swipes of the test split (how they were picked is not recorded in the repository) | final scoring only |
| `test.jsonl` | FUTO's full test split | clean3000 below is drawn from it |
| clean3000 | 3,000 swipes from `test.jsonl` excluding bench-2000 and test-extra-10000 | diagnostic; never trained on or tuned against |
| trv8k | 7,198 swipes: a 5 % session-level slice of FUTO's training file that keeps its sentence text | validation, and the sentence-context experiment |
| validation sessions | 10 % of `dev.jsonl` by whole recording session (6,637 swipes) | model selection |

bench-2000 is scored the same way for FUTO's engine and ours. Two denominators are always reported: **all swipes** (including those whose target is not in the word list and so can never be found) and **in-dictionary** (only swipes whose target is in the word list). FUTO's own tool skips about 3 % of swipes it cannot score; we state the count where we compare.

### Swipes, Spanish, French, German

- **HeliBoard real multilingual swipes** (from the HeliBoard project's gesture test data; CC BY-SA): 5,000 real swipes per language, with each swipe's real board geometry. **Test only. Never trained on** (the share-alike licence is the reason we kept it out of training).
- **Synthetic noisy swipes** (`bench/languages/synthetic-noisy-v1-{es,fr,de}.json`): 400 words each, sampled by frequency from our everyday and web test text, traced by a noise model fitted on real English swipes, then scaled so synthetic English matched real English on bench-2000 (92.3 vs 92.8 first guess). They only cover dictionary words and assume people swipe other languages like English. They are **not human swipes** and are never marked as reviewed by native speakers. A different generator (`scripts/swipe/synth_swipes.py`) makes training swipes, so the model is never scored on its own generator's output.
- **Real swipes from FUTO's `swipe-5`**: real French, German and Spanish swipes on their own layouts, used for training; validation slices of it are a development check only.
- **Hand-authored fixtures** (`synthetic-centered-v1.json`, 30 seeds): not reviewed by native speakers, no thresholds attached, used for code regression checks, not accuracy claims.

Real volunteer swipes for Spanish, French and German from people who speak them: **still missing**.

## How Gboard was measured from the outside

We compare with Gboard only by using the app, as a user would. Full procedure: `bench/gboard/README.md`.

- **What:** Gboard 14.2 (the copy built into Google's Android 35 x86_64 emulator image; the repository records no fuller build number), learning from typing **off**, one language at a time with multilingual typing off. Measured 2026-10-22.
- **Sentences:** 100 per language per set, from our own test sets (everyday and web).
- **Method:** for every word, the words before it plus its first 0, 1, 2 or 3 letters are put into a Messages text box, and the three suggestions are read off the screen. The three slots are recorded left, middle, right; middle is Gboard's best guess. This "text fill" method is much faster than tapping every key.
- **Check that text fill matches real typing:** 30 random phrases typed with real key taps gave the same suggestions as text fill in **27 of 30** cases; the 3 differences were single words with no direction (`validate_taps.py`).
- **Slot rule:** once letters are typed, Gboard spends one slot on the typed letters (95 to 100 % of rows), so the fair figure for us is "right word in our top 2". Before typing, both get 3. Sentence starts are not scored because Gboard shows shortcut chips there instead of words.
- **Frozen answers:** Gboard's replies are saved in `bench/gboard/reference/LANG-SET.tsv` (one row per query: sentence number, word number, letters typed, text before the word, the word, left / middle / right suggestion). `bench/scoreboard.sh` replays our predictor over exactly those queries, so anyone can compare without an Android device. English, Spanish, French and German references are committed.
- **Limits:** one Gboard version (current Gboard is newer and may differ); Gboard with learning on was not measured; learning off, so this is Gboard's out-of-the-box model, not what it does after weeks of use; 100 sentences per set gives roughly +-2 points of noise; the emulator is Google's and we have no view inside Gboard.
- **Gboard swipe typing: not measured.**

## How FUTO's engine was measured

FUTO's swipe engine was the keyboard's engine before our own replaced it, and it is our reference for swipe accuracy. The keyboard no longer contains it; we measured it earlier, through two paths:

1. FUTO's own `infer_jsonl` program on the bench-2000 slice (beam width 100, top 3). With the stock dictionary this gave 92.50 first guess / 95.43 top 3 on 2,000 swipes.
2. Through our own recogniser harness with FUTO's models and our current dictionary: 93.32 first guess / 96.56 top 3 on bench-2000. This is the "FUTO 93.32 / 96.56" figure used in the model card.

The two differ because of the dictionary and ranking used, not the same code path; when we compare, we name which one. On clean3000, FUTO's tool scores the 2,909 swipes it can read: 92.5 / 96.3.

FUTO's code and model weights were **not used** in our own swipe engine (see below). FUTO's public swipe data (MIT) was used for training, with its licence text kept. FUTO's next-word model was the keyboard's English suggestion engine before 2026-10-22; we measured it as the "before" baseline for English (see results).

## Reproducing everything: the scoreboard

`bench/scoreboard.sh` is one command that rebuilds the typing table for all four languages: next word, completion after 1 and 3 letters, speed, pack size, and the same against Gboard's frozen answers. It appends rows to `bench/scoreboard.tsv`; `--check` fails if any accuracy measure fell more than 1 point below the previous run (about 2,000 scored words per measure, so smaller moves are noise). A full run takes a few minutes.

Rows in `bench/scoreboard.tsv` (commit and label are recorded per row):

| Label | Commit | What it is |
|---|---|---|
| installed packs (Tatoeba-only) | 7f53d8c | The keyboard's n-gram files before 2026-10-22 (es / fr / de) |
| new web+Tatoeba packs (mix30-S) | 7f53d8c | First web + Tatoeba packs |
| published language files 2026-10-22 (XS) | fd59e68 | The published XS n-gram files |
| open word lists 2026-10-22 | a2041d8 | Same, with the open (non-share-alike) word lists |
| AI models + open word lists 2026-10-23 | d63a2ce | The above plus the four neural next-word models |

Swipe accuracy comes from `tools/swipekb-swipenet-bench` (our engine) and `bench/score.sh` (FUTO's), described in the model card.

## Data-leak safeguards

1. **8-word-run exclusion (next word).** Any training sentence that shares a run of 8 consecutive words with any sentence of the three test sets is dropped, for both the n-gram files and the neural models. The same rule applies to the counts behind the word lists. This removes verbatim overlap, not paraphrases.
2. **Tatoeba tenths.** Test set = ids ending in 0; training uses the other nine tenths only. 
3. **Test-only data stays test-only.** Common Voice sentences (CC0) and the HeliBoard swipes are never trained on.
4. **Tuning on other sentences.** Blend weights for the neural models were tuned on test-file sentences 1000 and beyond; reported scores use the first 300.
5. **Session-level splits (swipe).** Swipes from one person look alike, so a random split would leak that person into validation. Validation is 10 % of `dev.jsonl` by whole session (a stable SHA-1 of the session id).
6. **Overlap check against every evaluation file.** Before training on FUTO's other collections, every swipe whose (session, timestamp) key, or whose session, appears in bench-2000, test-extra-10000, `test.jsonl` or the validation sessions is dropped, with counts printed. Checked: 56,005 swipe keys and 768 sessions; no overlapping swipe or session occurred in any collection (counts kept: `swipe-1` 901,908; `swipe-2` 23,774; `swipe-3` 34,769; `swipe-4` 43,909; `swipe-5` 50,507).
7. **Different generators for training and test synthetic swipes.** (`synth_swipes.py` vs `make-noisy-swipes.py`.)
8. **Final sets are scored rarely.** bench-2000 and test-extra-10000 are scored once per final model, not used for selection.

## The devhold contamination incident

On 2026-10-22 we reported swipe results from validation sets called `devhold-5000`, `tune-2000` and `tune-10000`. They looked excellent: a first GPU-trained model scored 93.02 / 96.07, apparently tying FUTO's 93.3 / 96.6, and an earlier CPU model scored 91.5.

They were wrong. Those validation sets were **entirely contained in `dev.jsonl`, the training file** (100 % of ids and sessions). Every figure measured on them was a score on training swipes. We found it (the swipe helper noticed), verified it by comparing ids and sessions, and **retracted all devhold numbers**. The corrected first read, on 3,000 fresh swipes from `test.jsonl` that were never trained on, put our model at 88.6 / 93.9 (all swipes counted) versus FUTO's 92.5 / 96.3: a gap of roughly 2 to 4 points, not zero.

What we changed: validation is now whole recording sessions held out of `dev.jsonl`; model selection uses only that; bench-2000 and test-extra-10000 are final-only; and `split_by_session.py` documents the incident in its header so nobody reuses the old sets. The geometric decoder's 82.0 / 90.0 result never trained on `dev.jsonl` and stands.

## What we deliberately did not use

- **FUTO code.** The own swipe engine (CTC network, lexicon beam search, geometric fallback) was written from published literature (Graves et al. 2006 on CTC; Alsharif et al. 2015 and follow-ups on gesture decoding; Kristensson and Zhai 2004; dynamic time warping), by a writer who had not read FUTO's source. The older files that called FUTO's engine have since been removed from the repository.
- **FUTO model weights.** None are in our models, and FUTO's engine is no longer part of the keyboard. The only FUTO material used is its public swipe data (MIT), for training the swipe model; its licence text travels with the model.
- **A Gboard study report.** Early in the project a static study of Gboard 18.5's app code was written. It was withdrawn at the owner's request on 2026-10-23 and removed from the current tree (it remains in older git history); nothing in our models, data or numbers depends on it. Gboard is compared only by using the app from the outside, as described above.
- **HeliBoard data for training.** Its gesture data is CC BY-SA (share-alike); we use it for testing only.
- **wordfreq.** Replaced by word lists counted from FineWeb / FineWeb-2 and Tatoeba (share-alike term removed).
- **Real users' typing.** Nothing is collected from users. The opt-in swipe recorder in the plugin exists but its recordings are not part of any model here.

## Results summary

All typing figures: percent, 300 sentences per set, `bench/scoreboard.tsv` commit d63a2ce (n-gram files plus neural models); "vs Gboard" = points ahead (+) or behind (-) Gboard 14.2 (the copy built into Google's Android 35 x86_64 emulator image, learning off, tested 2026-10-22) on its recorded queries, 100 sentences per set, top 3 before typing and top 2 after. Compare with caution: web and Tatoeba sets are partly home ground.

### Typing

| Language | Next word, top 3: everyday / web / Tatoeba | After 1 letter, top 2: everyday / web / Tatoeba | After 3 letters, top 2: everyday / web / Tatoeba | vs Gboard 14.2 (emulator copy, learning off), 1 letter: everyday / web |
|---|---|---|---|---|
| English | 29.36 / 31.62 / 43.83 | 51.96 / 52.89 / 67.82 | 71.97 / 73.53 / 88.97 | +11.95 / +12.87 |
| Spanish | 26.27 / 33.25 / 31.32 | 49.93 / 56.57 / 58.58 | 67.08 / 75.10 / 77.24 | +6.58 / +17.46 |
| French | 22.10 / 31.97 / 32.79 | 46.46 / 54.07 / 59.35 | 65.44 / 73.70 / 74.47 | +4.29 / +12.70 |
| German | 25.04 / 24.22 / 31.57 | 48.70 / 47.46 / 57.87 | 70.01 / 66.69 / 78.40 | +5.88 / +10.76 |

The neural models add (against the n-gram files alone, commit a2041d8): between +0.36 (English, Tatoeba, after 1 letter) and +9.32 (Spanish, web, after 3 letters) points depending on set and measure, largest on web text and after 3 letters; 3 of the 36 measures moved by less than 1 point, i.e. within noise. See each [model card](models/README.md) for the per-measure tables. Before the 2026-10-22 open files, the keyboard trailed or tied Gboard on several French and German web measures (French web: -5.11 points after 1 letter, -13.22 after 3 letters), and on the recorded 0- and 1-letter measures its lead over Gboard was never larger than 2.33 points (Spanish, French and German, everyday text).

### Swipe (model T1; English real swipes, FUTO's test split)

| Set | Denominator | Ours first / top 3 | FUTO first / top 3 |
|---|---|---|---|
| bench-2000 | all 2,000 swipes | 90.40 / 94.95 | 93.32 / 96.56 |
| bench-2000 | in-dictionary (1,958) | 92.34 / 96.99 | not restricted this way |
| test-extra-10000 | all 10,000 | 90.77 / 94.97 | not measured |
| test-extra-10000 | in-dictionary (9,781) | 92.80 / 97.10 | not measured |
| clean3000 | in-dictionary | 93.13 / 97.37 | 92.5 / 96.3 (2,909 swipes it can score) |

### Swipe, other languages (HeliBoard real swipes, test only, 5,000 each, first / top 3)

| Language | Ours (T1, 260k word lists) | FUTO's engine (older, smaller packs; no longer used; not like-for-like) |
|---|---|---|
| English | 82.1 / 91.5 | 84.5 / 93.4 |
| Spanish | 81.1 / 89.8 | 77.7 / 85.1 |
| French | 71.7 / 85.9 | 64.6 / 76.9 |
| German | 73.3 / 82.3 | 69.7 / 76.4 |

### What is not measured

- Gboard swipe accuracy; Gboard English next word with its learning on; any newer Gboard.
- Real volunteer swipes for Spanish, French and German (HeliBoard's are real but from a different population; synthetic ones are model-made).
- Our swipe model on the synthetic Spanish, French and German sets (those sets were scored only with the earlier keyboard that used FUTO's engine: es 89.00 / 98.25, fr 83.25 / 93.00, de 87.75 / 97.00).
- FUTO's English next-word model on completion after letters; FUTO against the HeliBoard sets with the new open word lists.
- Validation perplexity and training curves of the next-word models.
- Speed on ARM or Steam Deck hardware (no device available).
- Texting-style text (all next-word sets are read-aloud, web or example-sentence text).

## Known limits of the whole method

- Small sets: 300 sentences (about 2,000 words) per typing measure, 100 sentences against Gboard. Differences under about 1 to 2 points are noise.
- One Gboard version (14.2, emulator copy), learning off, tested 2026-10-22; newer versions may differ and learning-on behaviour was not measured. We make no general claim about Gboard.
- Synthetic swipes are provisional; non-English real-swipe evidence comes from HeliBoard (test) and FUTO's `swipe-5` (training).
- The home-ground effect on web and Tatoeba sets.
- A single hardware setup: one desktop with an RTX 4070 Super for training; swipe speed measured on a loaded desktop CPU.
- We do not know a user's real-life accuracy; none of these are field trials.
- The incident above shows measurements here can be wrong. Corrections are recorded in git history and the roadmap.

## How to reproduce

From a checkout of https://github.com/itsdestin/swipe-keyboard, build the tools (the Qt build produces `build/tools/swipekb-nextword-eval` and `build/tools/swipekb-swipenet-bench`).

```
# Typing: build the three test sets (needs the Tatoeba export, Common Voice sentences, a FineWeb-2 test parquet)
python -I scripts/lm/make-nextword-testsets.py LANG TATOEBA.tsv.bz2 COMMONVOICE.txt FINEWEB2-TEST.parquet OUTDIR

# Typing: the scoreboard (all four languages; add --check to fail on a regression of more than 1 point)
SWIPEKB_NEXTWORD_EVAL=build/tools/swipekb-nextword-eval SWIPEKB_TESTSETS=<dir with LANG/test/*.txt> \
SWIPEKB_PACKS=<dir with LANG.ngram, LANG.vocab.tsv, LANG.nwp.bin> \
bench/scoreboard.sh --label "my run"

# Typing: one measure by hand, e.g. completion after 1 letter on the web set, first 300 sentences
build/tools/swipekb-nextword-eval --model es.ngram --vocab es.vocab.tsv --nwp es.nwp.bin \
    --sentences OUTDIR/web.txt --limit 300 --letters 1

# Gboard: replay frozen answers (no device needed) -- run by the scoreboard; or by hand
build/tools/swipekb-nextword-eval --model es.ngram --vocab es.vocab.tsv --gboard bench/gboard/reference/es-everyday.tsv

# Gboard: re-record (needs the Android SDK emulator, uiautomator2; see bench/gboard/README.md)
python -I bench/gboard/gboard_probe.py LANG SENTENCES OUT.tsv --count 100 --serial emulator-5554

# Swipe: FUTO's data, filtered and overlap-checked, then score our engine
python -I scripts/swipe/holdout_ids.py holdout-final.json bench-2000.jsonl test-extra-10000.jsonl test.jsonl
python -I scripts/swipe/assemble_futo.py DATA_DIR OUT_DIR holdout-final.json holdout-val.json
build/tools/swipekb-swipenet-bench --model swipe-model.bin --words words.tsv --layout scripts/swipe/layouts/qwerty.tsv bench-2000.jsonl

# Swipe: FUTO's engine on the same slice (historical: needs the old FUTO build, which the keyboard no longer contains)
bench/score.sh --runner futo --label "FUTO reference"

# Synthetic noisy swipes for es / fr / de
python -I bench/languages/make-noisy-swipes.py gen --lang es --params bench/languages/noisy-params-v1.json --text-dir <dir> --out OUT.json
```

Inputs we cannot redistribute from the repository (download them yourself): Tatoeba exports (CC BY 2.0 FR), Common Voice sentences (CC0), FineWeb / FineWeb-2 parquet files (ODC-By 1.0), FUTO's swipe collections (MIT), HeliBoard's gesture sets (CC BY-SA). The language files and trained models are published as GitHub releases `language-files-2026-10-22-open-words` and `models-2026-10-23`, pinned by SHA-256 in `packaging/assets.lock.json`.
