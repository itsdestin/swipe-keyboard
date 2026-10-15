# swipe-keyboard

Gboard-style swipe typing for the KDE Plasma on-screen keyboard on Destin's ROG Flow Z13.
Design: `docs/superpowers/specs/2026-10-14-swipe-typing-design.md`.

**Powered by FUTO Swipe.** Swipe recognition uses the FUTO Swipe models (FUTO Model Weights License 1.0,
shipped next to the models as `FUTO-MODEL-LICENSE.md`) and FUTO's GPL-3.0 `swipe-library`. The scoreboard
uses FUTO's MIT-licensed swipe dataset. The English dictionary (`en_swipe.combined`, built by
`scripts/dict/build-en-swipe.sh`) combines AOSP's US English word list (Apache-2.0) with word frequencies from
the [wordfreq](https://github.com/rspeer/wordfreq) package by Robyn Speer, whose data is CC-BY-SA 4.0; the
built dictionary file is therefore CC-BY-SA 4.0 too (it is built on install, not stored in this repo).
The optional word-sequence model (`assets/lm/en_ngram.bin`, built by `scripts/lm/build-lm.sh`) is computed
from [FineWeb](https://huggingface.co/datasets/HuggingFaceFW/fineweb) (sample-10BT) by Hugging Face, licensed
ODC-BY 1.0 (web pages from CommonCrawl, also subject to CommonCrawl's terms of use); it is built on install, not
stored in this repo. The optional neural word model (`assets/nlm/en_nlm.gguf`) is
[SmolLM2-135M](https://huggingface.co/HuggingFaceTB/SmolLM2-135M) by Hugging Face, Apache-2.0, in ggml-org's
8-bit GGUF conversion; it is downloaded on install, not stored in this repo, and run with
[llama.cpp](https://github.com/ggml-org/llama.cpp) (MIT, the system's `llama-cpp` package).

## Install / uninstall

    ./install.sh              # fetch assets, build, test, install to ~/.local/share/swipe-keyboard, restart keyboard
    ./install.sh --uninstall  # remove it; the keyboard goes back to plain tapping

Needs touch-profile's keyboard layout (it loads this module when present).

- The hook that loads this module is only in touch-profile's wide-space layout (`TP_KBD_WIDE_SPACE=1`,
  the default). With `TP_KBD_WIDE_SPACE=0` the keyboard never loads swipe typing.
- `--uninstall` leaves touch-profile's hook in its generated layout. That is harmless: the hook looks for
  this module and, when it's gone, the keyboard behaves exactly as before.

## Smarter guess ranking (n-gram model, off by default)

A 3-gram word model (how likely a word is after the previous two) can rank the guesses instead of FUTO's
context model. It is **off** until you create a switch file, then restart the on-screen keyboard:

    touch ~/.local/share/swipe-keyboard/use-ngram     # on  (delete the file to go back)

Measured (step 1 of the accuracy push): first guess 93.32% -> 93.42% on the scoreboard, 93.16% -> 93.67% on
10,000 other test swipes, 93.23% -> 94.12% on held-out validation swipes. Costs ~150 MB disk and up to ~130 MB
of (shared, reclaimable) memory; no measurable delay. Without the model file the keyboard ranks the old way.

## Scoreboard

    bench/score.sh --label "what changed" --check

Replays 2000 real human swipes (never trained on) and records first-guess % and top-3 % in `bench/history.tsv`.

## License

GPL-3.0 (links FUTO's GPL swipe-library). If this is ever shared, its source must be shared too.
