# swipe-keyboard

Gboard-style swipe typing for the KDE Plasma on-screen keyboard on Destin's ROG Flow Z13.
Design: `docs/superpowers/specs/2026-10-14-swipe-typing-design.md`.

**Powered by FUTO Swipe.** Swipe recognition uses the FUTO Swipe models (FUTO Model Weights License 1.0,
shipped next to the models as `FUTO-MODEL-LICENSE.md`) and FUTO's GPL-3.0 `swipe-library`. The scoreboard
uses FUTO's MIT-licensed swipe dataset. The English word list comes from AOSP (Apache-2.0).

## Install / uninstall

    ./install.sh              # fetch assets, build, test, install to ~/.local/share/swipe-keyboard, restart keyboard
    ./install.sh --uninstall  # remove it; the keyboard goes back to plain tapping

Needs touch-profile's keyboard layout (it loads this module when present).

- The hook that loads this module is only in touch-profile's wide-space layout (`TP_KBD_WIDE_SPACE=1`,
  the default). With `TP_KBD_WIDE_SPACE=0` the keyboard never loads swipe typing.
- `--uninstall` leaves touch-profile's hook in its generated layout. That is harmless: the hook looks for
  this module and, when it's gone, the keyboard behaves exactly as before.

## Scoreboard

    bench/score.sh --label "what changed" --check

Replays 2000 real human swipes (never trained on) and records first-guess % and top-3 % in `bench/history.tsv`.

## License

GPL-3.0 (links FUTO's GPL swipe-library). If this is ever shared, its source must be shared too.
