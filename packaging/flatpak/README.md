# Task D1 private Desktop build candidate

This is an isolated **build candidate**, not SteamOS Desktop input acceptance,
Gaming support, a physical Deck performance result, or OS-update survival proof.
Do not install/activate it on the development host. Task D2 must test the exact
built artifact in the protected installed SteamOS guest.

## Pinned build

Use only the owned Fedora builder. Install `flatpak-builder` there with normal
signature verification. Install the signed user KDE SDK/Platform 6.10 refs and
check their exact commits against `runtime-lock.json`. `build.py` fails closed
on a different SDK/runtime commit; merely putting a commit property in a manifest
is insufficient with builder 1.4.12, which replaces it with the installed ref.
An updated runtime is a new candidate requiring a new lock and verification.

The manifest deliberately uses a local, SHA-locked `swipe-source.tar.xz` beside it.
It captures this worktree's accepted staged native fixes, D1 tooling, recursive
pinned source, and verified models/dictionary/licenses, **not** mutable host build
objects, virtualenvs or symlinks to them. The private D1 report identifies that
archive and its per-file hash inventory. It is not checked into Git. Preserve
that exact archive to reproduce this candidate; a source edit requires a new
archive digest. All external archive/wheel inputs have SHA-256 pins.

With the archive, manifest, `build.py`, and `runtime-lock.json` together inside
`~/platform-168-flatpak/bundle` in the builder:

```sh
cd ~/platform-168-flatpak
python3 bundle/build.py build --repo repo
flatpak build-bundle repo org.itsdestin.SwipeKeyboard.flatpak org.itsdestin.SwipeKeyboard master --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo
```

The build uses four jobs (one flat bound on the 4-CPU hosted runner), `/app`, Python 3.13-specific hash-locked dependencies,
and rebuilds ExecuTorch/engine/plugin/helpers against SDK Qt 6.10.3/glibc 2.42.
The 3.14 native build path remains unchanged by default. Only hash-verified
`torchgen` Python/templates are extracted from the existing torch wheel; torch
and CPython 3.14 native extensions are never installed into the SDK environment.
The Qt Virtual Keyboard source is **6.10.3**, matching the observed SDK Qt patch
version, rather than blindly reusing the upstream precedent's 6.10.2 archive.
Backend/libplasma are stable 6.7.5 peeled commits, not branch/nightly references.
The bundle explicitly adds `/app/lib` and the observed SDK's private
`/app/lib/x86_64-linux-gnu` to its Swipe ELF library lookup, and `/app/lib/qml`
to its isolated loader-test imports. Native builds keep `$ORIGIN`-only library
lookup and their original import defaults. The bundle-only library option fails
outside the fixed `/app` prefix; no global `LD_LIBRARY_PATH` or host-library
replacement is used.

## Permissions and manual selection contract

Only `--socket=wayland` and `--socket=inherit-wayland-socket` are requested.
Both are necessary: Flatpak 1.16.6 `common/flatpak-run-sockets.c:184-189` passes
the ordinary Wayland permission as `allowed`, and `flatpak-run-wayland.c:260-264`
unsets `WAYLAND_SOCKET` and returns early without it. Lines 316-326 preserve/pass
the FD only with the inheritance permission. A synthetic socketpair test in the
owned Fedora builder verifies this behavior without connecting to a compositor.
No runtime network, home/host filesystem, host shell, system bus, all-device,
evdev/uinput, DRI, X11, or shared IPC permission is requested. Sound/haptic build
features are disabled. Recording remains default OFF in the shared native code.

The exported desktop entry is a KDE virtual-keyboard entry, not an enable/disable
controller. Before a guarded D2 install/selection, record the previous selection
(including no selection). Select the candidate manually through normal KDE
Settings. **Before uninstalling, manually select that recorded previous entry
or no keyboard again.** There is no tested automatic restoration. Native setup
commands are removed from the bundle. The launcher refuses non-Flatpak execution,
missing inherited sockets, and native enable/disable arguments. It generates a
private overlay under app-scoped data and preserves KWin's inherited FD.

Stock SteamOS root protection, Qt/Plasma libraries, pacman and existing input
method configuration must stay unchanged. Do not relax dependencies or replace
host libraries to make this candidate run.
