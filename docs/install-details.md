# Setup and recovery details

For download links and short installation steps, see the [main page](../README.md#install). This page describes **v0.2.1-1**: the Deck Flatpak, Arch/CachyOS package, and Fedora x86_64 and ARM64 packages. [The r3 release notes](releases/v0.1.0-r3.md) describe the older, English-only native downloads.

## Verify a download

Download [SHA256SUMS](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.2.1-1/SHA256SUMS) into the same folder as the package. Open a terminal there and run:

```sh
sha256sum --check --ignore-missing SHA256SUMS
```

Your downloaded package should report **OK**. Do not install a mismatching file. A checksum checks the file against the published list; it is not a package-signing service.

## Steam Deck Flatpak

### Installation and selection

Use **Desktop Mode** and the per-user `.flatpak` download. It bundles its own keyboard backend and uses the KDE Flatpak runtime instead of replacing SteamOS Qt/Plasma libraries. Flatpak may download that runtime on first installation; normal recognition does not require network access.

Before activation, run `flatpak run --user --command=swipe-keyboard-setup org.itsdestin.SwipeKeyboard` in Konsole. The read-only guide explains **“You may notice…”** appearance/settings/suggestion differences and asks **[y/N]** before showing setup steps. It never changes host settings: the bundle is a separate Plasma Keyboard, not a system-library replacement or removal of Maliit. Installing it alone does not select it.

The keyboard entry appears in **System Settings → Virtual Keyboard**, not as an ordinary application-menu launch. Record the previous selection before choosing Swipe Keyboard. If the entry is missing after installation, log out and back into Desktop Mode, then reopen Settings.

Open the **bundled language settings** with this ordinary-user command; no path overrides or extra permissions are needed:

```sh
flatpak run --user --command=kcmshell6 org.itsdestin.SwipeKeyboard kcm_plasmakeyboard
```

Enable your languages, maximizing the window if the list is clipped. The keyboard's globe key switches between enabled languages.

KWin starts the selected input method and supplies its privileged Wayland connection. Running the keyboard directly from Konsole is not an activation method. **Do not run `swipe-keyboard enable` or `disable` for the Flatpak**; those commands belong to the native package.

The bundle requests ordinary Wayland and inherited Wayland-socket access. It does not request host/home filesystem access, network access, host shell execution, broad device permissions or X11 access. Do not add overrides to work around an unsupported application.

### Updates and removal

- **Keyboard update:** download the newer bundle and repeat `flatpak install --user ./swipe-keyboard-0.2.1-1-x86_64.flatpak`. Keep the existing selection unless release instructions say otherwise. The local bundle is not an automatic keyboard-update feed; `flatpak update` can update its runtime but is not a substitute for downloading a new keyboard release.
- **SteamOS update:** use the ordinary supported SteamOS update mechanism. Do not unlock the root filesystem, install this project's native Arch package on SteamOS, or replace system libraries. The [release notes](releases/v0.2.1-1.md) distinguish tested reboot behavior from OS-update survival; do not assume the latter from the former.
- **Removal:** restore the previous entry—or None, if that was the original setting—in **System Settings → Virtual Keyboard**, then run `flatpak uninstall --user org.itsdestin.SwipeKeyboard` without sudo. There is **no automatic selection restoration** for this bundle.
- **Already uninstalled:** select the previous/stock input method manually in Settings. Reinstall the bundle if you want Swipe Keyboard again. Do not delete all KDE keyboard settings to recover one selection.

Private app data is under `~/.var/app/org.itsdestin.SwipeKeyboard/`; the keyboard uses its app-scoped data directory. Uninstalling without `--delete-data` retains that data. You do not need to remove shared Flatpak runtimes.

**Gaming Mode is not supported.** Steam's built-in Gaming Mode keyboard is a different integration. Installing the Desktop bundle does not replace it. Physical Steam Deck hardware performance is not established by our virtual-machine checks.

## Native packages: Arch/CachyOS and Fedora Plasma Mobile

Native packages require KDE Plasma Wayland and **Plasma Keyboard 6.7.x/6.8.x**, not Maliit. The release provides **Arch/CachyOS x86_64**, **Fedora 44 x86_64** and **Fedora 44 ARM64/aarch64** packages. The package records its build ABI; dependency resolution and `swipe-keyboard check` must both succeed. Do not ignore library dependencies or force a plugin onto an incompatible desktop.

For Plasma Mobile on ARM64, use the **aarch64 Fedora RPM** only on a compatible Fedora 44/glibc system. `uname -m` reports the CPU architecture. It is not an ARM32, Android/iOS or postmarketOS/Alpine APK package; other distributions, especially musl-based ones, need their own builds. ARM64 is a required release target and needs its own build/load and Plasma Mobile input acceptance, not an inference from x86 testing. Emulated ARM checks do not establish physical-phone performance, battery use or touch ergonomics.

`swipe-keyboard check` reports versions, session and selection, then checks native loading before activation changes settings. It is not a touchscreen or universal application-compatibility test. Run both `check` and `enable` inside the logged-in KDE session, as the desktop user and without sudo.

Default activation changes only the input method; it does **not** write `VirtualKeyboardMode`, change the live mode, or capture/modify a tablet manager's desktop/tablet baseline. Disable likewise leaves visibility alone for this policy-preserving activation. This respects convertible mode 0 with a physical keyboard, tablet mode 1, and existing touch-only policies. If visibility changes later through your manager or KDE Settings, those changes remain authoritative.

For an explicit always-available preference, use `enable --always-available` on initial activation. This separately opts into mode 1; setup records ownership for guarded restoration but never edits an external tablet manager's configuration. Already-enabled setups must disable before opting into a different policy; no old journal is silently reinterpreted. Older records retain their original provenance safeguards, so this update does not automatically reverse an already repaired device or invent a missing baseline.

Before activation, `enable` displays the effective current and proposed input-method settings and whether visibility is **preserved** or explicitly changed, explains likely suggestion/Backspace differences, then asks **[y/N]**. Declining, an empty response, or a noninteractive invocation without `--yes` leaves settings untouched. Automated callers may pass `enable --yes` as explicit consent; compatibility and ownership checks still apply. Normal compositor launches never prompt. The explicit `disable` command remains the restoration action and retains its ownership safeguards.

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

Preserve `~/.local/share/swipe-keyboard/setup-state.json` (or its XDG equivalent). Older setup records may contain effective values without proving whether a raw user key existed. If disable reports missing legacy provenance, it deliberately refuses to guess; retain the record and choose your intended keyboard preferences explicitly. Do not delete the restoration record or guess previous values. Keep the exact error if disable fails and retry from the original user's KDE session without sudo. For Flatpak recovery, use the [Deck procedure above](#steam-deck-flatpak), not the native setup record.

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

Use the complete corresponding-source archive attached to the [v0.2.1-1 release](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.2.1-1) for rebuilding. GitHub's automatic “Source code” ZIP/tar downloads omit submodules. The Fedora `.src.rpm`, when supplied, is for rebuilding, not installation. See [CONTRIBUTING.md](../CONTRIBUTING.md).

FUTO's swipe-library is GPL-3.0; model weights use the separate FUTO Model Weights License 1.0. Dictionaries incorporate AOSP word lists (Apache-2.0) and wordfreq data (CC-BY-SA-4.0). Generated dictionary data retains CC-BY-SA attribution/terms, including language-pack transformation notices. Preserve all supplied terms, attribution and source obligations. See [NOTICE](../packaging/NOTICE) and each pack's notices. Qt/KDE components retain their licenses. Optional development models have separate terms and are not part of the base release.
