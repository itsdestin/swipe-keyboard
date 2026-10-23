# Swipe Keyboard language files, NEXT RELEASE (draft, not published, not pinned)

DRAFT of the `NOTICE.md` for the release that replaces `language-files-2026-10-22`. Nothing here is live: the pins in
`assets.lock.json` still point at the 2026-10-22 files, whose word lists derive from wordfreq (CC BY-SA 4.0). When the
owner approves the new release, this text becomes its `NOTICE.md` (copy it to
`LANGUAGE-FILES-<date>-NOTICE.md`, drop the "DRAFT" paragraph) and the paragraphs under "Replacement text" below
replace the matching ones in `packaging/NOTICE` and `docs/install-details.md`.

Next-word and completion files for English, Spanish, French and German (3-word n-gram models, XS size), the word
lists they use, and the swipe dictionaries for Spanish, French and German. Built by `scripts/lm/count-words.py`,
`scripts/dict/corpus_dictionary.py` and `scripts/lm/build-fineweb2-model.py` (github.com/itsdestin/swipe-keyboard):
FineWeb text capped at 300 million words per language plus Tatoeba (n-gram: weighted 30x; word counts: once). No
neural network, no FUTO data or code, and no wordfreq data.

| File | What it is |
|---|---|
| `LANG.ngram` | word-sequence statistics |
| `LANG.vocab.tsv` | word list: letters-only form, frequency, display spelling |
| `LANG.combined`, `en_swipe.combined` | swipe dictionaries (word + frequency, the format the keyboard loads) |
| `LANG.wordcounts.tsv` | how often each word occurs, on how many pages, and how it is usually spelled (input of the dictionaries) |
| `LANG-CONTRIBUTORS.txt` | Tatoeba authors whose sentences were used |
| `SHA256SUMS` | checksums |

## Sources and licences

- **Web text** — FineWeb-2 (Spanish, French, German) and FineWeb sample-10BT (English), by Hugging Face, under the
  Open Data Commons Attribution License v1.0 (`ODC-By-1.0.txt`); FineWeb is built from Common Crawl, and its use is
  also subject to Common Crawl's terms of use. Only word counts are kept, never text.
- **Short everyday sentences** — Tatoeba (tatoeba.org), CC BY 2.0 FR (`TATOEBA-CC-BY-2.0-FR.txt`); authors listed in
  `LANG-CONTRIBUTORS.txt`. Sentences with id ending in 0 were held out for testing and are not counted.
- **Word lists** (`LANG.vocab.tsv`, `LANG.wordcounts.tsv`, the Spanish/French/German swipe dictionaries) — counted from
  the two sources above, so they carry the same terms (attribution; no share-alike). The English swipe dictionary
  also keeps every word, frequency and flag of the Android (AOSP) LatinIME US English word list, Apache-2.0
  (`https://android.googlesource.com/platform/packages/inputmethods/LatinIME`, revision in `assets.lock.json`).

Changes made: text was split into sentences, reduced to letter words (words glued to digits or addresses were
skipped), sentences sharing an 8-word run with our test sets removed, then counted; words seen on fewer than 3 pages,
likely misspellings (one typo from a far commoner word), page debris and words far more typical of another of the
four languages were dropped. https://github.com/itsdestin/swipe-keyboard

## Replacement text for `packaging/NOTICE` (word-list sentences)

> The English dictionary keeps every word of the Android (AOSP) LatinIME US English list (Apache-2.0, original notices
> retained) and adds words counted from FineWeb text (ODC-By 1.0); the Spanish, French and German dictionaries and all
> `<language>.vocab.tsv` word lists are counted from FineWeb / FineWeb-2 text (ODC-By 1.0, derived from Common Crawl,
> whose terms of use also apply) and Tatoeba sentences (CC BY 2.0 France). No share-alike term applies; keep the
> attribution texts (`ODC-By-1.0.txt`, `LANGUAGE-FILES-NOTICE.md`, the Tatoeba notice and contributor lists).
> Transformation: `scripts/lm/count-words.py`, `scripts/dict/corpus_dictionary.py`.
