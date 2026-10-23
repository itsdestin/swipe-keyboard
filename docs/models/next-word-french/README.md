---
license: other
license_name: "ODC-By-1.0 AND CC-BY-2.0-FR"
license_link: https://github.com/itsdestin/swipe-keyboard/blob/main/packaging/NOTICE
language:
- fr
library_name: custom
pipeline_tag: text-generation
tags:
- next-word-prediction
- keyboard
- on-device
- lstm
- int8
- swipe-keyboard
datasets:
- HuggingFaceFW/fineweb-2
model-index:
- name: next-word-french
  results:
  - task:
      type: text-generation
      name: Next-word suggestion (model blended with the n-gram pack)
    dataset:
      name: Common Voice everyday sentences, first 300 (CC0)
      type: custom
    metrics:
    - type: accuracy
      value: 22.10
      name: right word in top 3 (word-level, case-insensitive)
  - task:
      type: text-generation
      name: Next-word suggestion (model blended with the n-gram pack)
    dataset:
      name: FineWeb-2 test pages, first 300 sentences
      type: custom
    metrics:
    - type: accuracy
      value: 31.97
      name: right word in top 3 (word-level, case-insensitive)
  - task:
      type: text-generation
      name: Next-word suggestion (model blended with the n-gram pack)
    dataset:
      name: Tatoeba held-out tenth (id % 10 == 0), first 300 sentences
      type: custom
    metrics:
    - type: accuracy
      value: 32.79
      name: right word in top 3 (word-level, case-insensitive)
---

# youcoded/next-word-french: French next-word model for the Swipe Keyboard

> **Result:** used together with its n-gram pack, this model offered the right word more often than Gboard 14.2 on every measure we recorded: 4.29 to 14.56 points ahead, from before typing to three letters typed, on everyday and web sentences (100 sentences per set). Gboard 14.2 here is the copy built into Google's Android 35 x86_64 emulator image, with learning off, tested on 22 October 2026. Newer Gboard versions, and Gboard after it has learned from a user, were not measured.

A small neural network that predicts the next word of a sentence in French. It is one of four (English, Spanish, French, German) and runs on the user's own computer inside the Swipe Keyboard, an open-source swipe-typing and word-prediction add-on for KDE Plasma's on-screen keyboard.

Models trained and released by Destin Moss via youcoded.ai, 2026.

Code, scripts and tests: https://github.com/itsdestin/swipe-keyboard. How every number below was measured: [methods page](../../methods.md) (published copy: https://github.com/itsdestin/swipe-keyboard/blob/main/docs/methods.md).

## What it is, and what it is not

- It is a one-layer LSTM with a projection layer and tied word vectors: the same size class as the next-word models Google describes in its published paper on Gboard ([arXiv 2305.18465](https://arxiv.org/abs/2305.18465)). It is our own model, trained from scratch; no Google or FUTO weights or data were used.
- Header values read from the shipped file: word-vector size 128, LSTM hidden size 1024, vocabulary 23,923 tokens (23,920 words plus three special tokens), int8 weights. That is about 4.3 million numbers (computed from those sizes). The file is 4,698,696 bytes (4.70 MB).
- It is **not** a chat model, cannot write text, and has no memory of anything a user typed. It only returns a probability for each of its 23,920 known words, given the words so far in the current sentence.
- It is **not** meant to be used alone. The keyboard always blends it with a separate word-pair/word-triple (n-gram) pack for the same language; the n-gram knows a larger word list and exact word pairs and triples, this model knows only the most common words (up to 24,000) but reads the whole sentence so far.
- It needs a vocabulary that matches the keyboard's word list for the language; the keyboard refuses to use it if fewer than half of its words match.

## Intended use

On-device keyboard suggestions only. How the keyboard uses it (`engine/src/nwp_predictor.cpp`):

1. **Next word** (nothing typed yet): the model reads the sentence so far (up to the last 20 words), and the keyboard shows the best three suggestions after blending.
2. **Completion** (some letters typed): the same blend, restricted to words that start with the typed letters.
3. **Blend:** for words both sources know, score = lambda x model log-probability + (1 - lambda) x n-gram log-probability. For words only the n-gram knows, the n-gram score plus a fixed offset. This file carries lambda = 0.7 and offset = 0.0.
4. **Fallback:** if the model file is missing or damaged, the keyboard silently uses the n-gram pack only. If the model's word list matches the keyboard's by less than half, it is ignored.
5. It sees only words typed in the current field. Password and other private fields are blocked by the keyboard before any text reaches it. The model does not learn from or store anything.

Not intended for: generating text, content moderation, speech recognition, any use that needs its probabilities to be calibrated across contexts, or languages other than French.

## File format

One plain binary file, `fr.nwp.bin`, read by our own small C++ runner (`engine/src/nwp_model.cpp`, about 250 lines, no machine-learning library). All numbers little-endian.

| Part | Content |
|---|---|
| Magic | the 8 bytes `SWKBNWP1` |
| Header | version (1), data type (0 = float32, 1 = int8 with one scale per row), vocabulary size V, vector size D, hidden size H, byte length of the word list, blend weight lambda (float), out-of-vocabulary offset (float) |
| Words | the V-3 words, one per line, letters only, padded to a multiple of 16 bytes (token 0 = unknown, 1 = sentence start, 2 = sentence end; words are tokens 3 and up) |
| Tensors | word vectors [V x D] (also used as the output layer), output bias [V], LSTM input weights [4H x D], recurrent weights [4H x D], gate bias [4H], projection [D x H]; gate order input, forget, cell, output (PyTorch's) |
| Checksum | FNV-1a 64-bit hash of everything before it |

The loader refuses a file with a wrong magic, an unknown version, out-of-range sizes, a length that does not match the header exactly, or a wrong checksum. The keyboard additionally checks the file's SHA-256 against a pinned value before using it: `54554b495c2f824f9c1138f99f3de3c94b203d56bec59746381ac8d519aabf23`.

Arithmetic at run time: int8 weights, dot products in whole numbers with the input vector rounded to 13 bits; own exp/tanh approximations (relative error about 3e-6). A test checks the C++ runner against PyTorch on a small random model.

Written by `scripts/lm/nwp/export.py`.

## Training data and licences

| Source | What was taken | Licence |
|---|---|---|
| FineWeb-2 (fra_Latn) by Hugging Face (derived from Common Crawl) | web text, sentences split and lower-cased to letter words; pages whose URL looks like a forum, Q&A or discussion page counted twice | ODC-By 1.0 (and Common Crawl's terms of use) |
| Tatoeba (fra) | sentences whose id does **not** end in 0 | CC BY 2.0 France; every contributing author is listed in the language pack's `fr-CONTRIBUTORS.txt` |

Text was never kept in the model files; the model holds weights only.

Which slice of the web text was read for the shipped runs (the n-gram pack of the same language used the first 300 million words plus Tatoeba) was not logged; `prepare.py` reads up to 400 million words by default.

Vocabulary: the most frequent words, up to 24,000, that are also in the keyboard's word list (so every suggestion is something the keyboard can show).

**Test-set exclusion.** Any training sentence that shares a run of 8 words with any sentence of the three test sets (everyday, web, Tatoeba) is dropped. The Tatoeba test set is the id-ending-in-0 tenth, which is never trained on.

Not used: Gboard, FUTO or HeliBoard data of any kind; the Common Voice sentences are test-only.

## Training procedure and hardware

- Script: `scripts/lm/nwp/train.py` (PyTorch). Whole sentences, each starting from the sentence-start token with zero state, because the keyboard only ever feeds the model the current sentence. Sentences are bucketed by length (up to 40 words).
- Script defaults: AdamW, peak learning rate 2e-3 (betas 0.9, 0.98), 300 warm-up steps then cosine decay to 3 %, about 24,576 tokens per step, gradient clipping 1.0, bfloat16 autocast, 1 pass over the data, seed 1. 20,000 randomly chosen sentences held back to compute perplexity.
- Blend weights chosen afterwards by `scripts/lm/nwp/tune.py` on test-set sentences 1000 and beyond, which the scoreboard (first 300 sentences) never uses. Result for this language: best of the script's default 5 x 5 grid, mean of nine measures 46.48.
- Hardware: one desktop NVIDIA RTX 4070 Super. Time: the four exports were written about 20 to 25 minutes apart on 2026-10-23 (file timestamps; this includes data preparation), so training took minutes, not hours.
- **Not recorded:** the exact command-line flags of the shipped runs (epochs, minutes cap, number of web words read, Tatoeba weight), the final validation perplexity, and the loss curves. The shipped file header confirms the vector and hidden sizes above. Anyone reproducing the model should treat the script defaults as the starting point, not as a guarantee of identical results.

## Evaluation

Measured by `bench/scoreboard.sh` (the keyboard's own prediction code, replayed sentence by sentence) on **300 sentences per test set**, which is roughly 2,000 scored words per measure. All figures are percentages; "top 3" means the right word was among three suggestions; "top 2" is used after letters are typed because the keyboard (like Gboard) spends one of its three slots on the typed letters. Typical noise is about +-1 point on these sets (the project's own rule for a real change). Words no longer than the number of typed letters are skipped in the completion measures, and sentence-initial words are not scored against Gboard. Case-insensitive.

Test sets: see the [methods page](../../methods.md). Short version: everyday = Common Voice sentences (CC0); web = FineWeb-2's official test split, one sentence per page; Tatoeba = the id-ending-in-0 tenth. The web and Tatoeba sets are partly "home ground" for a model trained on web text and Tatoeba.

### What the model adds to the n-gram pack

Same n-gram pack in both columns (the published 2026-10-22 files); only the neural model differs.

| Test set | Measure | n-gram pack only | n-gram + this model | Change |
|---|---|---|---|---|
| Common Voice everyday sentences (CC0) | next word, right word in top 3 | 20.46 | 22.10 | +1.64 |
| Common Voice everyday sentences (CC0) | after 1 letter, top 2 | 44.61 | 46.46 | +1.85 |
| Common Voice everyday sentences (CC0) | after 3 letters, top 2 | 60.38 | 65.44 | +5.06 |
| FineWeb-2 test pages | next word, right word in top 3 | 26.11 | 31.97 | +5.86 |
| FineWeb-2 test pages | after 1 letter, top 2 | 46.80 | 54.07 | +7.27 |
| FineWeb-2 test pages | after 3 letters, top 2 | 65.05 | 73.70 | +8.65 |
| Tatoeba held-out tenth | next word, right word in top 3 | 31.87 | 32.79 | +0.92 |
| Tatoeba held-out tenth | after 1 letter, top 2 | 58.57 | 59.35 | +0.78 |
| Tatoeba held-out tenth | after 3 letters, top 2 | 71.10 | 74.47 | +3.37 |

### Against the keyboard before the open files

"Before" means the keyboard as it shipped until 2026-10-22 (for Spanish, French and German a Tatoeba-only n-gram file; for English FUTO's neural next-word model). Measured on the same sentences with the same tool.

| Test set (300 sentences) | Measure | Keyboard before the open files (Tatoeba-only pack) | n-gram + this model |
|---|---|---|---|
| Common Voice everyday sentences (CC0) | next word, top 3 | 18.24 | 22.10 |
| Common Voice everyday sentences (CC0) | after 1 letter, top 2 | 41.46 | 46.46 |
| FineWeb-2 test pages | next word, top 3 | 19.34 | 31.97 |
| FineWeb-2 test pages | after 1 letter, top 2 | 38.04 | 54.07 |
| Tatoeba held-out tenth | next word, top 3 | 31.73 | 32.79 |
| Tatoeba held-out tenth | after 1 letter, top 2 | 57.84 | 59.35 |

### Against Gboard 14.2 (emulator copy, learning off, tested 2026-10-22)

Points **ahead of** (+) or behind (-) Gboard's own suggestions on the exact queries Gboard was asked (100 sentences per language per set, recorded once from an Android emulator; see the [methods page](../../methods.md)). 0 letters: our top 3 versus Gboard's three suggestions; 1 to 3 letters: our top 2 versus Gboard's suggestions after it spends one slot on the typed letters.

| Gboard query set | Letters typed | n-gram pack only | n-gram + this model |
|---|---|---|---|
| everyday (Common Voice) | 0 | +3.64 | +5.97 |
| everyday (Common Voice) | 1 | +3.26 | +4.29 |
| everyday (Common Voice) | 2 | +4.34 | +9.43 |
| everyday (Common Voice) | 3 | +4.12 | +9.38 |
| web (FineWeb-2) | 0 | +3.78 | +8.99 |
| web (FineWeb-2) | 1 | +5.51 | +12.70 |
| web (FineWeb-2) | 2 | +6.97 | +14.56 |
| web (FineWeb-2) | 3 | +4.01 | +13.53 |

How to read this: Gboard had its learning from the user turned off, so it shows Gboard's out-of-the-box model, not what it does after weeks of use. One Gboard version (14.2, from an Android 35 emulator image) was tested. With 100 sentences per set the noise is roughly +-2 points. The web set is partly home ground for our model. We make no claim about Gboard in general. We used none of Gboard's code or data. Newer Gboard versions may differ, and Gboard's behaviour with learning on was not measured. The repository records no fuller Gboard build number than 14.2. No broader claim about Gboard is made.

## Known limitations

- **Web-text and Tatoeba style, not texting.** The training text is web pages and short example sentences. Slang, messaging style, emoji, code-switching and names are weak spots. The "everyday" set (Common Voice read-aloud sentences) is the closest we have to ordinary writing, and it is read-aloud style, not texting.
- **Vocabulary of about 24,000 words.** Rarer words are only ever suggested by the n-gram pack, never by this model.
- **Lower-cased letters only.** The model has no notion of capitals, accents-versus-base-letter handling beyond the vocabulary spelling, or punctuation; the keyboard handles those.
- **Test-set contamination was controlled but not eliminated:** the 8-word-run rule removes verbatim overlaps, not paraphrases. Perplexity (not recorded) was computed on random training sentences, not an independent set.
- **Tuned on the same sources as it is scored on:** blend weights were tuned on other sentences from the same three test files, not the 300 reported, but the style is the same.
- **Gain varies by test set and measure:** measured changes against the n-gram pack alone range from +0.78 (Tatoeba held-out tenth, after 1 letter, top 2) to +8.65 (FineWeb-2 test pages, after 3 letters, top 2) points; changes under about 1 point are within noise on 300 sentences.
- **The model reads only the current sentence** (at most the last 20 words); it does not use anything else on screen.
- No toxic-content filtering was applied to the model itself; the keyboard has a separate rule that never offers common swear words as an unprompted next word, and blocks suggestions in password fields.
- No fairness or bias evaluation has been done. Web-trained models reflect the web.

## How to reproduce

From a checkout of https://github.com/itsdestin/swipe-keyboard (needs a CUDA GPU for training, the Qt build for scoring). Data: the FineWeb / FineWeb-2 parquet files and Tatoeba export named in the scripts' headers; test sets from `scripts/lm/make-nextword-testsets.py`.

```
python -I scripts/lm/nwp/prepare.py fr fr.wordcounts.tsv fr.vocab.tsv OUT/data \
    --parquet <fineweb parquet files> --exclude <test set files> --tatoeba <tatoeba export>.bz2
python -I scripts/lm/nwp/train.py OUT/data OUT/model
python -I scripts/lm/nwp/export.py OUT/model/ckpt.pt OUT/data/lm_vocab.txt OUT/fr.nwp.bin --lambda 0.7 --beta 0.0
python -I scripts/lm/nwp/tune.py build/tools/swipekb-nextword-eval fr.ngram fr.vocab.tsv OUT/fr.nwp.bin <testsets dir> fr
SWIPEKB_PACK_fr=<folder with context.ngram + vocab.tsv> bench/scoreboard.sh --label "my run" --langs "fr"
```

The scoreboard needs the n-gram and word-list files from the GitHub release `language-files-2026-10-22-open-words` and the Gboard reference answers in `bench/gboard/reference/` (committed). The last command appends a row to `bench/scoreboard.tsv`.

## Licence of the model file

`fr.nwp.bin`: **ODC-By-1.0 AND CC-BY-2.0-FR** (SPDX-style expression; the file is derived from ODC-By 1.0 web text and CC BY 2.0 France Tatoeba sentences, and carries both attribution terms; no share-alike term applies).

Required credits when redistributing:

- **Models trained and released by Destin Moss via youcoded.ai, 2026**
- FineWeb / FineWeb-2 by Hugging Face, Open Data Commons Attribution License 1.0, derived from Common Crawl (Common Crawl's terms of use also apply).
- Tatoeba (tatoeba.org), CC BY 2.0 France. Keep the contributor list `fr-CONTRIBUTORS.txt` and the notice `TATOEBA-CC-BY-2.0-FR.txt` with the model.
- Keep the project's notices: `MODELS-NOTICE.md` (release `models-2026-10-23`) and `LANGUAGE-FILES-NOTICE.md`.

The surrounding code is licensed separately (see the repository's `LICENSE` and `packaging/NOTICE`). If you want to publish this card on Hugging Face, the licence field above is `other` because the expression above is not one of Hugging Face's identifiers.

## Credit

Models trained and released by Destin Moss via youcoded.ai, 2026.
