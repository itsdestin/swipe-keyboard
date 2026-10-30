<!--
SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
SPDX-License-Identifier: MIT
-->
# Tap-to-swap results (card #500)

## In plain words

- When your first swipe guess is wrong, the right word now shows up much more often, both in the bar after a swipe and when you tap a word to swap it. The gain is largest in English tap-back lists.
- The price: in Spanish, French and German, tap-back lists show more rare words than today, past the +3-point limit we set for ourselves (details below). English tap-back lists and the bar after a swipe show fewer rare words than today.
- The bar after a swipe shows less junk than today everywhere except HeliBoard English (9.44 to 10.30).
- Swipe first guess and next-word suggestions are unchanged. Memory: about +1 MB and +35 ms load for English.

Numbers are copied from `bench/swap-scoreboard.tsv`: baseline = label "baseline 0.5.0 (WordSwap::build)" (rows from 43b9e56), new = the one-brain rows (English: 43b9e56; Spanish/French/German: 2170b6c, the last run, floor off). Cells are first option / among the first three, in percent. Only swipes whose first guess was wrong count ("cases"), so the lists are judged where they matter. Nothing is rounded up.

## What is measured

- **After swipe**: the bar's other places right after a swipe.
- **Tap-back, with the swipe's guesses remembered** / **without** (a word you typed, or the guesses are gone).
- **Junk**: when the first guess was right, the share of the other places that are stretched, rare or rare names. Bar junk = after swipe; tap-back junk = the three tap-back options.

## Results per test set (baseline -> new)

### bench-2000 (English, 2000 swipes, 198 cases)
| measure | baseline | new |
|---|---|---|
| after swipe | 19.70 / 30.81 | 24.24 / 36.36 |
| tap-back with swipe | 19.70 / 33.33 | 36.36 / 42.42 |
| tap-back without | 12.12 / 16.67 | 26.26 / 32.32 |
| junk: bar / tap-back with / without | 6.04 / 10.02 / 17.69 | 4.82 / 7.33 / 5.12 |
| ms per tap-back list / after swipe | 2.25 / n.a. | 9.43 / 7.07 |

### test-extra-10000 (English, 10000 swipes, 957 cases)
| measure | baseline | new |
|---|---|---|
| after swipe | 24.66 / 32.60 | 28.63 / 36.36 |
| tap-back with swipe | 24.76 / 34.38 | 35.01 / 42.01 |
| tap-back without | 10.76 / 15.57 | 26.44 / 31.87 |
| junk: bar / tap-back with / without | 6.29 / 9.66 / 16.07 | 4.90 / 7.85 / 5.30 |
| ms per tap-back list / after swipe | 1.86 / n.a. | 9.57 / 7.12 |

### HeliBoard English (single words, 5000 swipes, 962 cases; test only)
| measure | baseline | new |
|---|---|---|
| after swipe | 30.35 / 43.14 | 37.32 / 50.21 |
| tap-back with swipe | 30.35 / 45.43 | 39.60 / 59.56 |
| tap-back without | 8.32 / 17.98 | 18.61 / 32.22 |
| junk: bar / tap-back with / without | 9.44 / 19.33 / 37.54 | 10.30 / 13.17 / 9.98 |
| ms per tap-back list / after swipe | 1.76 / n.a. | 11.76 / 7.05 |

### HeliBoard Spanish (5000 swipes, 1201 cases; test only)
| measure | baseline | new |
|---|---|---|
| after swipe | 26.73 / 35.72 | 29.98 / 40.72 |
| tap-back with swipe | 26.73 / 36.39 | 29.98 / 45.30 |
| tap-back without | 5.83 / 12.91 | 10.91 / 18.90 |
| junk: bar / tap-back with / without | 41.79 / 36.80 / 28.52 | 40.13 / 41.48 / 37.55 |
| ms per tap-back list / after swipe | 1.05 / n.a. | 8.87 / 6.00 |

### HeliBoard French (5000 swipes, 1583 cases; test only)
| measure | baseline | new |
|---|---|---|
| after swipe | 29.88 / 42.51 | 32.79 / 47.69 |
| tap-back with swipe | 29.88 / 44.41 | 32.79 / 54.64 |
| tap-back without | 8.78 / 19.77 | 12.51 / 23.18 |
| junk: bar / tap-back with / without | 40.76 / 35.27 / 26.51 | 38.44 / 38.99 / 35.48 |
| ms per tap-back list / after swipe | 1.16 / n.a. | 9.53 / 6.33 |

### HeliBoard German (5000 swipes, 1764 cases; test only)
| measure | baseline | new |
|---|---|---|
| after swipe | 18.54 / 23.64 | 23.19 / 29.48 |
| tap-back with swipe | 18.54 / 24.49 | 23.19 / 31.46 |
| tap-back without | 4.88 / 9.92 | 9.92 / 15.25 |
| junk: bar / tap-back with / without | 18.72 / 17.07 / 13.50 | 17.23 / 18.11 / 18.64 |
| ms per tap-back list / after swipe | 1.11 / n.a. | 9.24 / 6.10 |

All list times are under the 20 ms limit for the input thread.

### Known cost: rare words in Spanish, French and German tap-back lists
Tap-back junk, new minus baseline (points): Spanish +4.68 with swipe, +9.03 without; French +3.72, +8.97; German +1.04, +5.14. Our limit was +3, so Spanish and French (both) and German (without the swipe) are over it. Why: outside English the engine has no rare-word cutoff, and the swap search offers rarer words that fit the swipe path well. Every weight setting that stayed within the limit on the tuning slices lost correct words against today's keyboard. A rare-word cutoff for these languages was tried (French/Spanish 80, German 66): it cut junk below 11 %, but on the HeliBoard test words it lowered the right-word rate below today's keyboard in Spanish and French, so it is off. The tuning slices (48 and 47 cases) are sentences, HeliBoard is single words, and they disagreed; we did not re-tune on the test set. A larger single-word Spanish/French/German tuning set is the fix to try next.

## Diagnosis

On the English tuning slice (trv8k, 514 wrong first guesses): when the right word was gathered into the list, ranking put it in the top three 227 of 236 times (96 %) with the swipe's guesses. Ranking was fine; gathering was the problem. About a third (170 of 514, 33.07 %) of misses are not dictionary words (names, hyphenated compounds), so swapping can never fix them. The rest were mostly a too-narrow search, the rare-word rule, and typed words having no swipe path to compare.

## Rules followed

- Tuning only on trv8k (English) and the FUTO swipe-5 validation slices (French, German). Spanish uses French's settings.
- bench-2000, test-extra-10000 and HeliBoard are report-only, never tuned on. HeliBoard (CC BY-SA) is used for testing only.
- A first/in-3 gain is never rounded up; first and in-3 are always shown separately.

## Junk definition

A word is rare when it is not in the language's top N words by rank (English, French, Spanish N = 31,456; German N = 48,000), and a rare name is capitalised and not in the top 4,388. For English this equals the old rule (frequency under 80, names under 120), so English numbers did not change. Earlier es/fr/de rows used "under 80 / under 120" and are not comparable.

## Tuned settings

Weights path 1, before-words 0, after-words 1, neural 1; search width 256; 24 search words plus 12 one-letter-off neighbours (fit floor 15); typed-word path temperature 2. Tap-back lists only: 16 words guessed from the sentence, rare words allowed when they fit within 1 nat of the tapped word. Words with no sentence around them (single words) use path/before/neural weights 0 / 0 / 0, i.e. the swap search's own order.

## Not measured

- Destin's own typing.
- Typos made by hand (every case is a real swipe whose first guess was wrong).
- Gboard's swap.

## Unchanged

Swipe first guess: bench-2000 first guess 89.20 / in-3 93.10, identical to 0.5.0 (`bench/score.sh --check` pass). Next-word check, all languages: pass.

## Memory and load time vs origin/main (English predictor, 3 runs each)

origin/main (f480997): 57.8-58.0 MB, 222-225 ms. This branch: 58.8-59.0 MB, 255-266 ms. About +1.0 MB and +35 ms load.
