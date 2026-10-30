# Open models of the Swipe Keyboard

Models trained and released by Destin Moss via youcoded.ai, 2026.

Five small model files plus a tiny re-ranker built into the engine, used by the [Swipe Keyboard](https://github.com/itsdestin/swipe-keyboard), an open-source swipe-typing and word-prediction add-on for KDE Plasma's on-screen keyboard (English, Spanish, French, German). They run on the user's own computer with the project's own C++ code. Nothing is sent anywhere.

| Model | What it does | Size | Licence | Card |
|---|---|---|---|---|
| `youcoded/next-word-english` | Predicts the next English word; blended with the English n-gram file | 4.66 MB | ODC-By-1.0 AND CC-BY-2.0-FR | [card](next-word-english/README.md) |
| `youcoded/next-word-spanish` | Same, Spanish | 4.71 MB | ODC-By-1.0 AND CC-BY-2.0-FR | [card](next-word-spanish/README.md) |
| `youcoded/next-word-french` | Same, French | 4.70 MB | ODC-By-1.0 AND CC-BY-2.0-FR | [card](next-word-french/README.md) |
| `youcoded/next-word-german` | Same, German | 4.72 MB | ODC-By-1.0 AND CC-BY-2.0-FR | [card](next-word-german/README.md) |
| `youcoded/swipe-typing` ("T1") | Turns a finger path into ranked words, all four languages in one file | 2.12 MB | MIT | [card](swipe-typing/README.md) |
| Learned swipe re-ranker (built into the engine, no separate file) | Orders a swipe's 30 found words, with or without words before; one set for all languages | 2.4 KB | MIT (trained on FUTO's MIT swipe data) | [card, re-ranker section](swipe-typing/README.md#the-learned-re-ranker-built-into-the-engine) |

## Using them

- **Files:** GitHub release `models-2026-10-23` (the models) and `language-files-2026-10-23-apostrophes` (word lists and n-gram files they are used with). Every file is pinned by SHA-256 in `packaging/assets.lock.json`.
- **Code:** `engine/` (MIT). `swipekb::LanguageEngine` is the one class to start from; it takes a word list, an optional n-gram file and an optional swipe model, and falls back to simpler behaviour when a file is missing. See [`engine/README.md`](../../engine/README.md).
- **Licences:** the next-word models need attribution to Hugging Face (FineWeb), Common Crawl terms and the Tatoeba contributors (CC BY 2.0 France); no share-alike. The swipe model and the learned re-ranker built into the engine need the MIT text of FUTO's swipe data kept with them. Each card lists the exact credits.

## How good are they

- **Next word, against Gboard:** on the recorded test queries, the next-word models plus their n-gram files were ahead of Gboard on every measure, by 3.9 to 19.6 points. The test: Gboard 14.2 (the copy built into Google's Android 35 x86_64 emulator image), learning off, tested 2026-10-22, 100 sentences per set. Newer Gboard versions and Gboard with learning on were not measured, so this is not a general claim about Gboard.
- **Next word, against the n-gram files alone:** the models add up to about 9 points on some measures and under 1 point on others.
- **Swipe:** in the keyboard, English swipes are about 2 points behind the FUTO engine it replaced (91.47 % against 93.32 % first guess on the same 1,946 real swipes, previous words given). On real HeliBoard swipes with the shipped word lists, French is ahead of the FUTO keyboard of 0.4.1 (68.2 % against 64.6 %) and Spanish and German are behind (75.9 % against 77.7 %, 64.7 % against 69.7 %). Gboard swipe typing was not measured. All rows: `bench/swipe-scoreboard.tsv`.

How everything was measured, including a measurement error we found and retracted: [methods](../methods.md). Where each number comes from: [SOURCES.md](SOURCES.md).
