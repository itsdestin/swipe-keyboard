# Standalone v1 verification

Status: v0.1.0 native test-candidate verification passed. Firefox physical-edit spacing is an accepted v1 limitation. Downloads and publication status: [v0.1.0 test release](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.1.0).

## Boundaries

All runtime work used disposable distro roots or the explicitly approved, dedicated Fedora KDE VM. Destin's installed keyboard, normal KDE settings and running YouCoded app were not modified. The acceptance VM's original keyboard settings were restored, the final package removed, synthetic touch input stopped and the VM shut down after testing. Private keys, passwords, VM disks, screenshots and logs stay outside the repository.

## Final candidate evidence

Persistent base: `~/.cache/youcoded-z13-packaging/final-v1-output/`.

| Target | Native build | Extracted runtime | Native installation/removal |
|---|---|---|---|
| Arch x86_64, Plasma Keyboard 6.8.0 / Qt 6.12.0 | makepkg exit 0; 15 native suites, 24 packaging tests, 27 integration tests and 6 session-fixture tests pass | Actual stock InputPanel selects swipe method/models; word-backspace/candidate/cursor tests pass | Ordinary-user packaged check and native removal pass in isolated official distro root; stock keyboard remains |
| Fedora 44 KDE x86_64, Plasma Keyboard 6.7.5 / Qt 6.11.2 | rpmbuild exit 0; 15 native suites, 27 integration tests, 6 session-fixture tests pass; packaging suite passes with one expected Arch/makepkg-only skip | Actual stock InputPanel selects swipe method/models; word-backspace/candidate/cursor tests pass | Parent installed the exact finished RPM in a full KDE VM; actual touch/swipe/backspace/candidate undo, original-setting restoration and clean native removal pass; stock keyboard remains |

The parent independently recomputed the final artifact hashes:

- `arch/swipe-keyboard-0.1.0-1-x86_64.pkg.tar.zst`: 12,434,222 bytes; SHA-256 `eae568892973a154c6e03b15d7a5ed8a1eb39d98c1fb9cfb523549082613570c`.
- `fedora44/swipe-keyboard-0.1.0-1.fc44.x86_64.rpm`: 11,744,206 bytes; SHA-256 `492fc77112d6b42dde95d6e3372f64212d17ca4cc8fc5290277e26ba2f5ed8e0`.
- `arch/swipe-keyboard-0.1.0.tar.gz`: 194,380,620 bytes; corresponding frozen recursive source for both; SHA-256 `62e1a7f5158465a8f3792019e06d94dbbb92364201e360c6fe1372cd210b7aac`.
- `fedora44/swipe-keyboard-0.1.0-1.fc44.src.rpm`: 188,973,405 bytes; corresponding source RPM; SHA-256 `ddf7015f389186e1bafe15f77a681fe9ad30b9a1d4ac6762e8237c58dee6267c`.

Recipes, detailed logs, `README-FIRST.md`, `SHA256SUMS` and a machine-readable manifest are retained alongside the outputs. Later documentation-only edits do not change the runtime payload; the frozen source archive is the build's corresponding source.

## Real KDE acceptance

The full Fedora VM used an ordinary `keyboardtest` Wayland account, the stock theme/layout and a persistent guest-kernel touchscreen. The test client does not import the keyboard; committed text travels through real KWin input-method handling. Tests/session fixtures refuse host execution and require a QEMU guest plus explicit marker.

Observed passes include touch-triggered activation, `hi` tapping, sequential `the world` swipes, candidate replacement, symbols, password non-recording/tapping, default-off recording and owner-only opt-in data, exact restoration of absent KWin settings, same-version package reinstall, real reboot and subsequent typing, native Firefox 158 Wayland fields, and 150% scaling with current rendered key centers.

The **exact final RPM** additionally produced `the` then an empty field after first on-screen Backspace; candidate selection produced `the works`, then Backspace restored `the`. Disable restored the original settings exactly; native removal left stock Plasma Keyboard available.

The launcher lock race is fixed with a bounded five-second launch-only wait. Unit contention tests and three real rapid activation cycles passed. Enable/disable user calls remain fail-fast. The old single-character VM result describes an earlier candidate, not final word-backspace behavior.

## Accepted limitation and support boundaries

Firefox physical Ctrl+A/Delete may leave absent or stale surrounding text/cursor information, adding a leading space on the next swipe. The selection-aware helper is not claimed to fix unreported edits. Destin accepted/documented this as minor and non-blocking for v1; do not restart the upstream investigation as release housekeeping. Automatic correction/deletion cannot be guaranteed in apps that misreport context.

No broad non-KDE, X11, ARM, non-English swipe, additional Fedora-release or arbitrary-application support is claimed. Same-version reinstall and real reboot were tested; no future-version updater/feed is implied. The original Fedora release ISO had an older KDE/Qt baseline and needed a normal full OS update first; do not promise the keyboard installer upgrades stale desktops seamlessly.

## Distribution

The owner approved commit/push, public repository visibility and v0.1.0 test-release publication. The technical history/source/privacy checks and attribution review are recorded in [publication audit](publication-audit.md); they found no confirmed credential/private-owner-data blocker and do not constitute legal certification. Public download/source/issue links are provided from README and the release page. No Reddit post is part of this publication task.

A public AUR/COPR package repository or automatic update feed is not configured.

## Logs

Final native logs: `final-v1-output/{arch,fedora44}/build.log`, Fedora `extracted-runtime.log`, Arch `runtime-and-removal.log`, session-fixture logs and `verification-summary.txt`. Final real desktop evidence: `fedora-kde-vm/final-v1-rpm-install.log`, `final-rpm-word-{before,after}.txt`, `final-rpm-candidate-{before,after}.txt`, and `final-v1-rpm-removal.log`. Earlier network and minimal-compositor failures are not final compiler failures.
