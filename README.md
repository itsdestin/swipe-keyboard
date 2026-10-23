# Swipe Keyboard for KDE Plasma

Swipe across letters to type, or tap normally. English (US/UK), Spanish, French and German. Works locally; typing recording is off by default.

**Test release v0.4.0-1 · KDE Plasma Wayland**

Choose your platform: [Steam Deck](#steam-deck--desktop-mode) · [Gaming Mode (preview)](#steam-deck--gaming-mode-preview) · [Arch / CachyOS](#arch--cachyos) · [Fedora 44 / Plasma Mobile](#fedora-44--plasma-mobile)

## Install

### Steam Deck — Desktop Mode

**[Download the Steam Deck Flatpak](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.4.0-1/swipe-keyboard-0.4.0-1-x86_64.flatpak)**

Desktop Mode only; this does not change Steam's Gaming Mode keyboard (see the Gaming Mode preview below). Do not unlock SteamOS or install the Arch/Fedora packages on your Deck.

1. Switch to **Desktop Mode**. Save the download to **Downloads**, then open **Konsole** (the terminal app) from the application menu.
2. Install it; **do not use `sudo`**:

   ```sh
   flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
   flatpak install --user ~/Downloads/swipe-keyboard-0.4.0-1-x86_64.flatpak
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

### Steam Deck — Gaming Mode (preview)

**[Download the SteamOS file](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.4.0-1/swipe-keyboard-0.4.0-1-steamos-x86_64.tar.gz)**

**Preview: tested only on a virtual Steam Deck so far, not yet on a real one.** If something goes wrong, Steam's normal keyboard keeps working, and `bash uninstall.sh` removes everything. Please [report problems](https://github.com/itsdestin/swipe-keyboard/issues), especially text that comes out jumbled. Read the warning below before installing.

Gaming Mode support keeps Steam's own on-screen keyboard exactly as it is and adds swipe typing and a row of word suggestions on top of it. It is a separate, opt-in step you take in Desktop Mode, and it can be undone completely. This one download also contains the Desktop Mode keyboard, so you do not need the Flatpak above as well.

1. Switch to **Desktop Mode** and save the download to **Downloads**. Open **Konsole** and run, **without `sudo`**:

   ```sh
   cd ~/Downloads && tar xzf swipe-keyboard-0.4.0-1-steamos-x86_64.tar.gz && cd swipe-keyboard-steamos-0.4.0-1
   bash install.sh
   ```

   It asks two plain-language questions and shows you everything it will change before doing anything.
2. Restart Steam once if it was already open (the installer never restarts Steam while a game is running).
3. Switch to Gaming Mode, touch a text field and swipe across the letters.

**Choice 1: do you want Gaming Mode at all?** Pressing Enter picks the safer answer, the Desktop Mode keyboard only: nothing about Steam or your network changes and no password is asked.

| | Desktop keyboard only (default) | Also Gaming Mode swipe |
|---|---|---|
| You get | Swipe typing in Desktop Mode | Swipe typing and suggestions over Steam and games too |
| Cost | None | Turns on a Valve developer switch that Steam's rules arguably forbid using this way; Valve can break it at any time; Steam needs one restart |

**Choice 2 (only if you chose Gaming Mode): who else may reach Steam's developer switch?** On SteamOS the switch can also be reached from other devices on your Wi-Fi or home network, who could then control Steam and run programs on your Deck.

| | Block other devices (default) | Leave it open |
|---|---|---|
| You get | Only your Deck can use the switch; safe on shared or public Wi-Fi | No password prompt; keeps network access for people who use it on purpose, such as developer tools on another computer |
| Cost | Asks for your Deck's administrator password once (if you never set one, run `passwd` in Konsole first). Network tools that use the switch stop working | Anyone on the same network can control Steam on your Deck. Do not use on public or shared Wi-Fi |

If you choose to block and then skip or fail the password, the installer asks again: leave it open, or skip Gaming Mode. It never turns Gaming Mode on with the door open unless you say so. Blocking changes one small file in `/etc/systemd/system` and nothing else on your Deck.

**If the developer switch gets turned off.** Gaming Mode swipe depends on one small file that Steam reads when it starts. If that file disappears (you or another tool removed it, or Steam turned the setting off), swipe in Gaming Mode stops and Steam's normal keyboard is used, which always keeps working. We never put the file back on our own. Instead, a check on your Deck notices within about a minute and, the next time you are in Desktop Mode, shows a notification: "Steam's developer switch was turned off" with two buttons, **Turn it back on** and **Dismiss**. You are told once; you are told again only if it goes off again after being fixed. Gaming Mode itself cannot show our notification (nothing in Gaming Mode can), so while you are only playing you will not hear about it until you next visit Desktop Mode. **Turn it back on** opens a small window that shows the one empty file it will create, asks you to confirm, and reminds you to restart Steam once. Running `bash install.sh` again does the same and says what it found.

**Change your mind later.** Run `bash install.sh` again. It offers both choices again, independently: add Gaming Mode later, turn it off while keeping the Desktop keyboard, or switch between blocked and open. To remove everything, run `bash uninstall.sh` from the same download. It only removes what the installer added, including the block if the installer made it.

**How this works, and a warning.** Gaming Mode support works by attaching to Steam's own on-screen keyboard through a developer switch that Valve provides in Steam. It adds swipe typing and suggestions on top and leaves Steam's keyboard as it is. Steam's Subscriber Agreement says you may not modify or create derivative works of software accessed through Steam without Valve's written consent. Whether this counts is unclear. We do not change any Steam file, we do not touch games or anti-cheat, and a popular community tool (Decky Loader) uses the same switch, but Valve has not approved this and could stop it or object at any time. Turning the switch on lets other programs on your Deck control Steam's screens, and, unless you block it in the second install question, so can other devices on your Wi-Fi or home network (SteamOS opens a second door, port 8081, to the whole network). You can turn this off at any time. We are not lawyers; use it only if you are comfortable with that.

**Passwords.** Inside Steam's own password boxes the keyboard layer does nothing. Over games and other programs, Steam does not say whether a box is a password box, so nothing is learned or saved there and suggestions are kept only until you close the keyboard.

**Swipes start on a letter key.** Steam's keyboard already types letters when you lift your finger, so taps should feel the same as before (to be measured on a real Deck). A finger that rests on a letter for a moment before moving can type that one letter; Backspace, Shift, Space and Enter are unchanged.

**If Steam changes something we depend on, Gaming Mode support switches itself off and Steam's keyboard keeps working.** Updates are manual downloads.

### Arch / CachyOS

**[Download the x86_64 package](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.4.0-1/swipe-keyboard-0.4.0-1-x86_64.pkg.tar.zst)**

Requires KDE Plasma Wayland with Plasma Keyboard 6.7 or newer. It works with 6.7.x and 6.8.x; on Plasma 6.9 and later it steps aside (you get KDE's plain keyboard) until a version made for 6.9 is released, and it never blocks the Plasma upgrade.

1. Save the download to **Downloads**, then open **Konsole** from the application menu and paste:

   ```sh
   sudo pacman -U ~/Downloads/swipe-keyboard-0.4.0-1-x86_64.pkg.tar.zst
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

For KDE Plasma or Plasma Mobile with Plasma Keyboard 6.7.x or 6.8.x (on 6.9 and later it steps aside, see above). Choose the file matching your device:

| Device | Download |
|---|---|
| Intel / AMD — x86_64 | [Fedora x86_64 RPM](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.4.0-1/swipe-keyboard-0.4.0-1.fc44.x86_64.rpm) |
| ARM64 — aarch64 | [Fedora ARM64 RPM](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.4.0-1/swipe-keyboard-0.4.0-1.fc44.aarch64.rpm) |

Unsure which? `uname -m` in a terminal reports `x86_64` or `aarch64`.

1. Save the download to **Downloads**, then open **Konsole** from the application menu. Paste **only the matching install command**:

   **Intel / AMD:**
   ```sh
   sudo dnf install ~/Downloads/swipe-keyboard-0.4.0-1.fc44.x86_64.rpm
   ```

   **ARM64:**
   ```sh
   sudo dnf install ~/Downloads/swipe-keyboard-0.4.0-1.fc44.aarch64.rpm
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
- **Learning (off unless you turn it on):** the keyboard can remember your names, slang and favourite word pairs, on this computer only, and never from password, e-mail, web-address or number fields. Turn it on or off with `swipe-keyboard learning on` / `off`, erase it with `swipe-keyboard learning forget`, or press and hold a learned suggestion and choose **Remove from dictionary**. [Details](docs/install-details.md#learning-from-your-typing-off-by-default)
- **Move the cursor:** slide your finger (or mouse) sideways along the Space bar. The cursor moves in the direction you slide and no space is typed: one character at a time when you slide slowly, and further the faster you slide, like a touchpad. A normal tap still types a space. It works on the letters page, including in password fields, but not on the `?123` symbols page.
- **Texting words:** common abbreviations and slang (lol, wyd, rn, jaja, mdr, lg ...) can be swiped and completed in English, Spanish, French and German. Swear-word abbreviations are never suggested before you type them.
- **Backspace:** the first press can remove the whole swiped word. Moving the cursor or changing fields can clear suggestions.

## Help

- **Keyboard not appearing?** Touch the field instead of clicking it. Native-package users can run `swipe-keyboard check`.
- **No next-word suggestions?** Select a supported language, enter a word through this keyboard and press Space. Native predictions start from keyboard-entered words, not pasted text. Password and restricted fields do not offer suggestions.
- **Only one app has trouble?** Try another KDE Wayland app; application support varies.

[More help, recovery and privacy](docs/install-details.md) · [Report a bug](https://github.com/itsdestin/swipe-keyboard/issues/new?template=bug.yml) · [Release notes](docs/releases/v0.4.0-1.md)

For bug reports, include your system, app, keyboard version and steps using made-up text. **Do not send passwords, recordings or personal typing.**

Powered by [FUTO Swipe](https://gitlab.futo.org/keyboard/swipe-library). [Licenses and notices](packaging/NOTICE).
