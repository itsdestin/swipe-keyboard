# Setup and recovery details

For download links and short installation steps, see the [main page](../README.md#install). This page describes **v0.4.0-1**: the Deck Flatpak, Arch/CachyOS package, and Fedora x86_64 and ARM64 packages. [The r3 release notes](releases/v0.1.0-r3.md) describe the older, English-only native downloads.

## Verify a download

Download [SHA256SUMS](https://github.com/itsdestin/swipe-keyboard/releases/download/v0.4.0-1/SHA256SUMS) into the same folder as the package. Open a terminal there and run:

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

- **Keyboard update:** download the newer bundle and repeat `flatpak install --user ./swipe-keyboard-0.4.0-1-x86_64.flatpak`. Keep the existing selection unless release instructions say otherwise. The local bundle is not an automatic keyboard-update feed; `flatpak update` can update its runtime but is not a substitute for downloading a new keyboard release.
- **SteamOS update:** use the ordinary supported SteamOS update mechanism. Do not unlock the root filesystem, install this project's native Arch package on SteamOS, or replace system libraries. The [release notes](releases/v0.4.0-1.md) distinguish tested reboot behavior from OS-update survival; do not assume the latter from the former.
- **Removal:** restore the previous entry—or None, if that was the original setting—in **System Settings → Virtual Keyboard**, then run `flatpak uninstall --user org.itsdestin.SwipeKeyboard` without sudo. There is **no automatic selection restoration** for this bundle.
- **Already uninstalled:** select the previous/stock input method manually in Settings. Reinstall the bundle if you want Swipe Keyboard again. Do not delete all KDE keyboard settings to recover one selection.

Private app data is under `~/.var/app/org.itsdestin.SwipeKeyboard/`; the keyboard uses its app-scoped data directory. Uninstalling without `--delete-data` retains that data. You do not need to remove shared Flatpak runtimes.

## Gaming Mode on Steam Deck (preview, first released in v0.4.0-1)

The Desktop Mode bundle above does not change Steam's Gaming Mode keyboard. Gaming Mode support is a separate, opt-in add-on that is built and tested only in a SteamOS virtual machine so far; it has not run on a physical Deck, and physical-Deck performance is not established.

**What it is.** A small background program (the helper) attaches to Steam's own on-screen keyboard through Steam's developer switch and adds swipe typing and a suggestion row; a second program (the engine) does the word guessing. Nothing is sent to the internet: the engine has no network access at all, and the helper's network access exists only to reach Steam on the same Deck. Steam's keyboard keeps working underneath at all times.

**Consent: two separate choices.** `install.sh` (in the SteamOS download) shows its text first, asks, lists the exact changes and asks `[y/N]`; `--dry-run` shows the changes (both choices' plans when none is given) and does nothing.

1. *Gaming Mode on or off.* Pressing Enter, or `--yes` alone, installs the Desktop Mode keyboard only (the main Flatpak): no Steam change, no network change, no password. `--gaming` adds Gaming Mode: the helper Flatpak, Steam's developer-switch file `.cef-enable-remote-debugging` (the record notes whether it already existed, for example from Decky Loader, so removal never deletes a file it did not create), and two user-level systemd units in `~/.config/systemd/user/`. `--desktop-only` on a machine that has Gaming Mode turns it off: units, helper app and our own switch file go, the Desktop keyboard stays. Pressing Enter on a re-run keeps whatever you have.
2. *Only with Gaming Mode: block other devices or leave the door open.* SteamOS ships a system socket, `steam-web-debug-portforward.socket`, that forwards Steam's debug port (8080, this Deck only) to port 8081 on every network interface, and its firewall allows it. `--block-outside` (the default) writes one drop-in file, `/etc/systemd/system/steam-web-debug-portforward.socket.d/swipekb-loopback-only.conf`, which makes that socket listen on `127.0.0.1:8081` only, then restarts the socket and checks `/proc/net/tcp` that nothing is reachable from the network. This is the only step that uses the administrator password (via `sudo`, five commands: `mkdir`, `tee`, `rm`, `rmdir`, `systemctl daemon-reload/restart`), it is done before the switch file is made, and it is undone if the check fails. `--leave-open` makes no system change and asks for no password; the status file then notes the choice. Why this mechanism: `/etc` stays writable on SteamOS and a drop-in there is a single stamped file that is removed by deleting it; a firewall rule would change shared security settings for every program, and masking the socket would remove 8081 entirely, including for local use. Blocking does stop tools that reach Steam from another computer through 8081 until you choose to leave it open.

If you choose to block and no password is given (none set, declined, or the change is refused), nothing about Gaming Mode is enabled silently: an interactive run asks whether to leave the door open or skip Gaming Mode (Enter skips); a scripted run (`--yes`) skips Gaming Mode, finishes the Desktop keyboard and exits with code 3. A re-run with the door already closed asks for no password.

**If the developer switch file disappears.** The helper app runs in a Flatpak sandbox that deliberately cannot see Steam's folder (it holds login files) or Steam's processes, so it cannot notice this itself and it never recreates the file (owner decision: tell the person, do not silently put it back). Gaming Mode adds a small script, `~/.local/share/swipe-keyboard-gaming/switch-watch.sh`, run once a minute by a user timer (`swipekb-switch-watch.timer`/`.service`, also run at login). It only tests that the file exists (never writes in Steam's folder) and writes one word (`ok`, `off`, `restart`, `no-steam`) to `~/.local/state/swipekb-gaming/switch-state`, which the helper turns into its status: `switch-off` ("Steam's developer switch was turned off. Swipe in Gaming Mode is off; Steam's normal keyboard is in use. To turn it back on, run the Swipe Keyboard installer again..."), distinct from `needs-steam-restart`, `steam-not-found` and `waiting-for-steam`. When the file is gone it shows one desktop notification through the notification service (`notify-send` with Turn it back on / Dismiss buttons; without button support it says how to fix it in words), only when a notification service is running, so it appears in Desktop Mode and is retried each minute until it can be shown; once shown it is not repeated until the file has come back and gone again. The button opens Konsole running `switch-watch.sh restore`, which lists the one file it will create, asks, creates only that, and reminds you to restart Steam. Re-running `install.sh` also puts the file back and says it had been turned off. While Steam is still running with the port open, swipe keeps working until Steam's next restart (Steam reads the file only at start).

The choices are recorded in `~/.local/share/swipe-keyboard-gaming/install-record.json` (`gaming`, `outside_access`, `created_system_files`), and the network choice is also written as one word to `~/.local/state/swipekb-gaming/outside-access` for the helper. The installer never touches `/usr` or the read-only image, never changes a Steam file and never restarts Steam while a game runs. `uninstall.sh` and turning Gaming Mode off remove the drop-in only if it is recorded as ours and still carries our stamp; without the password they leave it (harmless) and say so.

**Status.** The helper writes `~/.local/state/swipekb-gaming/status.json`. Meanings: `active` (the layer is on Steam's keyboard), `waiting-for-steam` (Steam is not running, is restarting, or its keyboard page is not up yet), `needs-steam-restart` (Steam is running without the developer switch; restart Steam once), `paused-steam-changed` (the helper gave up after a few failed tries on this Steam version, usually because Steam's keyboard no longer matches what we depend on, so the layer is off and Steam's own keyboard is in use; running `install.sh` again or restarting the device makes it try again), `blocked-outside-access` (you chose to block other devices but Steam's debug port is reachable from the network again, for example after a SteamOS update replaced the block; the layer is off and the fix is to run `install.sh` again), `steam-not-found` (no Steam installation was found), `error` (the helper hit an unexpected problem; Steam's keyboard is unaffected) and `off`. A paused or waiting state never leaves you without a keyboard. The helper re-checks the outside door before it attaches and every 30 seconds (it only reads `/proc/net/tcp`; no network call). If you chose to leave the door open, the layer runs and an `active` status carries the note `Outside access left open by your choice.`

**Fields and privacy.** Inside Steam's own password boxes the layer does nothing at all. Where Steam cannot say what kind of box it is (for example the keyboard opened over a game), swipe and suggestions work but nothing is learned or written to disk, and what is held is erased when the keyboard closes. Gaming Mode does no learning anywhere in the first version.

**Restarts and updates.** Everything lives in your home folder. Designed to survive reboot, switching between Gaming and Desktop Mode, Steam restarts and updates; a reboot and mode-switch survival were measured in a virtual machine, while SteamOS image updates and the installer's real-Flatpak behaviour are not yet verified (see the plan's M8 results when they exist). The one case that may need a Steam restart is Steam forgetting the developer switch.

**Updates and removal.** For now an update is the newer download plus running its `install.sh` again (a downgrade is refused unless `--force`). Removal is `bash uninstall.sh` from the download (`--remove-apps` also removes the two Flatpaks if the installer added them). Because the layer lives only in Steam's memory, nothing remains in Steam after its next restart. If you removed the Flatpaks without running `uninstall.sh`, the two units fail to start harmlessly and stop retrying; `uninstall.sh` still cleans them up.

## Native packages: Arch/CachyOS and Fedora Plasma Mobile

Native packages require KDE Plasma Wayland and **Plasma Keyboard 6.7 or newer**, not Maliit. They install alongside any 6.7+ keyboard and never block a Plasma upgrade; the add-on itself works with 6.7.x/6.8.x, and on 6.9 or later `swipe-keyboard launch` starts KDE's plain keyboard unchanged (and logs one line saying so) until a version made for 6.9 is released. The release provides **Arch/CachyOS x86_64**, **Fedora 44 x86_64** and **Fedora 44 ARM64/aarch64** packages. The package records its build ABI; dependency resolution and `swipe-keyboard check` must both succeed. Do not ignore library dependencies or force a plugin onto an incompatible desktop.

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

Use the complete corresponding-source archive attached to the [v0.4.0-1 release](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.4.0-1) for rebuilding. GitHub's automatic “Source code” ZIP/tar downloads omit submodules. The Fedora `.src.rpm`, when supplied, is for rebuilding, not installation. See [CONTRIBUTING.md](../CONTRIBUTING.md).

Our own trained models (neural next-word `*.nwp.bin`, swipe `swipe-model.bin`; trained and released by Destin Moss via youcoded.ai, 2026) are downloaded by checksum with the other files; the next-word models carry ODC-By 1.0 AND CC BY 2.0 FR, the swipe model MIT (trained on FUTO's MIT swipe data, notice installed as `FUTO-SWIPE-DATA-LICENSE.txt`). A missing or changed file turns off just that model. FUTO's swipe-library is GPL-3.0; model weights use the separate FUTO Model Weights License 1.0. The English dictionary keeps the AOSP word list (Apache-2.0); all dictionaries and word lists are otherwise counted from FineWeb/FineWeb-2 text (ODC-By 1.0) and Tatoeba sentences (CC BY 2.0 FR), so only attribution terms apply and no share-alike licence is involved. Preserve all supplied terms, attribution and source obligations. See [NOTICE](../packaging/NOTICE) and each pack's notices. Qt/KDE components retain their licenses. Optional development models have separate terms and are not part of the base release.
