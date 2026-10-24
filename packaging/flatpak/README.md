# Flatpak (Steam Deck Desktop Mode)

This folder builds the Steam Deck app `org.itsdestin.SwipeKeyboard`. For installing and using it, see the [main README](../../README.md#steam-deck); this page is for people building or reviewing the package.

## What it is

A self-contained Plasma Keyboard with Swipe Keyboard in it, on the KDE Flatpak runtime (`org.kde.Platform` 6.10). It does not replace any SteamOS library or touch the read-only system. Installing it does not select it: the person picks it in **System Settings → Virtual Keyboard**, and must pick their previous keyboard again before removing it (there is no automatic restore for the Flatpak).

## Build

The supported build is CI's: `.github/workflows/packages.yml`, job `flatpak`, which runs `packaging/flatpak/build.py flatpak-build` with `flatpak-builder` 1.16 or newer on `org.itsdestin.SwipeKeyboard.json`. Follow the commands in that job to build by hand; they are not repeated here so they cannot go stale.

- The KDE SDK and Platform runtimes must be the exact commits in `runtime-lock.json`; `build.py` refuses any other (a commit written in the manifest alone is not enough, because the builder replaces it with the installed one). A runtime update means a new lock and a new round of testing.
- Every downloaded input is pinned by SHA-256. The Qt Virtual Keyboard source is 6.10.3 to match the runtime's Qt; the Plasma Keyboard backend and libplasma are pinned KDE source commits.
- The bundle looks for its libraries in `/app/lib` only (no global `LD_LIBRARY_PATH`, no host libraries). Native packages keep their `$ORIGIN`-only lookup.

## Permissions

The app asks for the Wayland socket (`--socket=wayland` and `--socket=inherit-wayland-socket`, both needed so KWin can hand the keyboard its privileged connection), one private folder under the runtime directory (`xdg-run/swipekb`) and it hides KDE's global settings file and three desktop-settings bus names from the app. It asks for **no** network, home or host filesystem, shell, system bus, device or X11 access. Do not add overrides to make an unsupported application work.

The launcher refuses to run outside a Flatpak, without the inherited socket, or with the native `enable`/`disable` arguments. It builds a private layout overlay in the app's own data folder. Recording stays off by default.

## Licences

The Flatpak installs the same licence folder as the native packages (`/app/share/licenses/swipe-keyboard/`, including `LICENSES/`): MIT for the engine, tools and launcher, `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL` for the Qt Virtual Keyboard plugin. Bundled Qt and KDE components keep their own licences, installed beside it.
