# Real KDE session acceptance fixtures

These are **test-only fixtures**, not an installer or runtime dependency. Start a new disposable KDE VM only with the owner's approval. Never automate keys/touches against the host desktop.

- `session-client.cpp` is an ordinary Wayland text/password client. It does **not** import SwipeKeyboard. Its private test-account D-Bus service exposes committed normal text and password length, so a pass requires the packaged keyboard to deliver text through the real KWin input-method protocol. No slot injects text: controls only clear the fixture, move focus (including a delayed password transition), or select existing following text. The delayed transition reproduces an application moving focus while a real touchscreen gesture is held.
- `vm-touch.py` adapts touch-profile's existing virtual-touch test. It opens **only the guest kernel's** uinput device, and refuses unless running as guest root on QEMU with `/etc/swipekb-full-vm-acceptance`. Its normal host invocation must exit 2. Do not replace this with host uinput, ydotool, desktop key injection or a default Wayland-display connection.
- The text client also refuses unless the QEMU guest marker exists. Build it against the guest's Qt version; a developer-host binary is not evidence of distro compatibility.

```sh
cmake -S tests/session -B build-session-fixture -G Ninja
cmake --build build-session-fixture --parallel 2
```

The VM requires the supported Plasma Keyboard/Qt baseline, Python evdev for test input, and a normal logged-in KDE Wayland test account. Install/enable the actual native keyboard package as that account; do not load the plugin into this client or replace the keyboard with a drawing.

## Acceptance sequence

1. Record the account's original KWin input method and visibility mode. Check that there is no touch-profile, custom layout or ASUS dependency.
2. Install the native package using the distro package manager; run its compatibility check. Updating an old desktop to the supported KDE/Qt baseline is a separate OS operation, not a promised keyboard-installer feature.
3. Enable as the normal GUI user; touch a normal field. Verify automatic keyboard activation, ordinary tapping, swipe commits, candidate selection/correction, symbols and backspace.
4. Check a password field using synthetic input only. Verify sensitive-input behavior and recording safeguards without logging secret text.
5. Check keyboard restart/package reinstall and an actual VM reboot, then disable and verify exact restoration. Remove the package and confirm stock typing remains available.
6. Preserve evidence and report every unrun case as unverified. Do not advertise broad application/distro/hardware support from one client.

## Reuse the existing tools

Use the workspace's `scripts/vm/vmctl.sh` for guest-console keys, never host key APIs. QEMU's GL scanout has no CPU surface, so its monitor screendump can return `no surface`; capture from **inside the guest** with Spectacle and copy that image out. The keyboard acceptance test must use actual guest screenshots, not re-created UI.

A stripped-down namespace root was unsuitable for the real compositor: KWin's virtual renderer crashed in RenderDevice initialization. Native offscreen package tests remain useful, but do not substitute them for real user-session acceptance. Fedora 44 uses Plasma Login Manager and `plasma-setup` for first-user setup; do not assume an SDDM-only fixture gets a normal GUI login.

VM images, private SSH keys/passwords, guest configuration helpers and screenshots live outside the repository under the maintainer's dedicated verification cache. They must never enter a public source snapshot.
