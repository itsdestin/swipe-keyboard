# Swipe Keyboard for KDE Plasma

Swipe across letters to type, or tap normally. English (US/UK), Spanish, French and German. Works locally; typing recording is off by default.

**Test release v0.3.1-1 · KDE Plasma Wayland**

Choose your platform: [Steam Deck](#steam-deck--desktop-mode) · [Arch / CachyOS](#arch--cachyos) · [Fedora 44 / Plasma Mobile](#fedora-44--plasma-mobile)

## Install

### Steam Deck — Desktop Mode

**[Download the Steam Deck Flatpak](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.3.1-1/swipe-keyboard-0.3.1-1-x86_64.flatpak)**

Desktop Mode only; this does not change Steam's Gaming Mode keyboard. Do not unlock SteamOS or install the Arch/Fedora packages on your Deck.

1. Switch to **Desktop Mode**. Save the download to **Downloads**, then open **Konsole** (the terminal app) from the application menu.
2. Install it; **do not use `sudo`**:

   ```sh
   flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
   flatpak install --user ~/Downloads/swipe-keyboard-0.3.1-1-x86_64.flatpak
   ```

   Allow Flatpak to download any required supporting components.

3. Open the setup guide and read the changes before continuing:

   ```sh
   flatpak run --user --command=swipe-keyboard-setup org.itsdestin.SwipeKeyboard
   ```

   You may notice a different keyboard appearance, settings and suggestions. Read the explanation and confirm if you want to continue. You activate the keyboard yourself in Settings in the next step.

4. In **System Settings → Virtual Keyboard**, note your current selection (including **None**), select **Swipe Keyboard**, and apply.
5. **Touch** a text field to open the keyboard. If Swipe Keyboard is missing from Settings, log out and back into Desktop Mode.

**Languages:** open the bundled settings, enable your languages, then use the keyboard's globe key to switch:

```sh
flatpak run --user --command=kcmshell6 org.itsdestin.SwipeKeyboard kcm_plasmakeyboard
```

**Update:** get the newer Deck `.flatpak` from [Releases](https://github.com/itsdestin/swipe-keyboard/releases), save it to **Downloads**, and repeat the `flatpak install` command using the new filename. Then save your work, log out and back into Desktop Mode to load the new keyboard. Keep Swipe Keyboard selected; no need to repeat setup. Updates are manual: `flatpak update` alone does not fetch a new keyboard release.

**Remove:** first restore your previous selection in **System Settings → Virtual Keyboard**, then run:

```sh
flatpak uninstall --user org.itsdestin.SwipeKeyboard
```

### Arch / CachyOS

**[Download the x86_64 package](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.3.1-1/swipe-keyboard-0.3.1-1-x86_64.pkg.tar.zst)**

Requires KDE Plasma Wayland with Plasma Keyboard 6.7.x or 6.8.x.

1. Save the download to **Downloads**, then open **Konsole** from the application menu and paste:

   ```sh
   sudo pacman -U ~/Downloads/swipe-keyboard-0.3.1-1-x86_64.pkg.tar.zst
   ```

2. Enable the keyboard **without `sudo`**:

   ```sh
   swipe-keyboard enable
   ```

   This checks compatibility and asks for confirmation before changing your keyboard selection. Read the proposed changes and answer **y** to continue. Your existing show/hide settings are preserved. Suggestions and Backspace behavior may differ from your previous keyboard. If setup reports an error, stop and follow its message.
3. **Touch** a text field to open the keyboard.

**Update:** get the newer Arch/CachyOS `.pkg.tar.zst` from [Releases](https://github.com/itsdestin/swipe-keyboard/releases), save it to **Downloads**, and repeat the `pacman -U` command using the new filename. Save your work, then log out and back in to load the new keyboard. No need to disable or enable again.

**Remove:** restore your previous keyboard and then remove the package:

```sh
swipe-keyboard disable && sudo pacman -R swipe-keyboard
```

If restoring the keyboard fails, removal stops. Follow the error or use the [recovery guide](docs/install-details.md#removed-the-package-before-disabling).

### Fedora 44 / Plasma Mobile

For KDE Plasma or Plasma Mobile with Plasma Keyboard 6.7.x or 6.8.x. Choose the file matching your device:

| Device | Download |
|---|---|
| Intel / AMD — x86_64 | [Fedora x86_64 RPM](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.3.1-1/swipe-keyboard-0.3.1-1.fc44.x86_64.rpm) |
| ARM64 — aarch64 | [Fedora ARM64 RPM](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.3.1-1/swipe-keyboard-0.3.1-1.fc44.aarch64.rpm) |

Unsure which? `uname -m` in a terminal reports `x86_64` or `aarch64`.

1. Save the download to **Downloads**, then open **Konsole** from the application menu. Paste **only the matching install command**:

   **Intel / AMD:**
   ```sh
   sudo dnf install ~/Downloads/swipe-keyboard-0.3.1-1.fc44.x86_64.rpm
   ```

   **ARM64:**
   ```sh
   sudo dnf install ~/Downloads/swipe-keyboard-0.3.1-1.fc44.aarch64.rpm
   ```

2. Enable the keyboard **without `sudo`**:

   ```sh
   swipe-keyboard enable
   ```

   This checks compatibility and asks for confirmation before changing your keyboard selection. Read the proposed changes and answer **y** to continue. Your existing show/hide settings are preserved. Suggestions and Backspace behavior may differ from your previous keyboard. If setup reports an error, stop and follow its message.
3. **Touch** a text field to open the keyboard.

**Update:** get the newer Fedora `.rpm` for the same device type from [Releases](https://github.com/itsdestin/swipe-keyboard/releases), save it to **Downloads**, and repeat the matching `dnf install` command using the new filename. Save your work, then log out and back in to load the new keyboard. No need to disable or enable again.

**Remove:** restore your previous keyboard and then remove the package:

```sh
swipe-keyboard disable && sudo dnf remove swipe-keyboard
```

If restoring the keyboard fails, removal stops. Follow the error or use the [recovery guide](docs/install-details.md#removed-the-package-before-disabling).

## Using the keyboard

- **Languages:** enable languages in **On-Screen Keyboard** settings, then switch with the globe key. Deck users should use the settings command in their section above. Maximize the settings window if the list is clipped.
- **Swipe:** drag across letters and lift to enter a word. Tap an alternative above the keys to change it.
- **Tap typing:** suggestions finish or correct the word you are typing. The strongest is centered, and your exact spelling remains available. Only tapping a suggestion changes the word; Space does not autocorrect it.
- **Next-word suggestions:** in English, Spanish, French or German, tap or swipe a word, then press Space. The strongest suggestion is in the middle. Tap a suggestion to insert it. Spanish, French and German suggestions are an early baseline and can be repetitive or less natural.
- **Move the cursor:** slide your finger (or mouse) sideways along the Space bar. The cursor follows one character at a time, in the direction you slide, and no space is typed. A normal tap still types a space. It works on the letters page, including in password fields, but not on the `?123` symbols page.
- **Backspace:** the first press can remove the whole swiped word. Moving the cursor or changing fields can clear suggestions.

## Help

- **Keyboard not appearing?** Touch the field instead of clicking it. Native-package users can run `swipe-keyboard check`.
- **No next-word suggestions?** Select a supported language, enter a word through this keyboard and press Space. Native predictions start from keyboard-entered words, not pasted text. Password and restricted fields do not offer suggestions.
- **Only one app has trouble?** Try another KDE Wayland app; application support varies.

[More help, recovery and privacy](docs/install-details.md) · [Report a bug](https://github.com/itsdestin/swipe-keyboard/issues/new?template=bug.yml) · [Release notes](docs/releases/v0.3.1-1.md)

For bug reports, include your system, app, keyboard version and steps using made-up text. **Do not send passwords, recordings or personal typing.**

Powered by [FUTO Swipe](https://gitlab.futo.org/keyboard/swipe-library). [Licenses and notices](packaging/NOTICE).
