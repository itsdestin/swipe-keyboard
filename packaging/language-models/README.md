# Multilingual contextual n-gram packs

These are compact baseline, word-level modified Kneser–Ney trigram models built from Tatoeba sentence exports. They provide actual sentence-context-conditioned next-word probabilities; they are not unigram popularity lists or hand-authored phrase tables. Spanish `es_ES` and `es_MX` intentionally share the same `es` model.

## Data, license, and provenance

The source is the official per-language full sentence export and its matching detailed author export, fetched from `https://downloads.tatoeba.org/exports/per_language/{spa,fra,deu}/`. Snapshot byte sizes and SHA-256 pins for both exports, and the resulting model, vocabulary and complete contributor-credit file, are recorded under `multilingual_context_models` in `../assets.lock.json`. The lock was checked against the downloaded bytes on 2026-10-20; the official listings showed the snapshot dated 2026-10-17. The `*_sentences.tsv.bz2` schema is sentence ID, language ISO code, text. The `*_sentences_detailed.tsv.bz2` schema adds the author username; the training recipe joins on sentence ID and records every author of an included training sentence in the matching `*-CONTRIBUTORS.txt` file.

Tatoeba's official Terms of Use state that its text is CC BY 2.0 France and that the author's name must be cited. The terms URL and sentence-level contributor notices ship with each generated pack (`TATOEBA-CC-BY-2.0-FR.txt` and `<language>-CONTRIBUTORS.txt`). The model is an adaptation of those sentences; keep the notices with redistributed models. The separate wordfreq-derived swipe dictionary keeps its existing CC-BY-SA-4.0 notice. No sentence text is copied into source notices or logs.

No foreign FUTO neural weights are used: FUTO's published ContextLM is English-only. At runtime the model and vocabulary roles are format-versioned, language-tagged, and checked against the repository lock; missing, corrupt, mismatched, or unsupported assets do not enable contextual suggestions and never select English as fallback.

## Reproducible bounded build

1. Generate the pinned `assets/language-packs/{es,fr,de}.combined` dictionaries with the repository's existing `scripts/fetch-assets.sh` flow. Download each full and detailed Tatoeba export from the official index to a private scratch directory; the builder rejects any byte hash that differs from its hard-coded snapshot pins (which must match the lock).
2. Build the existing n-gram counter against the engine's exact scorer implementation (Qt Core supplies Unicode casefolding):

   ```sh
   g++ -O2 -std=c++20 -fPIC $(pkg-config --cflags Qt6Core) -Iengine/include \
     scripts/lm/ngram-build.cpp engine/src/ngram.cpp $(pkg-config --libs Qt6Core) -o ngram-build
   ```

3. For each language, run `python3 scripts/lm/build-tatoeba-model.py es spa_sentences.tsv.bz2 spa_sentences_detailed.tsv.bz2 assets/language-packs/es.combined OUT/es ngram-build` (substitute `fr fra` or `de deu`). The builder streams the compressed exports, NFC-normalizes and lowercases Unicode letter tokens, bounds its vocabulary to the existing roughly 100k word-frequency dictionary, and uses sentence ID modulo ten for a deterministic held-out split. It emits the binary n-gram, vocabulary with canonical lookup token / dictionary frequency / preserved display spelling, and author credits. The count tool writes a compact threshold-2 trigram model; there is no neural training or multi-gigabyte corpus. Final generated hashes must equal the checked-in lock before publication.

## Measured quality and limits

Sentence IDs select a deterministic held-out tenth that is excluded from training. The held-out partitions contain 44.2k Spanish, 72.6k French, and 78.4k German sentences. Current artifacts contain 99,959 / 99,908 / 99,849 vocabulary entries and 6,091,688 / 7,719,176 / 9,144,360 model bytes for es / fr / de. Each attribution file lists 1,884 / 1,955 / 1,688 distinct training contributors. Known held-out target-token top-1 accuracy from the most frequent training unigram versus previous-word-conditioned bigram counts was: Spanish 12,000/318,673 (3.8%) vs 43,096/318,673 (13.5%); French 17,764/522,983 (3.4%) vs 78,082/522,983 (14.9%); German 19,226/591,308 (3.3%) vs 68,508/591,308 (11.6%). This is a reproducible context-vs-unigram sanity comparison, not an end-user acceptance benchmark. The real runtime ranks using the full trigram model.

Tatoeba is a small, topic-skewed learner/community corpus; these baseline metrics do not establish broad conversational coverage, locale-specific Spanish quality, or parity with Gboard. German display spellings are selected from observed training surface forms; scores stay on the shared lowercase lookup word. Evaluate on independent natural-device typing before making stronger quality claims.
