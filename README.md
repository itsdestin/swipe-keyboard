# Swipe Keyboard for KDE Plasma

Swipe across the letters to type a word, or tap normally. It adds swipe typing, word suggestions and spelling help to KDE Plasma's on-screen keyboard, in **English (US/UK), Spanish, French and German**. It runs entirely on your own device, and updates itself.

- **Works on:** KDE Plasma on Wayland (Plasma Keyboard 6.7 or newer): Arch/CachyOS, Fedora 44 (including Plasma Mobile on ARM64) and Steam Deck Desktop Mode. Steam Deck Gaming Mode is a preview.
- **Private:** nothing is sent anywhere. Learning from your typing is off unless you turn it on, and recording is off.
- **Open:** the recognition engine and the models are open source and reusable; see [For developers](#for-developers).
- **Honest limits:** on English, Spanish and German swipes it is a few points less accurate than the FUTO engine used up to version 0.4.1 (see [How good is it](#how-good-is-it)). Spanish, French and German next-word suggestions are early baselines. Plasma 6.9 support has not yet been tried on a real device.

**Test release v0.5.0-1**

> **0.5.0 is not released yet** (this branch prepares it), so the download links below start working when it is.
> The current release is [v0.4.1-1](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.4.1-1). Remove this note when publishing.

Choose your platform: [Steam Deck](#steam-deck--desktop-mode) · [Gaming Mode (preview)](#steam-deck--gaming-mode-preview) · [Arch / CachyOS](#arch--cachyos) · [Fedora 44 / Plasma Mobile](#fedora-44--plasma-mobile)

## Install

### Steam Deck — Desktop Mode

**[Download the Steam Deck file](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.5.0-1/swipe-keyboard-0.5.0-1-steamos-x86_64.tar.gz)**

One download for both Desktop Mode and the optional [Gaming Mode preview](#steam-deck--gaming-mode-preview). Do not unlock SteamOS or install the Arch/Fedora packages on your Deck.

1. Switch to **Desktop Mode**. Save the download to **Downloads**, then open **Konsole** (the terminal app) from the application menu and run, **without `sudo`**:

   ```sh
   cd ~/Downloads && tar xzf swipe-keyboard-0.5.0-1-steamos-x86_64.tar.gz && cd swipe-keyboard-steamos-0.5.0-1
   bash install.sh
   ```

   At the first question, press **Enter** for the Desktop Mode keyboard only (choose 2 only if you also want the Gaming Mode preview). It shows every change before making it. Allow Flatpak to download any required supporting components.

2. Open the setup guide and read the changes before continuing:

   ```sh
   flatpak run --user --command=swipe-keyboard-setup org.itsdestin.SwipeKeyboard
   ```

   You may notice a different keyboard appearance, settings and suggestions. Read the explanation and confirm if you want to continue. You activate the keyboard yourself in Settings in the next step.

3. In **System Settings → Virtual Keyboard**, note your current selection (including **None**), select **Swipe Keyboard**, and apply.
4. **Touch** a text field to open the keyboard. If Swipe Keyboard is missing from Settings, log out and back into Desktop Mode.

**Languages:** open the bundled settings, enable your languages, then use the keyboard's globe key to switch:

```sh
flatpak run --user --command=kcmshell6 org.itsdestin.SwipeKeyboard kcm_plasmakeyboard
```

**Updates are automatic.** Once a day the Deck checks for a newer Swipe Keyboard and installs it by itself; the new version is used from your next login (in Gaming Mode, straight away). Only updates signed with the project's key are accepted. To turn this off, run the installer again with `bash install.sh --no-auto-update` (and `--auto-update` to turn it back on). **On version 0.4.0 or older?** Those did not update themselves: install this download once as above, and after that it stays current.

**Remove:** first restore your previous selection in **System Settings → Virtual Keyboard**, then run `bash uninstall.sh --remove-apps` from the download folder. It removes the keyboard app, its daily update check and anything Gaming Mode added.

<details><summary>Only the Desktop Mode app, without the installer (no automatic updates)</summary>

[Download the Flatpak](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.5.0-1/swipe-keyboard-0.5.0-1-x86_64.flatpak), then in Konsole: `flatpak install --user ~/Downloads/swipe-keyboard-0.5.0-1-x86_64.flatpak`, and continue at step 2. To update, repeat with a newer file. Remove with `flatpak uninstall --user org.itsdestin.SwipeKeyboard`.

</details>

### Steam Deck — Gaming Mode (preview)

**[Download the SteamOS file](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.5.0-1/swipe-keyboard-0.5.0-1-steamos-x86_64.tar.gz)** (the same file as the Desktop Mode installer above; it also contains the Desktop Mode keyboard)

**Preview: tested only on a virtual Steam Deck, not yet on a real one.** It adds swipe typing and a row of word suggestions on top of Steam's own on-screen keyboard, which keeps working underneath. If something goes wrong, `bash uninstall.sh` removes everything. Please [report problems](https://github.com/itsdestin/swipe-keyboard/issues), especially jumbled text.

**Read this first.** Gaming Mode works through a developer switch that Valve provides in Steam. Steam's Subscriber Agreement says you may not modify software accessed through Steam without Valve's consent. Whether this counts is unclear. We change no Steam file and do not touch games or anti-cheat, and a popular community tool (Decky Loader) uses the same switch, but Valve has not approved this and could stop it. With the switch on, other programs on your Deck can control Steam's screens, and, unless you block it in question 2, so can other devices on your Wi-Fi or home network (SteamOS opens port 8081 to the whole network). We are not lawyers; use it only if you are comfortable with that.

1. In **Desktop Mode**, save the download to **Downloads**, open **Konsole** and run, **without `sudo`**:

   ```sh
   cd ~/Downloads && tar xzf swipe-keyboard-0.5.0-1-steamos-x86_64.tar.gz && cd swipe-keyboard-steamos-0.5.0-1
   bash install.sh
   ```

   It asks two plain-language questions and lists every change before making it.
2. Restart Steam once if it was open (the installer never restarts Steam during a game).
3. Switch to Gaming Mode, touch a text field and swipe across the letters.

**Question 1: do you want Gaming Mode at all?** Pressing Enter picks the safe answer: the Desktop Mode keyboard only. Nothing about Steam or your network changes and no password is asked. Choosing Gaming Mode turns on the Valve developer switch and needs one Steam restart.

**Question 2 (only if you chose Gaming Mode): who else may reach Steam's developer switch?** The default blocks other devices: it asks for your Deck's administrator password once (if you never set one, run `passwd` in Konsole first), changes one small file in `/etc/systemd/system`, and stops network tools that use the switch from another computer. The alternative leaves it open, which needs no password but lets anyone on the same network control Steam on your Deck: never on public or shared Wi-Fi. If a block fails, the installer asks again; it never turns Gaming Mode on with the door open unless you say so.

**Passwords.** Inside Steam's own password boxes the keyboard layer does nothing. Over games and other programs Steam does not say whether a box is a password box, so nothing is learned or saved there, and suggestions are dropped when the keyboard closes.

**Swipes start on a letter key.** A finger that rests on a letter for a moment before moving can type that one letter. Backspace, Shift, Space and Enter are unchanged.

**If the developer switch is turned off** (by you, another tool or Steam), Gaming Mode swipe stops and Steam's normal keyboard is used. We never put the file back on our own; the next time you are in Desktop Mode a notification offers **Turn it back on** or **Dismiss**. If Steam changes something we depend on, Gaming Mode switches itself off.

**Change your mind:** run `bash install.sh` again to add Gaming Mode, remove it, or switch between blocked and open. `bash uninstall.sh` removes only what the installer added. Full details, status messages and the exact files: [Gaming Mode details](docs/install-details.md#gaming-mode-on-steam-deck-preview-first-released-in-v041-1).

### Arch / CachyOS

**[Download the x86_64 package](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.5.0-1/swipe-keyboard-0.5.0-1-x86_64.pkg.tar.zst)**

Requires KDE Plasma Wayland with Plasma Keyboard 6.7 or newer. One package carries a keyboard for 6.7/6.8 and one for Plasma 6.9's rebuilt keyboard and starts the matching one. The 6.9 version is new and has only been tested against KDE's development code, not yet on a real Plasma 6.9 device. If anything does not fit, KDE's plain keyboard starts and typing keeps working. The package never blocks a Plasma upgrade.

1. Save the download to **Downloads**, then open **Konsole** from the application menu and paste:

   ```sh
   sudo pacman -U ~/Downloads/swipe-keyboard-0.5.0-1-x86_64.pkg.tar.zst
   ```

2. Enable the keyboard **without `sudo`**:

   ```sh
   swipe-keyboard enable
   ```

   This checks compatibility and asks for confirmation before changing your keyboard selection. Read the proposed changes and answer **y** to continue. Your existing show/hide settings are preserved. Suggestions and Backspace behavior may differ from your previous keyboard. If setup reports an error, stop and follow its message.
3. **Touch** a text field to open the keyboard.

**Updates are automatic.** Once a day (starting after the next restart) your computer checks for a newer Swipe Keyboard and installs it with `pacman`; only this one package is changed, and only a version signed with the project's key is accepted. The new version is used from your next login. Turn it off with `sudo swipe-keyboard updates off` (and `on` to turn it back on). **On version 0.4.0 or older?** Those did not update themselves: install this package once as above.

**Remove:** restore your previous keyboard and then remove the package:

```sh
swipe-keyboard disable && sudo pacman -R swipe-keyboard
```

If restoring the keyboard fails, removal stops. Follow the error or use the [recovery guide](docs/install-details.md#removed-the-package-before-disabling).

### Fedora 44 / Plasma Mobile

For KDE Plasma or Plasma Mobile with Plasma Keyboard 6.7 or newer (see the Plasma 6.9 note under Arch). Choose the file matching your device:

| Device | Download |
|---|---|
| Intel / AMD — x86_64 | [Fedora x86_64 RPM](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.5.0-1/swipe-keyboard-0.5.0-1.fc44.x86_64.rpm) |
| ARM64 — aarch64 | [Fedora ARM64 RPM](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.5.0-1/swipe-keyboard-0.5.0-1.fc44.aarch64.rpm) |

Unsure which? `uname -m` in a terminal reports `x86_64` or `aarch64`.

1. Save the download to **Downloads**, then open **Konsole** from the application menu. Paste **only the matching install command**:

   **Intel / AMD:**
   ```sh
   sudo dnf install ~/Downloads/swipe-keyboard-0.5.0-1.fc44.x86_64.rpm
   ```

   **ARM64:**
   ```sh
   sudo dnf install ~/Downloads/swipe-keyboard-0.5.0-1.fc44.aarch64.rpm
   ```

2. Enable the keyboard **without `sudo`**:

   ```sh
   swipe-keyboard enable
   ```

   This checks compatibility and asks for confirmation before changing your keyboard selection. Read the proposed changes and answer **y** to continue. Your existing show/hide settings are preserved. Suggestions and Backspace behavior may differ from your previous keyboard. If setup reports an error, stop and follow its message.
3. **Touch** a text field to open the keyboard.

**Updates are automatic.** The package adds the project's signed update source to dnf, and once a day (starting after the next restart) it installs a newer Swipe Keyboard by itself; Discover's Updates page also shows it. Only this one package is changed. The new version is used from your next login. Turn automatic installing off with `sudo swipe-keyboard updates off` (updates then still appear in Discover). **On version 0.4.0 or older?** Those did not update themselves: install this package once as above.

**Remove:** restore your previous keyboard and then remove the package:

```sh
swipe-keyboard disable && sudo dnf remove swipe-keyboard
```

If restoring the keyboard fails, removal stops. Follow the error or use the [recovery guide](docs/install-details.md#removed-the-package-before-disabling).

## Using the keyboard

- **Languages:** enable them in **On-Screen Keyboard** settings, then switch with the globe key. On a Deck, use the settings command in its install section above. Maximise the settings window if the list is cut off.
- **Swipe:** drag across the letters and lift to enter a word. Tap an alternative above the keys to change it.
- **Tap typing:** suggestions finish or correct the word you are typing. The strongest is in the middle and your exact spelling stays available. Only tapping a suggestion changes the word; Space never autocorrects.
- **Next-word suggestions:** after a word, press Space; tap a suggestion to insert it. Spanish, French and German suggestions are an early baseline and can be repetitive or less natural.
- **Apostrophes and capitals:** the sentence decides "were" or "we're", "its" or "it's". "I", names, places, days and months come out capitalised in English, Spanish and French.
- **Texting words:** lol, wyd, rn, jaja, mdr, lg and similar can be swiped and completed. Abbreviations of swear words are never suggested before you type them.
- **Learning (off until you turn it on):** remembers your names, slang and favourite word pairs on this computer only, never from password, e-mail, web-address or number fields. `swipe-keyboard learning on` / `off`; `swipe-keyboard learning forget` erases it; or press and hold a learned suggestion and choose **Remove from dictionary**. [Details](docs/install-details.md#learning-from-your-typing-off-by-default)
- **Move the cursor:** slide along the Space bar. Slow slides move one character at a time and fast slides go further. A normal tap still types a space. It works on the letters page, including in password fields, not on `?123`.
- **Backspace:** the first press can remove the whole swiped word. Moving the cursor or changing fields clears suggestions.

## How good is it

- **Suggestions (next word and completion).** On our recorded test queries in all four languages, the suggestions were ahead of Gboard on every measure, by about 4 to 20 points. The test: **Gboard 14.2, Android 35 emulator, learning off, tested 2026-10-22**, 100 sentences per set. Newer Gboard versions and Gboard after it has learned from a user were not measured, so this says nothing general about Gboard.
- **Swipe typing, English.** On 1,946 real swipes typed inside sentences (the keyboard knows the earlier words), the first guess is right 91.5 % of the time and the right word is in the top 3 for 95.0 %. The FUTO engine used up to version 0.4.1 scored 93.3 % and 96.6 % on the same swipes, measured the same way. For the first word of a sentence ours is right 89.5 % of the time. Measured on the final 0.5.0 test build, 2026-10-24 ([all rows](bench/swipe-scoreboard.tsv)). Swipe typing was not compared with Gboard.
- **Swipe typing, Spanish, French, German.** First guess right 75.9 % (Spanish), 68.2 % (French) and 64.7 % (German) on about 5,000 real swipes each from another project's test data, with no sentence around them. On the same swipes the FUTO engine of version 0.4.1 scored 77.7 %, 64.6 % and 69.7 %: French is better now, Spanish and German worse. Their training data is partly synthetic, and the German word list lacks about one in five of the test words.
- Test sets are small, so differences under about 1 to 2 points are noise. How everything was measured, and one measurement error we found and retracted: [docs/methods.md](docs/methods.md). Per-model tables: [docs/models](docs/models/README.md).

## For developers

The engine behind the keyboard is a separate C++ library (`engine/`, MIT) with one entry point, `swipekb::LanguageEngine`: give it the finger path and the key rectangles and it returns ranked words; it also does completion, spelling alternatives and next-word suggestions. It needs Qt 6 Core. See [engine/README.md](engine/README.md) for what it takes and returns, the limits, and how to build it. The trained models (four small next-word models and one swipe model) and word lists are published as GitHub releases and described, with licences and accuracy, in [docs/models](docs/models/README.md) and on [Hugging Face](https://huggingface.co/youcoded). To contribute, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Help

- **Keyboard not appearing?** Touch the field instead of clicking it. Native-package users can run `swipe-keyboard check`.
- **No next-word suggestions?** Select a supported language, type a word with this keyboard, then press Space. Predictions start from words typed on this keyboard, not pasted text. Password and restricted fields offer none.
- **Only one app has trouble?** Try another KDE Wayland app; application support varies.

[More help, recovery and privacy](docs/install-details.md) · [Report a bug](https://github.com/itsdestin/swipe-keyboard/issues/new?template=bug.yml) · [Release notes](docs/releases/v0.5.0-1.md) · [Changelog](CHANGELOG.md)

For bug reports, include your system, app, keyboard version and steps using made-up text. **Do not send passwords, recordings or personal typing.**

## Licences

| Part | Licence |
|---|---|
| Engine, tools, launcher, Gaming Mode, shared language rules, build files | MIT |
| The two keyboard add-on layers that run inside GPL software: `plugin/` (Plasma 6.7/6.8, linked with Qt Virtual Keyboard) and `frontend-plasma69/` (Plasma 6.9, runs inside KDE's plasma-keyboard) | GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL (KDE's standard) |
| Patches to KDE's own code (`scripts/patches/`) | KDE's licence |
| Next-word models and word lists (Hugging Face FineWeb text, Tatoeba sentences) | ODC-By 1.0 and CC BY 2.0 France: attribution only |
| Swipe model | MIT (trained on FUTO's MIT swipe data plus synthetic swipes) |
| English dictionary base: Android's word list | Apache-2.0 |
| Swear-word lists | CC BY 4.0 |
| Texting-word lists, trivial data | CC0 |
| Documentation | CC BY 4.0 |

Models were trained and released by Destin Moss via youcoded.ai, 2026. No FUTO code or models are in the keyboard. Each file names its licence (SPDX header or `REUSE.toml`; `reuse lint` passes); the texts are in `LICENSES/`, summarised in [LICENSE](LICENSE) and [packaging/NOTICE](packaging/NOTICE).
