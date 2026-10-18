# Swipe Keyboard for KDE Plasma

**English swipe typing for Plasma Keyboard. Powered by FUTO Swipe.**

Swipe across the keys, choose alternative words, and undo a fresh swipe with one Backspace. Uses local recognition models and your existing keyboard's look. **No Z13, ASUS hardware, tablet bundle, theme, agent or YouCoded account required.**

[**Download v0.1.0 test release**](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.1.0) · [Report a bug](https://github.com/itsdestin/swipe-keyboard/issues/new?template=bug.yml) · [Contribute](CONTRIBUTING.md)

![Swipe Keyboard in a Fedora KDE VM, with synthetic “the world” text and word alternatives](docs/images/swipe-typing.png)

*Fedora KDE test VM; synthetic text only. The keyboard follows your system theme.*

## What it does

- English (US) swipe typing with word alternatives and normal tapping.
- One-shot whole-word Backspace immediately after a swipe or alternative choice, when exact field context is available. After typing or a reported cursor move, Backspace returns to normal. It never guesses a multi-character deletion from hidden/changed/selected or sensitive text.
- Stock Plasma layout and theme retained; no changes to panels, gestures, tablet mode, rotation or wallpaper.
- Reversible per-user activation; later manual keyboard choices are preserved on disable.
- Recognition runs locally. Recording is **off by default**.

## Supported test-release targets

| Package | Tested baseline |
|---|---|
| Arch / CachyOS, x86_64 | Plasma Keyboard 6.8.0, Qt 6.12.0 |
| **Updated** Fedora 44 KDE, x86_64 | Plasma Keyboard 6.7.5, Qt 6.11.2 |

Requires **KDE Plasma Wayland** and a touchscreen. Activation accepts Plasma Keyboard 6.7/6.8 and checks that your Qt major/minor matches the package build. Unsupported versions are refused rather than patched blindly. These first packages do not claim X11, non-KDE, ARM or non-English swipe support.

Both native package builds, extracted-keyboard loading, installation and removal were checked in isolated distro environments. A full Fedora KDE VM additionally verified real typing, word alternatives/backspace, password safeguards, reinstall, reboot and setting restoration. This is a **test release**, not a universal hardware/application guarantee. See [verification details](docs/standalone-verification.md).

## Install

1. Download the package matching your distribution from the [release page](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.1.0). Optionally download `SHA256SUMS.txt` and check the files with `sha256sum --check --ignore-missing SHA256SUMS.txt`.
2. Install the local package:

   **Arch / CachyOS**
   ```sh
   sudo pacman -U ./swipe-keyboard-0.1.0-1-x86_64.pkg.tar.zst
   ```

   **Fedora 44 KDE**
   ```sh
   sudo dnf install ./swipe-keyboard-0.1.0-1.fc44.x86_64.rpm
   ```

3. As your **normal logged-in KDE user**, not sudo:
   ```sh
   swipe-keyboard check
   swipe-keyboard enable
   ```
4. Select **English (US)** in Plasma Keyboard. Touch a supported text field and swipe a word.

The package includes compiled code, models and a dictionary. You do not need a compiler, a Python environment setup or a training-corpus download. A fresh Fedora release ISO has an older KDE/Qt baseline: update Fedora normally first. This keyboard is not a desktop-upgrade tool.

**Already using custom Plasma keyboard layouts?** Setup refuses to overwrite `~/.local/share/plasma/keyboard/layouts` (or its XDG equivalent). Migrate that setup explicitly first; do not stack this over touch-profile's existing keyboard overlay.

## Updates and removal

Install a newer **matching native package** normally. At keyboard startup the private English overlay is rebuilt from the installed stock layouts. Unknown layout formats or incompatible versions fail closed, with a stock-keyboard fallback where possible.

Before removing the package, run as your normal KDE user:

```sh
swipe-keyboard disable
```

Then remove it with `sudo pacman -R swipe-keyboard` or `sudo dnf remove swipe-keyboard`. Original settings still owned by setup are restored; later manual choices are preserved. Personal opt-in recordings/options are not silently deleted.

There is **no public AUR/COPR package repository or automatic update feed yet**; GitHub releases are the download source.

## Known v1 limits

- **Firefox physical Ctrl+A/Delete:** the browser/input-method chain can leave field context stale; the next swipe may insert an unwanted leading space. This is a documented minor v1 issue, not a solved upstream bug.
- Apps that hide or misreport text cannot guarantee automatic word deletion or candidate replacement. Normal deletion is the conservative fallback, but an edit never reported by an app cannot be detected reliably.
- Application Wayland text-input support affects when the keyboard appears. Not every application's keyboard-summoning behavior is fixed by this extension.
- English swipe recognition only. The base release excludes optional neural/n-gram ranking and training/evaluation data.

[Open a bug report](https://github.com/itsdestin/swipe-keyboard/issues/new?template=bug.yml) with the package version, distribution, Plasma Keyboard/Qt versions, keyboard layout, affected app and a synthetic reproduction phrase. **Do not attach typing recordings, learned words, credentials or unredacted logs.**

## Privacy

Package code/models live under `/usr/share/swipe-keyboard/`. Per-user state and private overlays live under `$XDG_DATA_HOME/swipe-keyboard/`, normally `~/.local/share/swipe-keyboard/`.

Recording requires a user-created `record` switch file and can contain typed words/context; it is intended for explicit local development, not support-log collection. Recordings are owner-only, and password/sensitive-field hints are checked. No recordings are included in release files.

## Development and source

See [CONTRIBUTING.md](CONTRIBUTING.md). Code, engine benchmarks, tests and native package recipes are in this repository. The legacy `./install.sh` is retained for the original touch-profile **developer** setup; it is not the standalone end-user installer.

For complete corresponding source, use the release's **`swipe-keyboard-0.1.0.tar.gz`**, which includes the recursive upstream dependencies. GitHub's automatic “Source code” ZIP/tar links do **not** include Git submodules. Alternatively clone with `git clone --recurse-submodules https://github.com/itsdestin/swipe-keyboard.git`.

The checked-in scoreboard records around 93.32% first-guess accuracy on its 2,000-swipe benchmark, not a guarantee about anyone's typing. Compare changes on the same held-out dataset/configuration rather than treating different benchmarks as equivalent.

## Credits and licenses

**Powered by FUTO Swipe.** [FUTO's swipe-library](https://gitlab.futo.org/keyboard/swipe-library) is GPL-3.0; the model weights have the separate **FUTO Model Weights License 1.0**, distributed alongside them. Preserve the upstream notices and provide corresponding source with GPL binaries. The benchmark dataset is MIT-licensed.

The English dictionary combines AOSP US English word lists (Apache-2.0) and Robyn Speer's [wordfreq](https://github.com/rspeer/wordfreq) frequency data (CC-BY-SA 4.0). The generated dictionary is CC-BY-SA 4.0; attribution/source/checksum details are in [NOTICE](packaging/NOTICE) and the installed licenses. Stock Qt/KDE layouts retain their notices. Optional development models have their own licenses and are not part of the base release merely because the code is GPL.

This project is not endorsed by FUTO, KDE, Google or ASUS. It was built and refined with [YouCoded](https://youcoded.ai); the maintainer also makes YouCoded. Using or contributing to this keyboard does not require it.
