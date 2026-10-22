# Swipe Keyboard language files, 2026-10-22

Next-word and completion files for English, Spanish, French and German (3-word n-gram models,
medium size). Built by `scripts/lm/build-fineweb2-model.py` (github.com/itsdestin/swipe-keyboard,
commit d5859a1), FineWeb text capped at 300 million words per language plus Tatoeba weighted 30x;
counts kept when seen at least 20 times (word pairs) / 40 times (word triples). No neural network,
no FUTO data or code.

| File | What it is |
|---|---|
| `LANG.ngram` | word-sequence statistics |
| `LANG.vocab.tsv` | word list: letters-only form, frequency, display spelling |
| `LANG-CONTRIBUTORS.txt` | Tatoeba authors whose sentences were used |
| `SHA256SUMS` | checksums |

## Sources and licences

- **Web text** — FineWeb-2 (Spanish, French, German) and FineWeb sample-10BT (English), by Hugging Face,
  under the Open Data Commons Attribution License v1.0 (`ODC-By-1.0.txt`); FineWeb is built from
  Common Crawl, and its use is also subject to Common Crawl's terms of use. Only word counts are kept,
  never text.
  https://huggingface.co/datasets/HuggingFaceFW/fineweb-2 · https://huggingface.co/datasets/HuggingFaceFW/fineweb
- **Short everyday sentences** — Tatoeba (tatoeba.org), CC BY 2.0 FR (`TATOEBA-CC-BY-2.0-FR.txt`);
  authors listed in `LANG-CONTRIBUTORS.txt`. Sentences with id ending in 0 were held out for testing.
- **Word lists** (`LANG.vocab.tsv`, and which words the `.ngram` files cover) — derived from wordfreq 3.1.1
  frequency data by Robyn Speer and contributors, CC BY-SA 4.0 (`CC-BY-SA-4.0.txt`; wordfreq's code is
  Apache-2.0, `wordfreq-Apache-2.0-LICENSE.txt`); the English list also from Android (AOSP) LatinIME word
  lists, Apache-2.0. Because of wordfreq's share-alike term, the vocabulary files are offered under
  CC BY-SA 4.0.

Changes made: text was split into sentences, lower-cased, reduced to letter words, sentences sharing an
8-word run with our test sets removed, then counted. https://github.com/itsdestin/swipe-keyboard
