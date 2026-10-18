# Revision 3 compatibility acceptance

Status: final native-package acceptance complete for v0.1.0-r3 (package revision 0.1.0-3). The maintainer device installed and activated that exact Arch package without an OS update. Installed payload, live system-plugin loading, recording-off default and preserved rollback state were verified; human touchscreen feedback is separate from the automated evidence.

## Design actually implemented

- Direct ELF dependency generation replaces recursive `ldd` build-machine version stamping. Duplicate providers, unrelated transitive floors and fakeroot injection are removed; actual Qt/GLIBC/GLIBCXX/CXXABI requirements remain.
- The module uses `NO_CACHEGEN` to remove private, Qt-minor-specific generated QML imports. A final-ELF test prevents reintroducing them.
- A Core-only native preflight uses eager `QPluginLoader` binding without instantiating a keyboard, GUI, QML engine or recognition models. Setup refuses missing libraries/symbols before changing settings.
- The extension may run on the same Qt major at or above its build-minor baseline after preflight. This does not authorize mixing the distro's upstream Plasma Keyboard executable with mismatched private Qt libraries.

## Verified package acceptance

- Independent host-native check: all 18 CTest suites pass, including public Qt ABI, successful preflight, failure on missing shared libraries/unresolved symbols and stock InputPanel integration.
- Python suites: 32 setup/overlay tests, 31 packaging tests and 6 guest-input safety tests pass. A source review's future-only CXXABI_1.3.16 concern was fixed by explicitly rejecting the RISC-V-only tag for this x86_64 package, with an observed red → green regression test.
- Qt 6.11-built no-AOT experiment passes stock InputPanel recognition/typing/undo and safety tests on the newer Qt 6.12 host. The original Qt 6.11 AOT build fails on the same runtime with a private-Qt symbol-version error.
- Fedora revision-3 RPM builds successfully using the normal native RPM stages and unchanged recognition-engine cache. The frozen source is overlaid before a normal `-ba --noprep` run; this is an incremental build, not a claimed fresh engine rebuild. The resulting RPM has no ShortCircuited or private-Qt requirements.
- That exact revision-3 RPM was installed with `dnf`, checked and enabled as the ordinary KDE user. Actual guest touchscreen input through KWin verified focus-to-password cancellation (password remains empty), display-scale interruption (no commit, next tap works), and an overlong held gesture (no commit, next tap works).
- As with revision 2, initial focus synchronization can cause conservative one-character Backspace fallback. Settled whole-word/candidate undo is checked separately rather than hiding the first observation.

## Same-byte compatibility matrix

| Environment | Qt | KWindowSystem | Plasma Keyboard | glibc | Scope |
|---|---|---|---|---|---|
| Older Arch build/runtime baseline (2026/07/01) | 6.11.1-1 | 6.27.0-1 | 6.7.2-1 | 2.43+r37 | Native build/tests; extracted stock InputPanel; install/check/remove |
| Current Arch, after full disposable-container upgrade | 6.12.0-3 | 6.31.0-1 | 6.8.0-1 | 2.44+r50 | The **same archive**: extracted stock InputPanel; install/check/remove; before/after hashes identical |
| Maintainer CachyOS device, unchanged installed libraries | 6.12.0-2 | 6.30.0-1.1 | 6.7.5-1.1 | 2.44+r50 | Exact archive `pacman -T` and isolated stock InputPanel swipe/safety tests pass. Native package installed: 0 altered package files; all 96 installed payload files match the tested archive. KWin's live keyboard loads the system plugin, recording is off and legacy files/settings are preserved. Human gesture feedback is not claimed by these checks |
| Fedora 44 full KDE VM | 6.11.2 | Distro-installed KF6 | 6.7.5 | Distro glibc | Exact final native RPM install/check/enable, real touchscreen swipe/candidate undo, privacy/interruption tests and ordinary KWrite typing/saving |

The final Arch archive's dependency check reports every requirement already satisfied on the device. No forced dependencies or device OS upgrade were used. Real KWrite typing with the final Fedora RPM saved `The world`. In one startup trace the first recognition was `tee`; first Backspace still removed the complete recognized word. This is not a claim of perfect word recognition. Candidate `the works` then Backspace restored `the`; a held-gesture password transition left both fields empty.

The long-lived disposable build container exhausted its PID limit with unreaped GPG children during a second downgrade/upgrade cycle. Restarting only that container, initializing/populating its local pacman keyring, and normally updating `archlinux-keyring` completed the final current-runtime check. Repository signatures remained required; no signature/dependency checks were disabled. Disposable build-container init/keyring lifecycle hardening is follow-up work, not a change to the user's OS or package-signing policy.

## Final frozen artifacts

| File | Bytes | SHA-256 |
|---|---:|---|
| `swipe-keyboard-0.1.0-3-x86_64.pkg.tar.zst` | 12360740 | `1e1a699a011945f7187072834efd36a4eca6d390d6971e3b8710d954f3390233` |
| `swipe-keyboard-0.1.0-3.fc44.x86_64.rpm` | 11695937 | `652726eacc5366d864a8f75e135704331d983939e139aac13b687a7895db5de3` |
| `swipe-keyboard-0.1.0-3.fc44.src.rpm` | 188981085 | `3fc932809d7d5837a5ad6d8b574c12659bd3ed5183eb0c98ea5bb388c6689136` |
| `swipe-keyboard-0.1.0.tar.gz` | 194430047 | `4ba92f043bd02d06f806fec8feb280741670c4f21ba6cd57b5dbb7cdf161d551` |

Frozen source includes recursive dependencies and the runtime/build-recipe changes used for these packages. Later release-documentation edits do not alter the compiled payload. GitHub's automatic source ZIP/tar omits submodules and is not the complete corresponding source.

## Scope and follow-up

No full Arch compositor, arbitrary old distro, Plasma Keyboard 6.6, Maliit, ARM or non-English swipe compatibility is implied by these checks. Browser-specific acceptance, portrait rotation and interrupted multi-touch were not separately rerun for revision 3; the known Firefox physical-clear spacing limitation remains accepted. Reinstall/reboot/exact restoration/removal were previously verified for revision 2; revision 3 repeated native install/load/removal on both Arch runtimes and was installed/restarted in the full Fedora VM. There is no automatic update feed or AUR/COPR repository in this release. Maintenance automation is explicitly a post-launch follow-up, not a launch blocker.
