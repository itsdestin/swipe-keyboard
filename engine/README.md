# Swipe engine

The library behind the Swipe Keyboard: swipe recognition, word completion, spelling alternatives, next-word suggestions and learning. It is plain C++20, MIT-licensed, and has no keyboard or Plasma dependency. The only external requirement is **Qt 6 Core (6.8 or newer)**, used for Unicode handling (Qt Core is LGPL-3.0 and is linked dynamically). Everything runs on the device; the library never touches the network.

Languages: English, Spanish, French, German. Any key layout works, because you hand it the key rectangles.

## What goes in, what comes out

`swipekb::LanguageEngine` (`include/swipekb/language_engine.hpp`) is the one class to start from.

```cpp
#include "swipekb/language_engine.hpp"
using namespace swipekb;

LanguageEngine engine;
LanguageEngineSetup setup;
setup.language   = "en";                 // "en", "es", "fr" or "de"
setup.dictionary = "en_swipe.combined";  // required: the language's word list
setup.ngram      = "en.ngram";           // optional: sentence prior; "" = word frequency only
setup.swipeModel = "swipe-model.bin";    // optional: neural swipe model; "" = geometry only
if (!engine.init(setup)) { /* dictionary unreadable */ }

// keys: one KeyRect per letter key (letter, x, y, w, h in pixels).
// trace: finger samples (x, y in the same pixels, t_ms since the swipe started).
auto guesses = engine.recognize(trace, keys, contextWords("I think we"), 3);
// guesses[0].word is the best guess; Candidate::score is only comparable within one answer.
auto completions = engine.complete("hel");   // words that start with "hel", plus close corrections
auto alternatives = engine.otherSpellings("its");    // {"it's"}
```

- **Swipes:** `recognize()` takes the finger path, the on-screen letter-key rectangles and the previous words of the sentence (`contextWords()` builds them from the text before the cursor). It returns up to `topK` ranked words, every one of them a word in the dictionary.
- **Typed words:** `complete()` (completions and one-edit corrections), `similar()` (neighbouring words, for tap-to-swap), `otherSpellings()`, `displaySpelling()`, `hasWord()`, `isOffensive()`.
- **Your own words:** `setLearnedWords()` and `liftWords()` add words at run time; `setCapitals()` supplies the words shown with a capital.
- **Next-word suggestions** are a separate class, `swipekb::Predictor` (`prediction.hpp`): `init(ngramFile, vocabFile, nwpFile)` then `predict(contextWords, 3)`. Personal learning is `learning.hpp`.

## Behaviour you can rely on

- A missing or damaged model or n-gram file never stops the engine; that part is simply switched off (`hasModel()` and `usingNgram()` tell you which). Only an unreadable dictionary stops `init()`.
- Nothing throws to the caller. Bad input gives an empty answer.
- One instance per thread. Call every member from the thread that owns it.
- The first `recognize()` after new letters or new words rebuilds the word list once, which takes a short pause.
- A word that is not in the dictionary can never be returned by a swipe.

## Files it reads

| File | Needed? | Where to get it | Licence |
|---|---|---|---|
| Word list `en_swipe.combined`, `es.combined`, `fr.combined`, `de.combined` | yes | GitHub release `language-files-2026-10-23-apostrophes` | ODC-By 1.0 and CC BY 2.0 France; English also Apache-2.0 (Android word list) |
| N-gram file `LANG.ngram` and `LANG.vocab.tsv` | no | same release | ODC-By 1.0 and CC BY 2.0 France |
| Neural next-word model `LANG.nwp.bin` | no | GitHub release `models-2026-10-23` | ODC-By 1.0 and CC BY 2.0 France |
| Swipe model `swipe-model.bin` | no | same release | MIT (trained on FUTO's MIT swipe data plus synthetic swipes) |

Credits to keep when you redistribute, formats and accuracy: [docs/models](../docs/models/README.md). Files are pinned by SHA-256 in `../packaging/assets.lock.json`; `scripts/fetch-assets.py` downloads them and checks the hashes.

## Build and test

The library target is `swipekb_engine` (`engine/CMakeLists.txt`). It is built as part of the top-level project, which also needs GoogleTest:

```sh
cmake -S . -B build && cmake --build build --target swipekb_engine
ctest --test-dir build -R engine
```

Some tests use the real language files and need `SWIPEKB_ASSETS` to point at a folder holding them.

## Limits

Accuracy depends on the files you load; see the model cards for numbers and for what was not measured. English swipe recognition is a few points behind the FUTO engine this project used before; Spanish, French and German swipe models are trained partly on synthetic data. It is built and tested on Linux (x86_64 and aarch64) only.
