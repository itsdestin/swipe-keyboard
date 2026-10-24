# Open models of the Swipe Keyboard

Models trained and released by Destin Moss via youcoded.ai, 2026.

Five small models, used by the [Swipe Keyboard](https://github.com/itsdestin/swipe-keyboard), an open-source swipe-typing and word-prediction add-on for KDE Plasma's on-screen keyboard (English, Spanish, French, German). They run on the user's own computer with the project's own C++ code. Nothing is sent anywhere.

| Model | What it does | Size | Licence | Card |
|---|---|---|---|---|
| `youcoded/next-word-english` | Predicts the next English word; blended with the English n-gram file | 4.66 MB | ODC-By-1.0 AND CC-BY-2.0-FR | [card](next-word-english/README.md) |
| `youcoded/next-word-spanish` | Same, Spanish | 4.71 MB | ODC-By-1.0 AND CC-BY-2.0-FR | [card](next-word-spanish/README.md) |
| `youcoded/next-word-french` | Same, French | 4.70 MB | ODC-By-1.0 AND CC-BY-2.0-FR | [card](next-word-french/README.md) |
| `youcoded/next-word-german` | Same, German | 4.72 MB | ODC-By-1.0 AND CC-BY-2.0-FR | [card](next-word-german/README.md) |
| `youcoded/swipe-typing` ("T1") | Turns a finger path into ranked words, all four languages in one file | 2.12 MB | MIT | [card](swipe-typing/README.md) |

## Using them

- **Files:** GitHub release `models-2026-10-23` (the models) and `language-files-2026-10-23-apostrophes` (word lists and n-gram files they are used with). Every file is pinned by SHA-256 in `packaging/assets.lock.json`.
- **Code:** `engine/` (MIT). `swipekb::LanguageEngine` is the one class to start from; it takes a word list, an optional n-gram file and an optional swipe model, and falls back to simpler behaviour when a file is missing. See [`engine/README.md`](../../engine/README.md).
- **Licences:** the next-word models need attribution to Hugging Face (FineWeb), Common Crawl terms and the Tatoeba contributors (CC BY 2.0 France); no share-alike. The swipe model needs the MIT text of FUTO's swipe data kept with it. Each card lists the exact credits.

## How good are they

- **Next word, against Gboard:** on the recorded test queries, the next-word models plus their n-gram files were ahead of Gboard on every measure, by 3.9 to 19.6 points. The test: Gboard 14.2 (the copy built into Google's Android 35 x86_64 emulator image), learning off, tested 2026-10-22, 100 sentences per set. Newer Gboard versions and Gboard with learning on were not measured, so this is not a general claim about Gboard.
- **Next word, against the n-gram files alone:** the models add up to about 9 points on some measures and under 1 point on others.
- **Swipe:** on English the swipe model is about 3 points behind the FUTO engine it replaced (90.40 % against 93.32 % first guess, 2,000 real swipes). It is ahead of that engine's older word packs on Spanish, French and German real swipes (HeliBoard), with a different word list, so that is not like-for-like. Gboard swipe typing was not measured.

How everything was measured, including a measurement error we found and retracted: [methods](../methods.md). Where each number comes from: [SOURCES.md](SOURCES.md).
