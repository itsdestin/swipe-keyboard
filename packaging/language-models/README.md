# Language files (next-word, completion and swipe-ranking n-grams)

English, Spanish, French and German each have a word-level trigram model (`<lang>.ngram`), a word list
(`<lang>.vocab.tsv`) and a Tatoeba credit file (`<lang>-CONTRIBUTORS.txt`). They are **not stored in this repository**:
they are published once as the GitHub release `language-files-2026-10-22-open-words` and every build downloads them by
SHA-256 (`language_files` and `multilingual_context_models` in `../assets.lock.json`; `scripts/fetch-assets.py`
puts them in `assets/language-files/`, `scripts/dict/language_pack.py` copies them into `assets/language-packs/`,
and the installer/packages ship that folder). A file that does not match its pin is refused at build time and again
at run time.

What stays in this folder: `TATOEBA-CC-BY-2.0-FR.txt` (the Tatoeba terms notice, pinned by hash) and
`LANGUAGE-FILES-2026-10-22-open-words-NOTICE.md` (the same text as the release's `NOTICE.md`, installed as
`language-packs/LANGUAGE-FILES-NOTICE.md`).

## Sources and licences

- Web text: FineWeb (English) and FineWeb-2 (Spanish, French, German) by Hugging Face, Open Data Commons Attribution
  License 1.0 (`ODC-By-1.0.txt`); derived from Common Crawl, whose terms of use also apply. Only word counts are kept.
- Short everyday sentences: Tatoeba, CC BY 2.0 France. Every contributing author is listed in `<lang>-CONTRIBUTORS.txt`
  and the terms are in `TATOEBA-CC-BY-2.0-FR.txt`. Both must travel with the models.
- Word lists (`<lang>.vocab.tsv`) and the `<lang>.combined` swipe dictionaries (also `en_swipe.combined`): counted from the
  same two sources, so they carry the same attribution terms (no share-alike). The English dictionary additionally keeps
  every word, frequency and flag of the Android LatinIME US English list, Apache-2.0 (`Apache-2.0.txt`).
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

## Trained models (release `models-2026-10-23`)

Models trained and released by Destin Moss via youcoded.ai, 2026. `trained_models` in `../assets.lock.json` pins, by SHA-256
and size, `en/es/fr/de.nwp.bin` (neural next-word models, ODC-By-1.0 AND CC-BY-2.0-FR, trained on the same FineWeb /
FineWeb-2 and Tatoeba text), `swipe-model.bin` (MIT; trained on FUTO's MIT swipe.futo.org data plus our synthetic swipes), the
release `NOTICE.md` (installed as `MODELS-NOTICE.md`) and the MIT text of FUTO's swipe data. `scripts/fetch-assets.py`
puts the next-word files in `assets/language-files/` (`language_pack.py` copies them next to `LANG.ngram` in
`language-packs/`) and the swipe model in `assets/models/`; both notices go to `assets/licenses/`.
At run time (`plugin/languageprofile.cpp`) a `LANG.nwp.bin` is used only when its size and SHA-256 equal the lock; the
engine is handed that exact path and never picks up an unchecked neighbour. A missing or altered file turns off only that
model: the n-gram keeps working, and the language is never disabled. `swipe-model.bin` is checked the same way before the
own engine loads it (mismatch: geometry only). The `SWIPEKB_SWIPE_MODEL` environment override (trials and tests) is not checked.
Test-only: `SWIPEKB_TEST_MODELS_MIRROR=<folder or URL>` makes `fetch-assets.py` read these files from a local folder instead
of the release (the pins still decide what is accepted).

## Rebuilding the files

`scripts/lm/build-fineweb2-model.py` builds a language's n-gram from FineWeb text plus Tatoeba (sentences weighted 30x;
word pairs kept when seen 20+ times, triples 40+), `scripts/lm/tatoeba-credits.py` writes the credit files, and
`scripts/lm/make-vocab.py` the word lists. After publishing a new release, update `language_files.baseUrl` and the hashes
in `../assets.lock.json` (every hash and byte size is checked by `tests/packaging`). The earlier small Tatoeba-only packs
(`scripts/lm/build-tatoeba-model.py`) were replaced by these files and are no longer shipped.

## Word lists: how they are made and rebuilt

1. `scripts/lm/count-words.py LANG OUT.tsv --parquet ... --exclude <3 test sets> --tatoeba ...` writes
   `LANG.wordcounts.tsv`: per word the occurrences, distinct pages and usual spelling (capitals kept for names and German
   nouns; sentence-initial capitals ignored). Same text slice and token rules as `build-fineweb2-model.py`; test text never
   feeds the counts. The four count files are published in the release (not fetched by the installer).
2. `scripts/dict/build-corpus-dictionaries.sh COUNTS_DIR AOSP_DIR OUT_DIR` builds `{es,fr,de}.combined` (top 100k words) and
   `en_swipe.combined` (AOSP kept exactly, plus up to 250k words AOSP lacks). **It replaces the old
   `build-en-swipe.sh` / `wordfreq_combined.py`**: the AOSP inputs stay pinned in `assets.lock.json` (`dict-src/*.b64`,
   fetched by `scripts/fetch-assets.py`; decode with `base64 -d X.gz.b64 | gunzip > X`) and are the `AOSP_DIR` input, so the
   English dictionary is rebuildable. Filters: 3+ pages, the language's letters, likely misspellings, words far more typical of
   another of the four languages, rare all-capital abbreviations, old German ß spellings, web debris
   (`scripts/dict/corpus_dictionary.py`; tests in `tests/test_corpus_dictionary.py`). The installer does not rebuild them: it
   downloads the pinned files.
3. `build-fineweb2-model.py` + `ngram-build` rebuild the four n-gram packs against those dictionaries (the vocabulary ids
   are the dictionary's words, so a new dictionary always means a new `.ngram` + `.vocab.tsv`).

What the keyboard checks at run time (`plugin/languageprofile.cpp`): the manifest's licence set must be `ODC-By-1.0 AND
CC-BY-2.0-FR` with the notice, ODC-By text and Tatoeba text matching the lock (a damaged licence text makes the language
unavailable; a damaged n-gram or credit list only removes suggestions); English's n-gram additionally needs `Apache-2.0.txt`.

Measured when the open word lists replaced the wordfreq ones, 2026-10-22 (published files vs these; 2,000 test sentences per
set): no measure fell more than 0.1 point. Next word right in top 3 and completion after 1 and 3 letters right in top 2:
-0.09 to +2.0 points on every set and language (French gained most). English real swipes (2,000, with the English n-gram):
first guess 93.11 -> 93.01 %, top 3 96.40 -> 96.30 %. Spanish/French/German synthetic swipes (400 words each): first guess
88.25/83.00/87.50 -> 89.00/83.25/87.75 %, top 3 98.50/93.75/96.75 -> 98.25/93.00/97.00 %. Words of the test sentences found
in the word list: English 99.7/97.9/98.8 -> 99.7/98.0/99.8 %, French 95.0/95.1/96.8 -> 96.7/97.1/98.7 %, German
95.6/92.8/96.9 -> 95.9/93.2/97.5 %, Spanish 97.9/97.8/98.7 -> 97.4/97.3/98.5 % (slightly lower: single letters and slang).
The German synthetic targets are now spelled as the dictionary shows them (`bench/languages/display-targets.py`).
