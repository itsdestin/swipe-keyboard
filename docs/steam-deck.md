# Steam Deck details

Short install steps are in the [README](../README.md#steam-deck). This page holds the fine print for Gaming Mode and the Flatpak-only install. Files, status messages and every command: [Gaming Mode in install-details](install-details.md#gaming-mode-on-steam-deck-preview-first-released-in-v041-1).

Gaming Mode is new and has been tested on a virtual Steam Deck, not yet on a real one. Please [report problems](https://github.com/itsdestin/swipe-keyboard/issues), especially jumbled text.

## The risk, in full

Gaming Mode works through a developer switch that Valve provides in Steam. Steam's Subscriber Agreement says you may not modify software accessed through Steam without Valve's consent. Whether this counts is unclear. We change no Steam file and do not touch games or anti-cheat, and a popular community tool (Decky Loader) uses the same switch, but Valve has not approved this and could stop it. With the switch on, other programs on your Deck can control Steam's screens. We are not lawyers; use it only if you are comfortable with that.

## The installer's one question

The installer lists every change, then asks **Also swipe in Gaming Mode? [Y/n]**.

- **Yes (Enter):** turns on Valve's developer switch, and blocks other devices on your network from reaching it. SteamOS opens that switch to your whole Wi-Fi or home network (port 8081); blocking asks for the Deck's administrator password once (if you never set one, run `passwd` in Konsole first) and writes one small file in `/etc/systemd/system`. If blocking fails, the installer asks what to do and never turns Gaming Mode on with the door open unless you say so. Steam needs one restart.
- **No:** the Desktop Mode keyboard only. Nothing about Steam or your network changes and no password is asked.

If you said no before, running the installer again keeps no as the default. **Leave the network door open on purpose** (for developer tools on another computer): `bash install.sh --gaming --leave-open`. No password, but anyone on the same network can then control Steam on your Deck; never on public or shared Wi-Fi.

## Passwords

Inside Steam's own password boxes the keyboard layer does nothing. Over games and other programs Steam does not say whether a box is a password box, so nothing is learned or saved there, and suggestions are dropped when the keyboard closes.

## Swipes start on a letter key

A finger that rests on a letter for a moment before moving can type that one letter. Backspace, Shift, Space and Enter are unchanged.

## If the developer switch is turned off

If you, another tool or Steam turns the switch off, Gaming Mode swipe stops and Steam's normal keyboard is used. We never put the file back on our own. The next time you are in Desktop Mode, a notification offers **Turn it back on** or **Dismiss**. If Steam changes something we depend on, Gaming Mode switches itself off.

## Changing your mind

Run `bash install.sh` again to add or remove Gaming Mode. `--block-outside` or `--leave-open` switches the network choice. `bash uninstall.sh` removes only what the installer added.

## Desktop Mode only, without the installer (no automatic updates)

[Download the Flatpak](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.5.1-1/swipe-keyboard-0.5.1-1-x86_64.flatpak), then in Konsole:

```sh
flatpak install --user ~/Downloads/swipe-keyboard-0.5.1-1-x86_64.flatpak
```

Then select Swipe Keyboard in **System Settings → Virtual Keyboard**. To update, install a newer file. Remove with `flatpak uninstall --user org.itsdestin.SwipeKeyboard`. Before choosing it, you can read what changes with `flatpak run --user --command=swipe-keyboard-setup org.itsdestin.SwipeKeyboard` (read-only; the installer's route does not require it).
