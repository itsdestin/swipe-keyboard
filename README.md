# Swipe Keyboard for KDE Plasma

**Swipe across letters to type a word, tap an alternative to correct it, or type normally.** English (US/UK), Spanish, French and German. Recognition runs on your device; recording is off by default.

[**Download v0.2.1-1**](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.2.1-1) · [Install](#install) · [Troubleshooting](#troubleshooting) · [Report a bug](https://github.com/itsdestin/swipe-keyboard/issues/new?template=bug.yml)

![Swipe typing and word alternatives in a Fedora KDE test VM](docs/images/swipe-typing.png)

## Choose your download

This is a **test release for KDE Plasma Wayland**. Application text-input support varies.

| Your system | Package |
|---|---|
| **Arch / CachyOS, x86_64** | [swipe-keyboard-0.2.1-1-x86_64.pkg.tar.zst](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.2.1-1/swipe-keyboard-0.2.1-1-x86_64.pkg.tar.zst) |
| **Fedora 44 Plasma Mobile, ARM64 / aarch64** | [swipe-keyboard-0.2.1-1.fc44.aarch64.rpm](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.2.1-1/swipe-keyboard-0.2.1-1.fc44.aarch64.rpm) |
| **Steam Deck — Desktop Mode, x86_64** | [swipe-keyboard-0.2.1-1-x86_64.flatpak](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.2.1-1/swipe-keyboard-0.2.1-1-x86_64.flatpak) |
| **Fedora 44 KDE / Plasma Mobile, x86_64** | [swipe-keyboard-0.2.1-1.fc44.x86_64.rpm](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.2.1-1/swipe-keyboard-0.2.1-1.fc44.x86_64.rpm) |

- **Deck:** Desktop Mode only. **Gaming Mode is not supported.** Use the Flatpak, not the Fedora or Arch package. No SteamOS root unlock is needed.
- **Native packages:** require Plasma Keyboard 6.7.x/6.8.x—not Maliit—and compatible Qt/glibc libraries. Use the package for your distribution and CPU; do not install the Fedora RPM on Arch or an x86_64 package on ARM.
- **Mobile means Plasma Mobile**, not Android or iOS. The ARM64 RPM is for **Fedora 44 / glibc**; it is not an APK for postmarketOS/Alpine, an ARM32 build or a universal phone installer. Physical-phone performance and other phone distributions need their own checks. See [release scope](docs/releases/v0.2.1-1.md).
- Release publication requires **all four platform packages**. Source preparation or x86 Mobile tests alone do not qualify as ARM acceptance.

Models, dictionaries and notices are included. You do not need a source build, source archive or `.src.rpm` to install. [Optional checksum verification](docs/install-details.md#verify-a-download).

## Install

### Steam Deck — Desktop Mode

1. Switch to **Desktop Mode** and download the `.flatpak` above.
2. Open **Konsole** in your download folder and run:

   ```sh
   flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
   flatpak install --user ./swipe-keyboard-0.2.1-1-x86_64.flatpak
   ```

   Allow Flatpak to install the required runtime. **Do not use `sudo`.**

3. Read the setup disclosure and confirm whether you want to continue:

   ```sh
   flatpak run --user --command=swipe-keyboard-setup org.itsdestin.SwipeKeyboard
   ```

   **You may notice a different keyboard appearance, settings and suggestions.** This is a separate bundled Plasma Keyboard, not a replacement of SteamOS libraries or removal of Maliit. Installing it alone does not activate it. The guide defaults to **No** and never writes your host settings.

4. If you choose to proceed, open **System Settings → Virtual Keyboard**, note the current selection—including **None**—then choose **Swipe Keyboard** and apply. This changes the Desktop Mode input method.
5. **Touch** a normal text field to open the keyboard. Restore your previous selection before uninstalling.

To enable languages, open the bundled keyboard settings:

```sh
flatpak run --user --command=kcmshell6 org.itsdestin.SwipeKeyboard kcm_plasmakeyboard
```

Select your languages there; maximize the window if the list is clipped. Then use the globe key on the keyboard to switch.

If the new entry is missing, log out and back into Desktop Mode, then reopen Settings. Do not unlock SteamOS or change system libraries to make it appear. [Deck setup and removal details](docs/install-details.md#steam-deck-flatpak).

### Arch / CachyOS — x86_64

Open a terminal in the download folder:

```sh
sudo pacman -U ./swipe-keyboard-0.2.1-1-x86_64.pkg.tar.zst
```

### Fedora 44 KDE / Plasma Mobile — x86_64 or ARM64

Use `uname -m` if you are unsure of the architecture. Run **one** matching command in the download folder:

```sh
# x86_64 computer
sudo dnf install ./swipe-keyboard-0.2.1-1.fc44.x86_64.rpm
```

```sh
# ARM64 / aarch64 system
sudo dnf install ./swipe-keyboard-0.2.1-1.fc44.aarch64.rpm
```

### Enable the native package

After either native installation, **inside your logged-in KDE Wayland session**, run as your normal user, **without `sudo`**:

```sh
swipe-keyboard check
swipe-keyboard enable
```

`enable` changes the input method **but preserves your existing keyboard-visibility policy**. Your tablet/desktop manager and physical-keyboard policy remain in control; touch-only devices keep their current behavior. It explains the proposed change and asks **[y/N]**. **You may notice different suggestions and whole-word Backspace behavior.** The native package keeps your system backend and does not replace desktop libraries. Declining leaves settings untouched.

If you deliberately want the touch keyboard available even with a physical keyboard attached, choose `swipe-keyboard enable --always-available` during initial activation. That explicitly opts into KDE visibility mode 1 and is explained before confirmation. Do not choose it for a convertible whose tablet/desktop manager owns visibility. Automated setup can add `--yes` as explicit consent, never as a compatibility bypass.

If dependencies or a compatibility check fail, stop and follow the error. Do not force installation or downgrade desktop libraries. Existing custom keyboard layouts are not overwritten; they need a separate migration.

## Try it

1. Use the keyboard's globe/language key to choose **English (US/UK), Spanish, French or German**. Keep your preferred physical layout; changing language does not force QWERTY.
2. Swipe a word and tap an alternative above the keys to correct it. Normal tapping, accents and symbols remain available. If the globe key has no languages to switch between, open **On-Screen Keyboard** settings and enable your languages; maximize that settings window if the list is clipped.
3. In **English**, tap or swipe a word, press Space, then tap a next-word suggestion to insert it. The three-suggestion bar puts the strongest choice in the middle, with dividers only between words. Native predictions use confirmed words entered through this keyboard; the bundled Deck backend also supports application-provided context. Other languages do not receive English predictions.

The first Backspace can remove the whole swiped word when the application supplies reliable field information; otherwise it deletes normally. Cursor movement, selections or switching fields can clear suggestions to prevent changing the wrong text.

Language accuracy is still developing. French ligature words can require the digraph gesture—for example, swipe `coeur` for **cœur**—or ordinary tapping/correction. Missing or incompatible language data leaves stock tapping available rather than substituting English recognition.

## Update or remove

**Update:** download the newer compatible file and repeat its installation command. Releases are distributed on GitHub; there is no automatic update feed or AUR/COPR repository. A local Flatpak bundle does not receive new keyboard releases merely by running `flatpak update`.

**Deck removal:** first return to **System Settings → Virtual Keyboard** and restore the selection you noted before installation. Then run, without sudo:

```sh
flatpak uninstall --user org.itsdestin.SwipeKeyboard
```

Do not uninstall while Swipe Keyboard is still selected. The Flatpak does **not** automatically restore your previous selection. Its private data is retained unless you explicitly delete it.

**Native removal:** first run `swipe-keyboard disable` as the same normal user who enabled it. Then remove the package:

- **Arch / CachyOS:** `sudo pacman -R swipe-keyboard`
- **Fedora, either architecture:** `sudo dnf remove swipe-keyboard`

Disable restores settings still owned by setup and preserves later manual keyboard choices. Personal options and opt-in recordings are not deleted. [Recovery after removing before disabling](docs/install-details.md#removed-the-package-before-disabling).

## Troubleshooting

| Problem | What to try |
|---|---|
| Keyboard does not appear | Touch the field rather than clicking it. Check **System Settings → Virtual Keyboard** and try another native Wayland app. For native packages, run `swipe-keyboard check` inside KDE, without sudo. |
| Taps work, but swipes do not | Select a supported language and a Latin letter layout. Password/restricted fields and unavailable language data can deliberately disable recognition. |
| No next-word suggestions | Select English, enter a word through this keyboard, then press Space. Native predictions start from confirmed keyboard-entered words, not pasted/preexisting text. Restricted fields, changed context or unavailable models can clear suggestions. Other languages do not use English predictions. |
| Works in one app but not another | Wayland text-input support varies. Include the app name/version and whether it is a Flatpak in a bug report. XWayland compatibility is not promised. |
| Alternatives or whole-word Backspace do not work | Cursor movement, selections or missing field information can prevent safe word replacement. Try a normal field with no selection. |
| Custom layouts or a disable error | Keep your files and the exact error. Do not delete layouts, remove the setup record or force-reset settings. See [recovery details](docs/install-details.md). |

**Known Firefox issue:** physical Ctrl+A/Delete can leave field information stale, causing an extra leading space on the next swipe.

[**Report a bug**](https://github.com/itsdestin/swipe-keyboard/issues/new?template=bug.yml). Include your OS, app/version, package version and a short reproduction using made-up text; native users can include `swipe-keyboard check`. **Do not attach passwords, typing recordings or unreviewed logs.**

## Details and credits

- [Setup, recovery and privacy](docs/install-details.md)
- [Release scope and verification](docs/releases/v0.2.1-1.md)
- [Building from source and contributing](CONTRIBUTING.md)

**Powered by [FUTO Swipe](https://gitlab.futo.org/keyboard/swipe-library).** Code is GPL-3.0; model weights use the FUTO Model Weights License 1.0. Dictionaries include AOSP/wordfreq data with separate attribution and CC-BY-SA terms. See [NOTICE](packaging/NOTICE) and the notices supplied with each language pack. Qt/KDE components retain their licenses. This project is not endorsed by Valve, FUTO, KDE or Google.
