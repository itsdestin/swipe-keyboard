# Language files (next-word, completion and swipe-ranking n-grams)

English, Spanish, French and German each have a word-level trigram model (`<lang>.ngram`), a word list
(`<lang>.vocab.tsv`) and a Tatoeba credit file (`<lang>-CONTRIBUTORS.txt`). They are **not stored in this repository**:
they are published once as the GitHub release `language-files-2026-10-22` and every build downloads them by
SHA-256 (`language_files` and `multilingual_context_models` in `../assets.lock.json`; `scripts/fetch-assets.py`
puts them in `assets/language-files/`, `scripts/dict/language_pack.py` copies them into `assets/language-packs/`,
and the installer/packages ship that folder). A file that does not match its pin is refused at build time and again
at run time.

What stays in this folder: `TATOEBA-CC-BY-2.0-FR.txt` (the Tatoeba terms notice, pinned by hash) and
`LANGUAGE-FILES-2026-10-22-NOTICE.md` (the same text as the release's `NOTICE.md`, installed as
`language-packs/LANGUAGE-FILES-NOTICE.md`).

## Sources and licences

- Web text: FineWeb (English) and FineWeb-2 (Spanish, French, German) by Hugging Face, Open Data Commons Attribution
  License 1.0 (`ODC-By-1.0.txt`); derived from Common Crawl, whose terms of use also apply. Only word counts are kept.
- Short everyday sentences: Tatoeba, CC BY 2.0 France. Every contributing author is listed in `<lang>-CONTRIBUTORS.txt`
  and the terms are in `TATOEBA-CC-BY-2.0-FR.txt`. Both must travel with the models.
- Word lists (`<lang>.vocab.tsv`) and the `<lang>.combined` swipe dictionaries of the files published 2026-10-22 (the
  ones pinned today): derived from wordfreq 3.1.1 (Robyn Speer), CC BY-SA 4.0 (`CC-BY-SA-4.0.txt`; wordfreq's code is
  Apache-2.0). **Being replaced** by word lists counted from the text above, see "Word lists counted from open text".
- No FUTO neural weights or data are in these files. FUTO's swipe encoder/decoder is still used for the swipe shape itself.

Each pack manifest (`es.json`, `fr.json`, `de.json`) records `contextLicense` with id `ODC-By-1.0 AND CC-BY-2.0-FR` and the
hashes of the credit, Tatoeba, ODC-By and notice texts; the loader compares all of it with the repository lock, so a
manifest cannot vouch for itself. English has no manifest of its own (its swipe files stay under `models/`); the loader
checks the same files against the same lock.

## How the keyboard uses them

- Spanish, French, German: swipe ranking, next-word suggestions and completions use the language's n-gram.
- English: when `language-packs/en.ngram` + `en.vocab.tsv` and their credit/licence texts are present and verified,
  English uses them for next-word suggestions, completions and swipe ranking instead of FUTO's `hungry_jellyfish` model.
  If any of them is missing or fails verification, English silently keeps using FUTO's model, as before.
- If a Spanish, French or German file is missing or fails verification, that language still swipes and types with its
  dictionary; only the suggestions are switched off.

## Rebuilding the files

`scripts/lm/build-fineweb2-model.py` builds a language's n-gram from FineWeb text plus Tatoeba (sentences weighted 30x;
word pairs kept when seen 20+ times, triples 40+), `scripts/lm/tatoeba-credits.py` writes the credit files, and
`scripts/lm/make-vocab.py` the word lists. After publishing a new release, update `language_files.baseUrl` and the hashes
in `../assets.lock.json` (every hash and byte size is checked by `tests/packaging`). The earlier small Tatoeba-only packs
(`scripts/lm/build-tatoeba-model.py`) were replaced by these files and are no longer shipped.

## Word lists counted from open text (staged for the next release, not yet pinned)

Nothing in the share-alike chain is needed any more: the word lists are counted from the same FineWeb / FineWeb-2 slice
and Tatoeba training sentences as the n-grams, so they carry only attribution terms (ODC-By 1.0, CC BY 2.0 FR; the
English swipe dictionary also keeps AOSP's list, Apache-2.0). The notice for the release is drafted in
`LANGUAGE-FILES-NEXT-NOTICE.draft.md`.

1. `scripts/lm/count-words.py LANG OUT.tsv --parquet ... --exclude <3 test sets> --tatoeba ...` writes
   `LANG.wordcounts.tsv`: per word the occurrences, distinct pages and usual spelling (capitals kept for names and German
   nouns; sentence-initial capitals ignored). Same text slice and token rules as `build-fineweb2-model.py`; test text never
   feeds the counts. These four files are the pinned inputs of everything below.
2. `scripts/dict/build-corpus-dictionaries.sh COUNTS_DIR AOSP_DIR OUT_DIR` builds `{es,fr,de}.combined` (top 100k words)
   and `en_swipe.combined` (AOSP kept exactly, plus up to 250k words AOSP lacks). Filters: 3+ pages, the language's letters,
   likely misspellings, words far more typical of another of the four languages, web debris
   (`scripts/dict/corpus_dictionary.py`; tests in `tests/test_corpus_dictionary.py`).
3. `build-fineweb2-model.py` + `ngram-build` rebuild the four n-gram packs against those dictionaries (the vocabulary ids
   are the dictionary's words, so a new dictionary always means a new `.ngram` + `.vocab.tsv`).

Switching the pins is a separate step that needs the owner's approval of a new release: upload the new files, replace the
hashes in `../assets.lock.json` (`language_files`, `multilingual_context_models`, `dictionary_sha256`), generate the packs
from the counts instead of the wordfreq wheel, accept the open licence in `plugin/languageprofile.cpp` and
`scripts/dict/language-pack.schema.json` (they still require `CC-BY-SA-4.0` and the wheel), then drop the wheel,
`CC-BY-SA-4.0.txt`, `scripts/dict/wordfreq_combined.py`, `build-en-swipe.sh` and the wordfreq sentences in
`packaging/NOTICE` / `docs/install-details.md`.
