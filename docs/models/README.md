# Open models of the Swipe Keyboard

Draft, not published. Models trained and released by Destin Moss via youcoded.ai, 2026.

The Swipe Keyboard is an open-source swipe-typing and word-prediction add-on for KDE Plasma's on-screen keyboard: https://github.com/itsdestin/swipe-keyboard. These are the models it uses beside its word-list files. All run on the user's own computer with our own small C++ code; nothing is sent anywhere.

| Model | What it does | Size | Licence of the file | Card |
|---|---|---|---|---|
| `youcoded/next-word-english` | English next-word prediction (one-layer LSTM, int8); used together with the English n-gram pack | 4,657,632 bytes | ODC-By-1.0 AND CC-BY-2.0-FR | [next-word-english](next-word-english/README.md) |
| `youcoded/next-word-spanish` | Spanish next-word prediction | 4,709,920 bytes | ODC-By-1.0 AND CC-BY-2.0-FR | [next-word-spanish](next-word-spanish/README.md) |
| `youcoded/next-word-french` | French next-word prediction | 4,698,696 bytes | ODC-By-1.0 AND CC-BY-2.0-FR | [next-word-french](next-word-french/README.md) |
| `youcoded/next-word-german` | German next-word prediction | 4,718,096 bytes | ODC-By-1.0 AND CC-BY-2.0-FR | [next-word-german](next-word-german/README.md) |
| `youcoded/swipe-typing` ("T1") | Swipe decoding for English, Spanish, French and German from key positions (small CTC network, lexicon beam search, geometric cross-check and fallback); opt-in beside FUTO's engine | 2,115,260 bytes | MIT | [swipe-typing](swipe-typing/README.md) |

How everything was measured, including a measurement error we found and retracted: [methods](../methods.md).

Honest summary: the next-word models add up to about 9 points over the n-gram files alone on some measures and little on others, and, on the recorded queries, are ahead of Gboard 14.2 (the copy built into Google's Android 35 x86_64 emulator image, learning off, tested 2026-10-22) on every measure. Newer Gboard versions may differ, Gboard's behaviour with learning on was not measured, and the repository records no fuller Gboard build number than 14.2. The swipe model is about 3 points behind FUTO's engine on English first guess over all swipes and is ahead of FUTO's current packs on Spanish, French and German real swipes from HeliBoard, with a different dictionary. Details, caveats and sources are in the cards and the methods page.

Credit line for all of them: **Models trained and released by Destin Moss via youcoded.ai, 2026.** Release assets: GitHub release `models-2026-10-23`; word lists and n-gram files: `language-files-2026-10-22-open-words`.
