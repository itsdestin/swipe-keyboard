# More KDE distributions: Kubuntu, Debian, openSUSE, Bazzite (card #502)

Owner decisions, 2026-10-30:
- **Regular packages** for the newest versions; the **Flatpak** only for older long-term versions.
- **Bazzite** gets the Steam Deck download (Flatpak plus Gaming Mode helper), not a layered Fedora package.
- Built and tested now, **published only after the OpenSwipe rename** (#501), so these users never meet the old name.
- Hands-on tests run in virtual machines on the project's desktop test machine, never on a maintainer's daily laptop.

## What users get

| Distribution | KDE keyboard / Qt (checked 2026-10-30 in each distro's own package index) | How they install |
|---|---|---|
| Kubuntu 26.10 | plasma-keyboard 6.7.5 | `.deb` + our apt source |
| Debian testing (forky) | plasma-keyboard 6.7.4, Qt 6.11.2 | `.deb` + our apt source |
| openSUSE Tumbleweed | plasma6-keyboard 6.8.1, Qt 6.12.0 | `.rpm` + our zypper source |
| Bazzite (Fedora based, Plasma 6.7) | built into the image | Steam Deck download: `install.sh` |
| Kubuntu 26.04, Debian 13, openSUSE Leap 16 | 6.6 / none in stable / 6.4: too old for a regular package | Flatpak (step 4, after the edge cases below) |

All regular targets are inside the range the 6.7/6.8 front end already supports, so no keyboard code changes; the work is
packaging, updates, CI and testing. Every package keeps today's layout: the add-on, its libraries and data live under
`/usr/share/swipe-keyboard/` with `$ORIGIN`-only library lookup; `/usr/bin/swipe-keyboard` is the only file outside it
apart from the update units and the package-manager source file.

## Steps, in order

1. **Debian package** (`packaging/debian/`), one recipe built twice: in `ubuntu:26.10` and in `debian:testing`.
   Dependencies name the distro's own package names (`plasma-keyboard`, `qml6-module-qtquick-virtualkeyboard` or its
   equivalent, KF6 Kirigami); the exact names are read from each distro's index during the build, not guessed. The
   Qt dependency pins the **Qt minor** the package was built with (Qt plugins load only on the same minor or newer, and
   QML ahead-of-time code needs the same minor; `docs/compatibility-investigation.md`).
2. **openSUSE package** (`packaging/opensuse/swipe-keyboard.spec`), built in `opensuse/tumbleweed`; same rules.
3. **Bazzite**: the SteamOS download's installer recognises Bazzite (`/etc/os-release` `ID=bazzite`) and installs the
   Desktop Mode Flatpak and the Gaming Mode helper the same way as on a Deck. What differs on Bazzite (Steam's location,
   the Gaming Mode session, the administrator password step) is found in the Bazzite VM first and handled explicitly.
4. **Flatpak on older long-term versions**, after the edge cases below have answers and tests.

## Updates

Same behaviour as Arch and Fedora today: a daily timer updates **only this package**, from a signed source the package
installs, and never upgrades the rest of the system (`integration/update.py`).
- **apt:** the package installs `/etc/apt/sources.list.d/swipe-keyboard.sources` with a `Signed-By` key file (the
  update key in `packaging/updates/`). The update site gains a signed apt repository per target (`InRelease`).
  The updater runs `apt-get update` limited to that one source, then `apt-get install --only-upgrade swipe-keyboard`.
- **zypper:** the package installs `/etc/zypp/repos.d/swipe-keyboard.repo` (signed rpm-md, `repomd.xml.asc`, same key).
  The updater runs `zypper --non-interactive update swipe-keyboard`.
- `update.py`'s `manager()` learns `apt` and `zypper`; a copy installed by neither is never updated automatically
  (as today).

## When the distro moves faster than we do

Debian testing and Tumbleweed update Qt and KDE often. A Qt minor update can stop our add-on from loading until we rebuild.
- The launcher already falls back to KDE's plain keyboard when the add-on does not fit; typing never stops.
- A daily CI job (like `arch-current`) builds and loads the package on today's image of each fast-moving distro and
  fails loudly when a new build is needed, before most users install the distro update.
- Where the distro's package manager can express it, the package declares the Qt minor it needs, so a Qt update that
  breaks it is visible at upgrade time rather than at the next login.

## Flatpak on desktops: edge cases (step 4; each needs a handling and a test before it is offered)

1. **Removed while it is the chosen keyboard.** The user must never be left with no on-screen keyboard. Removing it
   restores the previous keyboard; where Flatpak gives no removal hook, the next login detects the missing app and
   switches back to KDE's keyboard.
2. **KDE too old or missing what the app needs.** The app checks before it can be chosen and says so plainly; it never
   leaves a blank keyboard.
3. **The distro later updates KDE and something breaks:** same rule, KDE's own keyboard comes back.
4. **Two install methods on one machine** (Flatpak and a regular package): the installer refuses the second and says why.
5. **First download size:** the KDE Flatpak runtime is several hundred MB when not already present; the instructions and
   installer say so before downloading.
6. **Kubuntu ships without Flatpak:** the instructions include the one-time setup.
7. **Learned words** live in the app's own folder; moving to the regular package later copies them across.
8. **Looks:** the app does not read KDE's global settings file (privacy). Colours and light/dark mode are checked on each
   distro; any mismatch is fixed without widening what the app can read.
9. **Updates** come from our own Flatpak source; the instructions say where they show up.

## Testing

**Automatic (GitHub, every change):** each package builds in its distro's official image and runs the keyboard's tests
there. New: install the finished package into a fresh image of that distro and check the add-on loads (no missing
library, no build-tree fallback). Daily: the fast-moving images, as above.

**Hands-on (one VM per distro, on the desktop test machine):**
1. Install from the update source as a user would.
2. Choose it in System Settings → Virtual Keyboard.
3. Swipe and tap in Firefox, Chrome and a terminal.
4. It appears by itself in tablet mode.
5. Log out and in, then reboot.
6. Install an update.
7. Uninstall it; KDE's own keyboard is back.

Bazzite: also Gaming Mode, as with the virtual Steam Deck. Real Bazzite handhelds stay untested until someone tries one.

## Not in this card

GNOME (#503). ARM builds for these distros. Ubuntu itself (GNOME) and other Ubuntu flavours. Any new keyboard feature.
