# Setup and recovery details

For download links and short installation steps, see the [main page](../README.md#install). This page describes the **v0.2.0** Deck Flatpak and Fedora package. [The r3 release notes](releases/v0.1.0-r3.md) describe the older, English-only native downloads.

## Verify a download

Download [SHA256SUMS.txt](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.2.0/SHA256SUMS.txt) into the same folder as the package. Open a terminal there and run:

```sh
sha256sum --check --ignore-missing SHA256SUMS.txt
```

Your downloaded package should report **OK**. Do not install a mismatching file. A checksum checks the file against the published list; it is not a package-signing service.

## Steam Deck Flatpak

### Installation and selection

Use **Desktop Mode** and the per-user `.flatpak` download. It bundles its own keyboard backend and uses the KDE Flatpak runtime instead of replacing SteamOS Qt/Plasma libraries. Flatpak may download that runtime on first installation; normal recognition does not require network access.

The keyboard entry appears in **System Settings → Virtual Keyboard**, not as an ordinary application-menu launch. Record the previous selection before choosing Swipe Keyboard. If the entry is missing after installation, log out and back into Desktop Mode, then reopen Settings.

Open the **bundled language settings** with this ordinary-user command; no path overrides or extra permissions are needed:

```sh
flatpak run --user --command=kcmshell6 org.itsdestin.SwipeKeyboard kcm_plasmakeyboard
```

Enable your languages, maximizing the window if the list is clipped. The keyboard's globe key switches between enabled languages.

KWin starts the selected input method and supplies its privileged Wayland connection. Running the keyboard directly from Konsole is not an activation method. **Do not run `swipe-keyboard enable` or `disable` for the Flatpak**; those commands belong to the native package.

The bundle requests ordinary Wayland and inherited Wayland-socket access. It does not request host/home filesystem access, network access, host shell execution, broad device permissions or X11 access. Do not add overrides to work around an unsupported application.

### Updates and removal

- **Keyboard update:** download the newer bundle and repeat `flatpak install --user ./org.itsdestin.SwipeKeyboard.flatpak`. Keep the existing selection unless release instructions say otherwise. The local bundle is not an automatic keyboard-update feed; `flatpak update` can update its runtime but is not a substitute for downloading a new keyboard release.
- **SteamOS update:** use the ordinary supported SteamOS update mechanism. Do not unlock the root filesystem, install this project's native Arch package on SteamOS, or replace system libraries. The [release notes](releases/v0.2.0.md) distinguish tested reboot behavior from OS-update survival; do not assume the latter from the former.
- **Removal:** restore the previous entry—or None, if that was the original setting—in **System Settings → Virtual Keyboard**, then run `flatpak uninstall --user org.itsdestin.SwipeKeyboard` without sudo. There is **no automatic selection restoration** for this bundle.
- **Already uninstalled:** select the previous/stock input method manually in Settings. Reinstall the bundle if you want Swipe Keyboard again. Do not delete all KDE keyboard settings to recover one selection.

Private app data is under `~/.var/app/org.itsdestin.SwipeKeyboard/`; the keyboard uses its app-scoped data directory. Uninstalling without `--delete-data` retains that data. You do not need to remove shared Flatpak runtimes.

**Gaming Mode is not supported.** Steam's built-in Gaming Mode keyboard is a different integration. Installing the Desktop bundle does not replace it. Physical Steam Deck hardware performance is not established by our virtual-machine checks.

## Fedora and Plasma Mobile native package

The supplied RPM is for **Fedora 44, x86_64**, with KDE Plasma Wayland and **Plasma Keyboard 6.7.x/6.8.x**. Maliit is not interchangeable with Plasma Keyboard. The current native build uses **Qt 6.11.2**; package dependencies and `swipe-keyboard check` must both succeed. Do not ignore library dependencies or force the plugin to load on an older desktop.

**Plasma Mobile** describes the KDE shell. It does not mean Android/iOS support or an ARM-phone build. No ARM package is supplied in this release, and x86 virtual-machine results do not certify phone hardware, battery use or touch ergonomics.

`swipe-keyboard check` reports versions, session and selection, then checks native loading before activation changes settings. It is not a touchscreen or universal application-compatibility test. Run both `check` and `enable` inside the logged-in KDE session, as the desktop user and without sudo.

Native packages use the distro's Plasma Keyboard backend. Contextual next-word prediction stays disabled unless that backend provides the required privacy-policy bridge. The Deck bundle includes that bridge; do not patch system backend files manually to obtain predictions.

Do not upgrade your whole desktop solely to match a build machine, downgrade individual libraries or force package dependencies. If a desktop update moves Plasma Keyboard outside the accepted series and no compatible package exists, disable and remove the extension before that update.

### Existing custom keyboard layouts

Native setup refuses to overwrite a custom layout tree at `~/.local/share/plasma/keyboard/layouts` or its `$XDG_DATA_HOME` equivalent. Keep that directory intact. Deleting it or layering two customizations is not a safe migration.

The legacy repository `install.sh` is a developer tool, **not** the installer for the standalone release. Use the compatible native package and `swipe-keyboard enable` instead.

## Removed the package before disabling?

For the **native package**, KDE may still be configured to start a missing launcher:

1. Reinstall a compatible package.
2. As the **same normal user who enabled it**, run `swipe-keyboard disable`.
3. Remove the package again.

If a desktop upgrade prevents reinstalling, select stock **Plasma Keyboard** in **System Settings → Virtual Keyboard**. That restores a usable selection but does not reconstruct all prior settings.

Preserve `~/.local/share/swipe-keyboard/setup-state.json` (or its XDG equivalent). Do not delete the restoration record or guess previous values. Keep the exact error if disable fails and retry from the original user's KDE session without sudo. For Flatpak recovery, use the [Deck procedure above](#steam-deck-flatpak), not the native setup record.

## Languages, suggestions and application limits

- Supported language profiles are English (US/UK), Spanish, French and German. Enable the languages in the normal **On-Screen Keyboard** settings, then use the globe key to switch. Maximize the settings window if its header/footer hide the language list. Locale does not force a different physical layout.
- Missing/incompatible language packs disable swipe recognition for that profile while retaining stock tapping. There is no silent English fallback.
- English next-word suggestions require the bundled/compatible backend's privacy support. They insert after a confirmed word; correction choices replace the last word. Non-English profiles do not use English predictions.
- French ligature recognition remains limited: the tested short `cour`-shaped gesture does not recognize **cœur**; the `coeur` digraph gesture does. Tapping and alternatives remain useful fallbacks. Synthetic fixture results are not human-language accuracy measurements.
- Keyboard appearance depends on the application's Wayland text-input support. Native Wayland success does not establish XWayland or every browser/Flatpak application's compatibility.
- Alternatives and whole-word deletion require reliable field/cursor/selection information. Changed or unavailable context intentionally clears replacement authority or falls back to ordinary deletion.
- An application edit that is never reported to the input method cannot be detected reliably. Firefox physical Ctrl+A/Delete can leave stale field information and cause an extra leading space on the next swipe.

## Privacy and stored files

Recognition runs locally. Recording is **off by default**. Sensitive-field cancellation uses the live field hints as well as cached privacy vetoes; actual effectiveness still depends on the application/backend supplying correct input information. Do not interpret one tested configuration as a universal privacy guarantee.

Native paths:

- Installed code, models and dictionaries: `/usr/share/swipe-keyboard/`.
- User options, restoration record and private layout overlay: `$XDG_DATA_HOME/swipe-keyboard/`, normally `~/.local/share/swipe-keyboard/`.

The Flatpak uses app-scoped storage instead of the native paths. Developer recording requires an explicitly created `record` switch file; recordings can contain typed words and context and are not needed for ordinary use or support requests. They are not automatically deleted when removing the package.

Never send passwords, personal typing, learned words, recordings or unreviewed logs. Use made-up text. Include the OS, application/version, package version and reproduction; native users can also include `swipe-keyboard check`.

## Source and licenses

Use the complete corresponding-source archive attached to the [v0.2.0 release](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.2.0) for rebuilding. GitHub's automatic “Source code” ZIP/tar downloads omit submodules. The Fedora `.src.rpm`, when supplied, is for rebuilding, not installation. See [CONTRIBUTING.md](../CONTRIBUTING.md).

FUTO's swipe-library is GPL-3.0; model weights use the separate FUTO Model Weights License 1.0. Dictionaries incorporate AOSP word lists (Apache-2.0) and wordfreq data (CC-BY-SA-4.0). Generated dictionary data retains CC-BY-SA attribution/terms, including language-pack transformation notices. Preserve all supplied terms, attribution and source obligations. See [NOTICE](../packaging/NOTICE) and each pack's notices. Qt/KDE components retain their licenses. Optional development models have separate terms and are not part of the base release.
