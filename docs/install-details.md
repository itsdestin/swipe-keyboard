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

**Gaming Mode is not supported.** Steam's built-in Gaming Mode keyboard is a different integration. Installing the Desktop bundle does not replace it. Physical Steam Deck hardware performance is not established by our virtual-machine checks.

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

## Learning from your typing (off by default)

When switched on, the keyboard remembers, on this computer only: words you use that the language files do not know (names, slang) once you have used them twice; how often you use ordinary words; and which word you usually write after another. Suggestions and swipes then favour them, a little at first and more as the evidence grows; the language files' own order always stays the base. A swear word you typed or swiped yourself may be offered like any other word; one you never used is never offered out of the blue. Use fades with a half-life of about 45 days, and each language keeps a bounded list (4,000 words, 12,000 pairs).

What it never does:

- It does not learn in password, hidden, web-address, e-mail, number, date or phone fields, or wherever the application tells the keyboard not to predict. It uses the same checks as next-word prediction.
- On stock Plasma (where the keyboard cannot see what else is in the field) it learns only words this keyboard itself entered.
- It never sends anything over a network, and writes nothing outside the folder below. Diagnostics show counts only, never words.

Where it lives: `learn-typing` (an empty file; present = on) and `learning/<language>.swklearn` (one small private file per language, mode 0600 in a 0700 folder, written atomically a few seconds after a change) inside the keyboard data folder (`~/.local/share/swipe-keyboard/` natively). A damaged file is ignored and the keyboard starts empty. A typo you type twice looks like a new word to the keyboard and can be offered; press and hold it to remove it.

Choosing:

- `./install.sh` asks **Learn from your typing? [y/N]** when it runs in a terminal. Packages (Arch, Fedora, Flatpak) and installs without a terminal never ask and leave it **off**. `SWIPEKB_LEARN_TYPING=yes|no` answers the question ahead of time.
- Native packages: `swipe-keyboard learning on`, `off`, `forget` or `status`. Installs from `install.sh`: `python3 ~/.local/share/swipe-keyboard/learning.py on|off|forget|status` (the same commands). Or just create or delete the `learn-typing` file; the running keyboard notices within a second.
- **Off** stops learning and also stops using what was learned; the stored files stay until you run `forget`.
- **Forget everything** (`forget`) deletes the learned files at once; a keyboard that is running notices within about a second, drops what it holds in memory and writes nothing back. It is a command rather than an entry in the keyboard's settings-key menu because Qt's menu is a fixed list the keyboard cannot add to.
- **Forget one word**: press and hold a word you taught the keyboard (a name or slang word it did not know) in the suggestion bar and choose **Remove from dictionary**. Everything learned about that word and its pairs is erased, and it is not suggested again until you type it again (it then needs its two uses once more). Ordinary dictionary words show no such menu; to clear your habits with those, use `forget`.

Limits: swipes can only add words made of the language's letters (no apostrophes) to the FUTO dictionary, and that dictionary cannot drop a word until the keyboard restarts, so a forgotten word is filtered from the suggestions instead; the project's own swipe engine rebuilds its word list at the next swipe. The swipe bar that is already showing a word you just removed keeps showing it until your next swipe or key.

## Privacy and stored files

Recognition runs locally. Recording is **off by default**. Sensitive-field cancellation uses the live field hints as well as cached privacy vetoes; actual effectiveness still depends on the application/backend supplying correct input information. Do not interpret one tested configuration as a universal privacy guarantee.

Native paths:

- Installed code, models and dictionaries: `/usr/share/swipe-keyboard/`.
- User options, restoration record and private layout overlay: `$XDG_DATA_HOME/swipe-keyboard/`, normally `~/.local/share/swipe-keyboard/`.

The Flatpak uses app-scoped storage instead of the native paths. Developer recording requires an explicitly created `record` switch file; recordings can contain typed words and context and are not needed for ordinary use or support requests. They are not automatically deleted when removing the package.

Never send passwords, personal typing, learned words (the `learning` folder), recordings or unreviewed logs. Use made-up text. Include the OS, application/version, package version and reproduction; native users can also include `swipe-keyboard check`.

## Source and licenses

Use the complete corresponding-source archive attached to the [v0.3.1-1 release](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.3.1-1) for rebuilding. GitHub's automatic “Source code” ZIP/tar downloads omit submodules. The Fedora `.src.rpm`, when supplied, is for rebuilding, not installation. See [CONTRIBUTING.md](../CONTRIBUTING.md).

Our own trained models (neural next-word `*.nwp.bin`, swipe `swipe-model.bin`; trained and released by Destin Moss via youcoded.ai, 2026) are downloaded by checksum with the other files; the next-word models carry ODC-By 1.0 AND CC BY 2.0 FR, the swipe model MIT (trained on FUTO's MIT swipe data, notice installed as `FUTO-SWIPE-DATA-LICENSE.txt`). A missing or changed file turns off just that model. FUTO's swipe-library is GPL-3.0; model weights use the separate FUTO Model Weights License 1.0. The English dictionary keeps the AOSP word list (Apache-2.0); all dictionaries and word lists are otherwise counted from FineWeb/FineWeb-2 text (ODC-By 1.0) and Tatoeba sentences (CC BY 2.0 FR), so only attribution terms apply and no share-alike licence is involved. Preserve all supplied terms, attribution and source obligations. See [NOTICE](../packaging/NOTICE) and each pack's notices. Qt/KDE components retain their licenses. Optional development models have separate terms and are not part of the base release.
