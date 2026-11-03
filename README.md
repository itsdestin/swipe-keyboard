# OpenSwipe for KDE Plasma

Swipe across the letters to type a word, or tap as usual. Test release **v0.6.0-1**. Formerly **Swipe Keyboard**: same keyboard, new name (the package and command are still called `swipe-keyboard`).

<!-- WHY (Destin, 2026-11-02): versions before 0.5.1-1 cannot find updates after the move to the new name (GitHub does not
     forward the update website), and 0.4.0 and older never updated themselves. Keep this line for the next few releases. -->
> **Installed before 30 October 2026 (older than version 0.5.1-1)?** Install it once more from the instructions below. Older versions can't find updates after the move to the new name. After that it updates itself.

## What you get

- Swipe typing, word suggestions and spelling help on KDE Plasma's on-screen keyboard.
- English (US/UK), Spanish, French and German.
- Works offline. Nothing you type leaves your device, and learning from your typing is off unless you turn it on.
- Updates itself.

**Works on:** Steam Deck and Bazzite (Desktop Mode and Gaming Mode), Arch/CachyOS, Fedora 44 including Plasma Mobile (ARM64), Kubuntu 26.10, Debian testing and openSUSE Tumbleweed. Older KDE releases (Kubuntu 26.04, Debian 13, openSUSE Leap 16) use the Flatpak. Needs KDE Plasma on Wayland: Plasma Keyboard 6.7 or newer for the regular packages, Plasma 6.3 or newer for the Flatpak.

**Honest limits:** English, Spanish and German swipes are a few points less accurate than the FUTO engine used up to version 0.4.1 ([numbers](#how-good-is-it)). Spanish, French and German next-word suggestions are early baselines. Plasma 6.9 has not been tried on a real device; if something does not fit, KDE's plain keyboard starts and typing still works.

## Install

[Steam Deck / Bazzite](#steam-deck--bazzite) · [Arch / CachyOS](#arch--cachyos) · [Fedora 44 / Plasma Mobile](#fedora-44--plasma-mobile) · [Kubuntu / Debian](#kubuntu-2610--debian-testing) · [openSUSE Tumbleweed](#opensuse-tumbleweed) · [Older KDE (Kubuntu 26.04, Debian 13, Leap 16)](#older-kde-kubuntu-2604-debian-13-opensuse-leap-16)

### Steam Deck / Bazzite

**[Download for Steam Deck](https://github.com/itsdestin/openswipe/releases/download/v0.6.0-1/swipe-keyboard-0.6.0-1-steamos-x86_64.tar.gz)**, one file for Desktop Mode and Gaming Mode.

1. In **Desktop Mode**, save it to **Downloads**, open **Konsole** and run this **without `sudo`**:

   ```sh
   cd ~/Downloads && tar xzf swipe-keyboard-0.6.0-1-steamos-x86_64.tar.gz && cd swipe-keyboard-steamos-0.6.0-1
   bash install.sh
   ```

2. It shows everything it will change, then asks **Also swipe in Gaming Mode? [Y/n]**. Press **Enter** for yes. It asks for your Deck's administrator password once, to keep other devices on your network away from Steam's developer switch. Never set one? Run `passwd` in Konsole first.
3. Open **System Settings → Virtual Keyboard**, select **OpenSwipe** and apply (missing? log out and back into Desktop Mode). Restart Steam once. Then touch any text field, in either mode, and swipe.

Gaming Mode uses a hidden Steam developer switch that Valve hasn't approved and could break ([details](docs/steam-deck.md)). It is new: [tell us if anything looks wrong](https://github.com/itsdestin/openswipe/issues). Do not unlock SteamOS or install the Arch/Fedora packages on a Deck.

**Languages:** run this, enable yours, then switch with the globe key.

```sh
flatpak run --user --command=kcmshell6 org.itsdestin.SwipeKeyboard kcm_plasmakeyboard
```

**Updates:** automatic, once a day, signed. Turn off with `bash install.sh --no-auto-update` (`--auto-update` turns it back on). On version 0.4.0 or older, install this download once; after that it stays current.

**Remove:** put your old keyboard back in **System Settings → Virtual Keyboard**, then run `bash uninstall.sh --remove-apps` from the download folder. It removes Gaming Mode too. To turn Gaming Mode on or off, run `bash install.sh` again.

### Arch / CachyOS

**[Download the package](https://github.com/itsdestin/openswipe/releases/download/v0.6.0-1/swipe-keyboard-0.6.0-1-x86_64.pkg.tar.zst)**, then in Konsole:

```sh
sudo pacman -U ~/Downloads/swipe-keyboard-0.6.0-1-x86_64.pkg.tar.zst
```

Then [turn it on](#turn-it-on).

**Remove:** `swipe-keyboard disable && sudo pacman -R swipe-keyboard`. **Updates:** automatic; `sudo swipe-keyboard updates off` stops them.

### Fedora 44 / Plasma Mobile

Pick your device (unsure? run `uname -m`), download its file, then run its line in Konsole:

**Intel / AMD (x86_64):** [download](https://github.com/itsdestin/openswipe/releases/download/v0.6.0-1/swipe-keyboard-0.6.0-1.fc44.x86_64.rpm)

```sh
sudo dnf install ~/Downloads/swipe-keyboard-0.6.0-1.fc44.x86_64.rpm
```

**ARM64 (aarch64):** [download](https://github.com/itsdestin/openswipe/releases/download/v0.6.0-1/swipe-keyboard-0.6.0-1.fc44.aarch64.rpm)

```sh
sudo dnf install ~/Downloads/swipe-keyboard-0.6.0-1.fc44.aarch64.rpm
```

Then [turn it on](#turn-it-on).

**Remove:** `swipe-keyboard disable && sudo dnf remove swipe-keyboard`. **Updates:** automatic; `sudo swipe-keyboard updates off` stops them.

### Kubuntu 26.10 / Debian testing

Pick your system, download its file, then run its line in Konsole:

**Kubuntu 26.10:** [download](https://github.com/itsdestin/openswipe/releases/download/v0.6.0-1/swipe-keyboard_0.6.0-1.kubuntu26.10_amd64.deb)

```sh
sudo apt install ~/Downloads/swipe-keyboard_0.6.0-1.kubuntu26.10_amd64.deb
```

**Debian testing:** [download](https://github.com/itsdestin/openswipe/releases/download/v0.6.0-1/swipe-keyboard_0.6.0-1.debiantesting_amd64.deb)

```sh
sudo apt install ~/Downloads/swipe-keyboard_0.6.0-1.debiantesting_amd64.deb
```

Then [turn it on](#turn-it-on). **Remove:** `swipe-keyboard disable && sudo apt remove swipe-keyboard`. **Updates:** automatic, once a day, only this package (they never change the rest of your system); `sudo swipe-keyboard updates off` stops them.

### openSUSE Tumbleweed

**[Download the package](https://github.com/itsdestin/openswipe/releases/download/v0.6.0-1/swipe-keyboard-0.6.0-1.tumbleweed.x86_64.rpm)**, then in Konsole:

```sh
sudo zypper install --allow-unsigned-rpm ~/Downloads/swipe-keyboard-0.6.0-1.tumbleweed.x86_64.rpm
```

`--allow-unsigned-rpm` because the file itself is not signed; automatic updates check every new version against a signed list instead. Then [turn it on](#turn-it-on). **Remove:** `swipe-keyboard disable && sudo zypper remove swipe-keyboard`. **Updates:** automatic; `sudo swipe-keyboard updates off` stops them.

### Older KDE (Kubuntu 26.04, Debian 13, openSUSE Leap 16)

Their KDE is too old for the regular packages, so they use the Flatpak: the same download as the Steam Deck, without Gaming Mode.

1. Kubuntu and Debian come without Flatpak. Once: `sudo apt install flatpak`, then **log out and back in**.
2. Download the **[Steam Deck file](https://github.com/itsdestin/openswipe/releases/download/v0.6.0-1/swipe-keyboard-0.6.0-1-steamos-x86_64.tar.gz)** to **Downloads** and run, **without `sudo`**:

   ```sh
   cd ~/Downloads && tar xzf swipe-keyboard-0.6.0-1-steamos-x86_64.tar.gz && cd swipe-keyboard-steamos-0.6.0-1
   bash install.sh
   ```

   The first time it downloads KDE's app runtime from Flathub (about 700 MB).
3. **System Settings → Keyboard → Virtual Keyboard**, select **OpenSwipe** and apply.

Tested on Kubuntu 26.04 and Debian 13 (virtual machines); Leap 16 not yet. **Remove:** `bash uninstall.sh --remove-apps` from the download folder; your system's own on-screen keyboard comes back by itself. **Updates:** automatic, like on the Deck.

### Turn it on

Run **without `sudo`**, read the changes it proposes, and answer **y**:

```sh
swipe-keyboard enable
```

Then touch a text field. Trouble removing? See the [recovery guide](docs/install-details.md#removed-the-package-before-disabling).

On version 0.4.0 or older? Those did not update themselves: install the package once as above.

## Using the keyboard

- **Swipe:** drag across the letters and lift. Tap an alternative above the keys to change the word. A swipe starts on a letter key.
- **Tap typing:** suggestions finish or correct the word. Space never autocorrects.
- **Next word:** press Space after a word, then tap a suggestion.
- **Languages:** enable them in **On-Screen Keyboard** settings (Deck: the command above), then use the globe key.
- **Move the cursor:** slide along the Space bar.
- **Learning** (off by default): remembers your names and slang on this device only. `swipe-keyboard learning on`, `off` or `forget`. [Details](docs/install-details.md#learning-from-your-typing-off-by-default)

## How good is it

- **Suggestions:** ahead of Gboard in all four languages by about 4 to 20 points. Test: **Gboard 14.2, Android 35 emulator, learning off, tested 2026-10-22**, 100 sentences per set. This says nothing general about other Gboard versions.
- **English swipes:** first guess right 91.5 % (FUTO engine, up to 0.4.1: 93.3 %). Spanish 75.9 %, French 68.2 %, German 64.7 % (FUTO 0.4.1: 77.7 %, 64.6 %, 69.7 %). Not compared with Gboard.
- Small test sets: differences under 1 to 2 points are noise. How we measured, and a mistake we found and retracted: [docs/methods.md](docs/methods.md). Per-model tables: [docs/models](docs/models/README.md).

## Help

- **Keyboard not showing?** Touch the field rather than clicking it. On Arch/Fedora run `swipe-keyboard check`.
- **No next-word suggestions?** Pick a supported language, type a word with this keyboard, then press Space. Password fields offer none.
- **Only one app misbehaves?** Try another KDE Wayland app; support varies.

[More help, recovery and privacy](docs/install-details.md) · [Steam Deck details](docs/steam-deck.md) · [Report a bug](https://github.com/itsdestin/openswipe/issues/new?template=bug.yml) · [Release notes](docs/releases/v0.6.0-1.md) · [Changelog](CHANGELOG.md)

In bug reports, use made-up text. **Do not send passwords, recordings or personal typing.**

## For developers

The engine is a separate C++ library (`engine/`, MIT, needs Qt 6 Core): give it a finger path and key rectangles, get ranked words. See [engine/README.md](engine/README.md). Trained models and word lists are described in [docs/models](docs/models/README.md) and on [Hugging Face](https://huggingface.co/youcoded). To contribute, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Licences

| Part | Licence |
|---|---|
| Engine, tools, launcher, Gaming Mode, shared language rules, build files | MIT |
| The two keyboard add-on layers that run inside GPL software: `plugin/` (Plasma 6.7/6.8) and `frontend-plasma69/` (Plasma 6.9) | GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL (KDE's standard) |
| Patches to KDE's own code (`scripts/patches/`) | KDE's licence |
| Next-word models and word lists (Hugging Face FineWeb text, Tatoeba sentences) | ODC-By 1.0 and CC BY 2.0 France: attribution only |
| Swipe model | MIT (trained on FUTO's MIT swipe data plus synthetic swipes) |
| English dictionary base: Android's word list | Apache-2.0 |
| Swear-word lists | CC BY 4.0 |
| Texting-word lists, trivial data | CC0 |
| Documentation | CC BY 4.0 |

Models were trained and released by Destin Moss via youcoded.ai, 2026. No FUTO code or models are in the keyboard. Each file names its licence (SPDX header or `REUSE.toml`; `reuse lint` passes); the texts are in `LICENSES/`, summarised in [LICENSE](LICENSE) and [packaging/NOTICE](packaging/NOTICE).
