---
status: active
---
# Automatic updates for every platform (0.4.1)

Owner decisions (2026-10-23): updates install themselves on every platform ("full auto updates could be best across
the board depending on practicality/simplicity"); use each system's own installer; no AUR (new AUR accounts are
closed). Deck: the update installs by itself (was "show in Discover", superseded by full auto).

## What people experience

- Once a day, quietly, the keyboard checks our update site and installs a newer version if there is one. The new
  version takes effect at the next login; a running keyboard is never restarted mid-typing.
- `sudo swipe-keyboard updates off` (Arch, Fedora) or `bash install.sh --no-auto-update` (Deck) stops it;
  `... on` turns it back on.
- 0.4.0 and older never update themselves: each person installs 0.4.1 by hand once.
- A bad release reaches everyone within about a day. The owner's approval of each release is the safety gate;
  rollback is a new, higher version built from the old source.

## One update site

GitHub Pages (`https://itsdestin.github.io/swipe-keyboard/`), deployed by the release workflow from a fresh build
each release (no branch, no history growth): only the newest release is on it. Contents:

| Path | For | Integrity |
|---|---|---|
| `repo/` | Flatpak (OSTree) repository with both Deck apps | every commit and the summary signed with the update key |
| `fedora/<arch>/` | dnf repository (both RPMs, `repodata/`) | `repomd.xml` signed (`repo_gpgcheck=1`); package hashes are inside the signed metadata |
| `arch/latest.json` + `.sig` | Arch updater | detached signature; holds the package URL, version and SHA256 |
| `swipe-keyboard.gpg` | everyone | the public update key (also shipped inside every package) |

No static Flatpak deltas: a recent libostree refuses large delta parts, and a full update is about 30 MB.
RPMs and the Arch package are the exact released bytes (not re-signed), so release hashes still match.

## Per platform

- **Deck** (both modes; `install.sh` is now the only Deck install path, Desktop-only stays the default): after
  installing the bundles, `install.sh` points their Flatpak origin at `repo/` with the bundled public key
  (`flatpak remote-modify`), and installs a user timer `swipekb-update.timer` that runs
  `flatpak update --user --noninteractive` for our apps once a day (randomised). When the Gaming Mode helper
  changed, the helper and engine units are restarted after the update. No password, no system change.
- **Fedora**: the RPM ships `/etc/yum.repos.d/swipe-keyboard.repo`, the public key, and a system timer
  `swipe-keyboard-update.timer` (enabled by a packaged `timers.target.wants` link, so no install scriptlet) that runs
  `dnf -y upgrade --refresh swipe-keyboard`.
- **Arch**: the package ships the same timer; its service runs `swipe-keyboard-update`, which fetches
  `arch/latest.json`, checks the signature with the bundled key (`gpgv`), checks the version is newer
  (`vercmp`) and the downloaded file's SHA256, then `pacman -U --noconfirm` that one file. It never syncs or
  upgrades anything else.

## Owner one-time steps

1. Signing key: generated on the owner's laptop, private key backed up to the USB and stored as the secret
   `FLATPAK_REPO_GPG_KEY` (+ variable `FLATPAK_REPO_GPG_KEY_ID`); public key committed to the repository.
2. Settings, Pages, Source: GitHub Actions. Then variable `FLATPAK_REPO_PUBLISH=true`.

## Verification before 0.4.1 ships

- Unit tests for the updater, units, recipes and the workflow job.
- Rehearsal with a throwaway key and a local copy of the site: SteamOS VM (Desktop Mode) and Fedora VM install the
  0.4.1 candidate, then are offered a throwaway 0.4.2 and must install it by themselves. Arch updater logic is
  tested in a container (no Arch VM).

## Rehearsal results (2026-10-23)

Real CI-built 0.4.1 and a throwaway 0.4.2 (never published), a local copy of the site signed with the real key.
- **SteamOS VM (Flatpak 1.16.6):** install.sh 0.4.1 (Gaming Mode on), then the real `swipekb-update.service`: both
  apps 0.4.1 -> 0.4.2 (`runtime.json` 0.4.2-1), engine and helper restarted onto it, a second run "up to date". Found
  and fixed: the helper's bundle origin remote was already gone after install (now recreated); one refused app
  aborted the other (now one at a time). Flatpak refuses a commit with an older timestamp: real releases are built
  in order, the rehearsal re-stamped its parallel-built helper.
- **Laptop, isolated Flatpak 1.18.4:** right key updates, wrong key refused (listing fails, app unchanged); the
  pre-review command without `--gpg-verify` installed a wrongly signed update.
- **Fedora 44 VM (dnf5 5.4.6):** RPM 0.4.1, then the real `swipe-keyboard-update.service`: 0.4.1 -> 0.4.2. Wrong key on
  repomd.xml: "GPG signature verification error", nothing installed. Tampered package: "checksum doesn't match",
  nothing installed. Installing the RPM while the site does not exist yet works (skip_if_unavailable).
- **Arch (laptop, real gpgv and vercmp, stand-in pacman):** signed index -> exactly `pacman -U` of the 0.4.2 file;
  wrong key -> refused, nothing installed.

Still unverified: GitHub Pages serving the live site (first seen at the 0.4.1 release).

## Independent review (2026-10-23) and what changed

Fixed: Deck origin remotes now get `--gpg-verify` (importing the key alone left signature checks off on a bundle's
origin); the site verifier installs the real bundle, runs install.sh's exact command and requires the update to pass
with our key and fail with another; timers use `OnCalendar=daily` (a since-last-run timer stopped for good after an
"off" day); 30-minute unit limits and a whole-download deadline; an unreachable site is a quiet "try tomorrow", not a
failed unit; malformed signed indexes are refused without a crash; root updater ignores environment overrides unless
`SWIPEKB_UPDATE_TESTING=1`, runs `python3 -I` with PrivateTmp/ProtectHome=read-only; pacman's LocalFileSigLevel
refusal is explained.

Known limits, accepted for now:
- **Replay/freeze:** the signed indexes carry no expiry, so someone in the network path could keep serving an older
  signed index and hold a machine on an older version (never downgrade: versions only go up). Needs a scheduled
  re-signing job to fix; not built.
- **Key rotation:** every installed copy trusts one key, and gpgv ignores revocation. Rotating means shipping a release
  whose keyring holds the old and new key, then signing with the new one. A leaked key cannot be revoked for users
  who never update.
- **Gaming Mode restart:** after an update the engine and helper restart once, which can interrupt a word being typed
  at that moment in Gaming Mode.
- **First check:** native timers start at the next boot (no install scriptlet); until then nothing is checked.
- **Secret scope:** `FLATPAK_REPO_GPG_KEY` must be an environment secret of `keyboard-release`, not a repository
  secret, so only the approved publish job can read it.

