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

## Unverified until then

Pages serving the OSTree layout; dnf5 `repo_gpgcheck` on Fedora 44; the helper restart after a Flatpak update.
