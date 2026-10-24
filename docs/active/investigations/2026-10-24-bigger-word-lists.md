---
status: active
---

# Bigger Spanish, French and German word lists (on hold for 0.5.1)

Today each of the three languages ships 100,000 words; the German list lacks one in five of the HeliBoard test words.
This note records what larger lists do. **Nothing here is published or pinned**; Destin wants more refinement of word
selection and more accuracy testing first. Measured 2026-10-24 on the laptop.

## How the lists were made

`scripts/dict/corpus_dictionary.py` from the published word counts (`LANG.wordcounts.tsv` in release
`language-files-2026-10-23-apostrophes`, byte-identical to the counts used), only `--max` raised. The first 100,000
words are exactly today's list; without a drop file the builder still reproduces today's files byte for byte. Sources
and licences are unchanged (FineWeb ODC-By 1.0, Tatoeba CC BY 2.0 FR); no HeliBoard data is in any list (its swipes
are used for testing only).

## The problem a bigger list brings, and the filter

The added words are mostly names (34 % of the added Spanish words, 44 % French, 77 % of German are capitalised) and
fragments. Some draw nearly the same swipe line as a common word and win it: French amour → "Ammor", team → "Taal",
suivi → "Sivo"; German computer → "Compper", zahl → "zal"; Spanish aeropuerto → "apeó". WHY they win: the next-word
model never saw them, so they get a fixed score from their frequency, while a common word's score depends on the
sentence.

`tools/swipekb-shadow-words` swipes each of the 20,000 most common words (exact swipe plus 4 slightly wobbly ones,
fixed seed) on the language's real layout through the keyboard's engine, and lists every added word that wins the exact
swipe or 2 of the 5; repeated until nothing more steals. `corpus_dictionary.py --drop` leaves them out. The saved drop
files (`scripts/dict/swipe-shadows/`) rebuild the scored lists byte for byte. Dropped: Spanish 384, French 467, German
246 words. Mostly junk (missing accents "d’electricite", "epouser"; old or misspelled forms "Fussgängerzone",
"nourissent"; elisions without apostrophe "laffection"; names "Vallot", "Gellar"); a few rare real words go too
(French "grisées", German "ätzen", "geartet"), because each beat a far commoner word on that word's own swipe.

## Results

HeliBoard real swipes (test only, no sentence; first guess right / in top 3, %; "in list" = share of test words the
list contains). Rows without "filtered" are the raw lists.

| list | Spanish | French | German |
|---|---|---|---|
| today, 100k | 75.9 / 84.7 (90.3) | 68.2 / 82.0 (92.2) | 64.7 / 73.1 (80.0) |
| 150k | 77.4 / 87.6 (94.2) | 68.3 / 83.8 (95.5) | 68.4 / 77.4 (85.6) |
| 200k | 77.3 / 88.5 (96.0) | 67.8 / 84.1 (96.7) | 70.0 / 79.8 (88.8) |
| 260k | 76.7 / 88.9 (97.0) | 67.3 / 84.1 (97.2) | 71.4 / 81.5 (91.3) |
| 150k filtered | | **68.8 / 84.0** (95.5) | |
| 200k filtered | **77.7 / 88.6** (95.9) | 68.5 / 84.2 (96.6) | |
| 260k filtered | 77.1 / 89.0 (96.9) | | **71.6 / 81.5** (91.3) |
| 330k filtered | | | 72.8 / 83.0 (93.4) |
| FUTO keyboard 0.4.1, same swipes | 77.7 / 85.1 | 64.6 / 76.9 | 69.7 / 76.4 |

The filter never saw these swipes and still raised every language (French +0.5 first guess). The two filter
strengths (exact swipe only, or with wobbly swipes) scored identically.

Sentence check (500 Tatoeba + 500 everyday held-out sentences per language, every word swiped with wobble and the
earlier words known; first / top 3, %), today → recommended filtered list:

| | Spanish 200k | French 150k | German 260k |
|---|---|---|---|
| swipe, all words | 92.58 / 97.19 → 92.46 / 97.43 | 91.47 / 95.73 → 91.53 / 96.03 | 90.25 / 95.48 → 91.26 / 96.65 |
| capitals kept (mid-sentence) | 98.76 → 98.73 | 99.01 → 98.92 | 97.61 → 97.64 |
| next word | unchanged (16.66 / 28.80) | unchanged | unchanged |

Spanish first guess is 0.12 points lower (9 words of 7,732): within noise, but not a gain. Next-word and completion
suggestions do not read the swipe list, so the next-word/Gboard scoreboard cannot move.

5,000-common-word probe (exact swipes on a simple practice board): spelling mistakes (apostrophes, capitals)
unchanged from today in every language. Common words another word wins: Spanish 224 → 225, French 425 → 429,
German 285 → 285 (raw lists without the filter: 228, 440, 295). The French losses (suivi → "Sivo", rayon → "Ratton")
happen on the practice board, not on the real French layout.

Speed and size (quiet laptop, after a reboot):

| | each swipe | word-list build pause* | loading the language | memory | file |
|---|---|---|---|---|---|
| today 100k | 2 ms | 140-175 ms | 0.3 s | 103-123 MB | 2.1-2.2 MB |
| Spanish 200k | 2 ms | 275 ms | 0.65 s | 192 MB | 4.2 MB |
| French 150k | 2 ms | 200 ms | 0.45 s | 159 MB | 3.1 MB |
| German 260k | 2 ms | 467 ms | 1.0 s | 316 MB | 6.0 MB |
| German 330k | 2 ms | 595 ms | 1.3 s | 333 MB | 7.4 MB |

\* once on the first swipe after the keyboard starts or changes layout, and when it learns or forgets one of your own
words. The HeliBoard bench reports German at 200-800 ms per swipe because its test swipes come from four different
keyboards (with and without ß/ä/ö/ü), so it rebuilds the list on more than half of them; a real keyboard does not.

English: the 250k list already holds every HeliBoard English test word; what real sentences contain beyond it is
mostly misspellings ("asprin", "alot"), contraction pieces and codes. A bigger English list is not recommended.

## Recommendation

Spanish 200k, French 150k, German 260k, each with its drop file (German 330k if a 0.6 s first-swipe pause and 1.3 s
load are acceptable: +1.2 / +1.5 points). With them Spanish ties FUTO on first guess (77.7) and beats it on top 3,
French and German beat it on both; German still misses 8.7 % of the test words.

Open before shipping: the Spanish sentence check is flat rather than better; the filter uses exact and lightly wobbly
swipes on one layout per language; the build pause could be moved to load time. To ship: build with
`--max` above and `--drop scripts/dict/swipe-shadows/LANG.txt` (update `build-corpus-dictionaries.sh`), add a
sentence about the filter to the release notice's "changes made", publish a new language-files release and pin it.
