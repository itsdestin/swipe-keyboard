# Texting abbreviations and slang

`LANG.tsv` (en, es, fr, de): short forms people really type in messages (lol, idk, wyd; jaja, xq; mdr, jsp;
lg, vllt) that the web-text word lists count too rarely, or mark "not a word". The keyboard adds them to that
language's swipe dictionary when it starts, so they can be swiped and completed out of the box.

## Licence

CC0 1.0 (public-domain dedication). These lists are the project's own hand-written work: the words are common
usage, the numbers are our own choices; nothing was copied from another list, app or dictionary. To the extent
possible under law the author has waived all copyright and related rights.

## Format

One entry per line, tab-separated: `word`, `strength`, `tag`, `note`. Lines starting with `#` are comments.
- `word`: lower-case letters of the language only (no digits, no apostrophes: a swipe cannot draw them). It is
  shown exactly as written here.
- `strength`: 1-255, the same scale as the dictionary ("the" is about 222, a middling everyday word 60-110).
- `tag`: `-`, or `swear` for an abbreviation of a swear word. Swear-tagged words can still be swiped and
  completed, but are never offered as a next-word suggestion before anything is typed (same rule as
  `packaging/blocked-words`).
- `note`: free text for people; ignored by the keyboard.

A strength only ever LIFTS a word: if the dictionary already has the word stronger, the dictionary's number stays.

## How to choose a strength

1. Start from how often the form is typed in chat: the most universal (lol, omg, jaja, mdr, lg) 130-150,
   common ones (idk, tbh, jsp, vllt) 110-135, regional or niche ones 85-110. Nothing goes above 150: even
   the strongest abbreviation must stay below the dictionary's everyday words (the, to, of: 210-222).
2. Short forms (two or three letters) that look like a swipe of a more common word are the risk. Strength is
   limited by measurement (next point), never by taste.
3. Measure before shipping a change: swipe the real test sets with and without the list (the English real-swipe test sets
   bench-2000 and test-extra-10000, HeliBoard en/es/fr/de test sets; both swipe engines where they apply).
   The loss in first-guess accuracy must stay under 0.2 points on every set; lower or drop any entry that
   costs ordinary swipes. With the strengths here the measured change was at most 0.02 points (see CHANGELOG).
   `bench/texting-dict.py` builds the "after" dictionary the benches read.
