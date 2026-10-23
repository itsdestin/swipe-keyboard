# Open models of the Swipe Keyboard

Draft, not published. Models trained and released by Destin Moss via youcoded.ai, 2026.

The Swipe Keyboard is an open-source swipe-typing and word-prediction add-on for KDE Plasma's on-screen keyboard: https://github.com/itsdestin/swipe-keyboard. These are the models it uses beside its word-list files. All run on the user's own computer with our own small C++ code; nothing is sent anywhere.

| Model | What it does | Size | Licence of the file | Card |
|---|---|---|---|---|
| `nwp-en` | English next-word prediction (one-layer LSTM, int8); used together with the English n-gram pack | 4,657,632 bytes | ODC-By-1.0 AND CC-BY-2.0-FR | [nwp-en](nwp-en/README.md) |
| `nwp-es` | Spanish next-word prediction | 4,709,920 bytes | ODC-By-1.0 AND CC-BY-2.0-FR | [nwp-es](nwp-es/README.md) |
| `nwp-fr` | French next-word prediction | 4,698,696 bytes | ODC-By-1.0 AND CC-BY-2.0-FR | [nwp-fr](nwp-fr/README.md) |
| `nwp-de` | German next-word prediction | 4,718,096 bytes | ODC-By-1.0 AND CC-BY-2.0-FR | [nwp-de](nwp-de/README.md) |
| `swipe-multilingual` ("T1") | Swipe decoding for English, Spanish, French and German from key positions (small CTC network, lexicon beam search, geometric cross-check and fallback); opt-in beside FUTO's engine | 2,115,260 bytes | MIT | [swipe-multilingual](swipe-multilingual/README.md) |

<!-- src: sizes and licences: packaging/assets.lock.json trained_models -->

How everything was measured, including a measurement error we found and retracted: [methods](../methods.md).

Honest summary: the next-word models add up to about 9 points over the n-gram files alone on some measures and little on others, and lead Gboard 14.2 (learning off) on the recorded queries; the swipe model is about 3 points behind FUTO's engine on English first guess over all swipes and is ahead of FUTO's current packs on Spanish, French and German real swipes from HeliBoard, with a different dictionary. Details, caveats and sources are in the cards and the methods page.

Credit line for all of them: **Models trained and released by Destin Moss via youcoded.ai, 2026.** Release assets: GitHub release `models-2026-10-23`; word lists and n-gram files: `language-files-2026-10-22-open-words`.
