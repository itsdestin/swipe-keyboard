# Setup and recovery details

For download links and short installation steps, see the [main page](../README.md#install). This page describes **v0.3.1-1**: the Deck Flatpak, Arch/CachyOS package, and Fedora x86_64 and ARM64 packages. [The r3 release notes](releases/v0.1.0-r3.md) describe the older, English-only native downloads.

## Verify a download

Download [SHA256SUMS](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.3.1-1/SHA256SUMS) into the same folder as the package. Open a terminal there and run:

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

- **Keyboard update:** download the newer bundle and repeat `flatpak install --user ./swipe-keyboard-0.3.1-1-x86_64.flatpak`. Keep the existing selection unless release instructions say otherwise. The local bundle is not an automatic keyboard-update feed; `flatpak update` can update its runtime but is not a substitute for downloading a new keyboard release.
- **SteamOS update:** use the ordinary supported SteamOS update mechanism. Do not unlock the root filesystem, install this project's native Arch package on SteamOS, or replace system libraries. The [release notes](releases/v0.3.1-1.md) distinguish tested reboot behavior from OS-update survival; do not assume the latter from the former.
- **Removal:** restore the previous entry—or None, if that was the original setting—in **System Settings → Virtual Keyboard**, then run `flatpak uninstall --user org.itsdestin.SwipeKeyboard` without sudo. There is **no automatic selection restoration** for this bundle.
- **Already uninstalled:** select the previous/stock input method manually in Settings. Reinstall the bundle if you want Swipe Keyboard again. Do not delete all KDE keyboard settings to recover one selection.

Private app data is under `~/.var/app/org.itsdestin.SwipeKeyboard/`; the keyboard uses its app-scoped data directory. Uninstalling without `--delete-data` retains that data. You do not need to remove shared Flatpak runtimes.

## Gaming Mode on Steam Deck (preview; not in any published release)

The Desktop Mode bundle above does not change Steam's Gaming Mode keyboard. Gaming Mode support is a separate, opt-in add-on that is built and tested only in a SteamOS virtual machine so far; it has not run on a physical Deck, and physical-Deck performance is not established.

**What it is.** A small background program (the helper) attaches to Steam's own on-screen keyboard through Steam's developer switch and adds swipe typing and a suggestion row; a second program (the engine) does the word guessing. Nothing is sent to the internet: the engine has no network access at all, and the helper's network access exists only to reach Steam on the same Deck. Steam's keyboard keeps working underneath at all times.

**Consent and what the installer changes.** `install.sh` (in the SteamOS download) shows its text first and asks `[y/N]`; `--dry-run` shows the changes and does nothing. It installs the two Flatpaks (the main app and the Gaming Mode helper), creates Steam's developer-switch file `.cef-enable-remote-debugging` (it records whether the file already existed, for example from Decky Loader, so removal never deletes a file it did not create), writes two user-level systemd units to `~/.config/systemd/user/`, and records everything in `~/.local/share/swipe-keyboard-gaming/install-record.json`. It never uses `sudo`, never touches `/usr` or `/etc`, never changes a Steam file, and never restarts Steam while a game runs. The developer switch also exposes Steam to other devices on the same network through SteamOS's port 8081; the consent text says so.

**Status.** The helper writes `~/.local/state/swipekb-gaming/status.json`. Meanings: `active` (the layer is on Steam's keyboard), `waiting-for-steam` (Steam is not running, is restarting, or its keyboard page is not up yet), `needs-steam-restart` (Steam is running without the developer switch; restart Steam once), `paused-steam-changed` (Steam's keyboard no longer matches what we depend on, so the layer is off and Steam's own keyboard is in use), `steam-not-found` (no Steam installation was found), `error` (the helper hit an unexpected problem; Steam's keyboard is unaffected) and `off`. A paused or waiting state never leaves you without a keyboard.

**Fields and privacy.** Inside Steam's own password boxes the layer does nothing at all. Where Steam cannot say what kind of box it is (for example the keyboard opened over a game), swipe and suggestions work but nothing is learned or written to disk, and what is held is erased when the keyboard closes. Gaming Mode does no learning anywhere in the first version.

**Restarts and updates.** Everything lives in your home folder. Designed to survive reboot, switching between Gaming and Desktop Mode, Steam restarts and updates; a reboot and mode-switch survival were measured in a virtual machine, while SteamOS image updates and the installer's real-Flatpak behaviour are not yet verified (see the plan's M8 results when they exist). The one case that may need a Steam restart is Steam forgetting the developer switch.

**Updates and removal.** For now an update is the newer download plus running its `install.sh` again (a downgrade is refused unless `--force`). Removal is `bash uninstall.sh` from the download (`--remove-apps` also removes the two Flatpaks if the installer added them). Because the layer lives only in Steam's memory, nothing remains in Steam after its next restart. If you removed the Flatpaks without running `uninstall.sh`, the two units fail to start harmlessly and stop retrying; `uninstall.sh` still cleans them up.

## Native packages: Arch/CachyOS and Fedora Plasma Mobile

Native packages require KDE Plasma Wayland and **Plasma Keyboard 6.7.x/6.8.x**, not Maliit. The release provides **Arch/CachyOS x86_64**, **Fedora 44 x86_64** and **Fedora 44 ARM64/aarch64** packages. The package records its build ABI; dependency resolution and `swipe-keyboard check` must both succeed. Do not ignore library dependencies or force a plugin onto an incompatible desktop.

For Plasma Mobile on ARM64, use the **aarch64 Fedora RPM** only on a compatible Fedora 44/glibc system. `uname -m` reports the CPU architecture. It is not an ARM32, Android/iOS or postmarketOS/Alpine APK package; other distributions, especially musl-based ones, need their own builds. ARM64 is a required release target and needs its own build/load and Plasma Mobile input acceptance, not an inference from x86 testing. Emulated ARM checks do not establish physical-phone performance, battery use or touch ergonomics.

`swipe-keyboard check` reports versions, session and selection, then checks native loading before activation changes settings. It is not a touchscreen or universal application-compatibility test. Run both `check` and `enable` inside the logged-in KDE session, as the desktop user and without sudo.

Default activation changes only the input method; it does **not** write `VirtualKeyboardMode`, change the live mode, or capture/modify a tablet manager's desktop/tablet baseline. Disable likewise leaves visibility alone for this policy-preserving activation. This respects convertible mode 0 with a physical keyboard, tablet mode 1, and existing touch-only policies. If visibility changes later through your manager or KDE Settings, those changes remain authoritative.

For an explicit always-available preference, use `enable --always-available` on initial activation. This separately opts into mode 1; setup records ownership for guarded restoration but never edits an external tablet manager's configuration. Already-enabled setups must disable before opting into a different policy; no old journal is silently reinterpreted. Older records retain their original provenance safeguards, so this update does not automatically reverse an already repaired device or invent a missing baseline.

Before activation, `enable` displays the effective current and proposed input-method settings and whether visibility is **preserved** or explicitly changed, explains likely suggestion/Backspace differences, then asks **[y/N]**. Declining, an empty response, or a noninteractive invocation without `--yes` leaves settings untouched. Automated callers may pass `enable --yes` as explicit consent; compatibility and ownership checks still apply. Normal compositor launches never prompt. The explicit `disable` command remains the restoration action and retains its ownership safeguards.

Native packages keep the distro's Plasma Keyboard backend. On the supported stock backend, next-word prediction uses up to 20 confirmed words entered through this keyboard, held only in memory. Preexisting or pasted editor text is not fed to the native prediction model. Bounded surrounding text is still read for exact insertion/replacement validation after live and cached field checks.

Stock Plasma Keyboard hides some application completion preferences, so native mode cannot independently honor every raw completion opt-out or distinguish every name/terminal purpose. Password, sensitive, restricted, selected and composing fields remain blocked by the available public checks. Diagnostics identify `keyboard-owned-history` and leave raw policy unknown; they do not manufacture affirmative application permission. The bundled Deck backend supplies a raw-policy bridge and retains its stricter application-policy checks. Any present bridge denial or invalid bridge never falls through to native history. Do not patch system backend files to obtain predictions.

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
- Next-word suggestions are available in English, Spanish, French and German after a confirmed tapped or swiped word. With three valid suggestions, the strongest is centered, with dividers only between words. Native mode uses only confirmed keyboard-owned words as model input, as described above; no profile silently falls back to English.
- Spanish (Spain/Mexico), French and German use small statistical models. These are early baselines: recommendations can be repetitive or less natural than English. Spain and Mexico share the same Spanish model; region-specific prediction quality is not claimed.
- While tapping an unfinished word, completions and spelling corrections use the selected language's dictionary. Your literal spelling remains selectable; only tapping a suggestion replaces it. Space never autocorrects the word. Selecting a suggestion adds a space after it.
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

Use the complete corresponding-source archive attached to the [v0.3.1-1 release](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.3.1-1) for rebuilding. GitHub's automatic “Source code” ZIP/tar downloads omit submodules. The Fedora `.src.rpm`, when supplied, is for rebuilding, not installation. See [CONTRIBUTING.md](../CONTRIBUTING.md).

FUTO's swipe-library is GPL-3.0; model weights use the separate FUTO Model Weights License 1.0. Dictionaries incorporate AOSP word lists (Apache-2.0) and wordfreq data (CC-BY-SA-4.0). Generated dictionary data retains CC-BY-SA attribution/terms, including language-pack transformation notices. Preserve all supplied terms, attribution and source obligations. See [NOTICE](../packaging/NOTICE) and each pack's notices. Qt/KDE components retain their licenses. Optional development models have separate terms and are not part of the base release.
