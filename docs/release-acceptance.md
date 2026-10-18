# Release acceptance: standalone keyboard

A built package is not proof that swipe typing works in a real KDE session. Keep these gates separate. Never run these steps against the maintainer's daily keyboard automatically.

## Candidate targets

- x86_64 Arch/CachyOS with Plasma Keyboard 6.7.x/6.8.x.
- x86_64 Fedora 44 KDE with Plasma Keyboard 6.7.x/6.8.x (build against each Fedora release advertised).
- Qt major/minor must match the package build metadata. A new Qt minor requires a rebuilt and checked package.
- Other desktops, X11 sessions, ARM devices and non-English swipe recognition are not supported by the first package.

## Build and package gate (automatable)

- Fresh recursive source checkout; pinned required model assets pass checksums.
- Native build and all engine/plugin tests pass on the target distro.
- Tests for setup, layout generation and package staging pass.
- Compiled instructions are portable across ordinary x86_64 CPUs, not tuned for the builder.
- Stage the package in a temporary root; check the plugin loads from the staged files, with no build-tree fallback or missing shared libraries.
- Required models, dictionary, their licenses/notices and exact source provenance are present.
- Base install does not download training data or optional ranking models.
- Package install/upgrade/removal never changes a desktop user's KWin configuration as root.
- Verify the package-manager dependency constraints for Plasma/Qt and package update behavior.

## Interactive gate (isolated KDE Wayland account or VM with touch input)

1. Start from a clean account with stock Plasma Keyboard; save the prior input-method and keyboard-mode settings.
2. Install the native package, then run `swipe-keyboard check` and `swipe-keyboard enable` as that user.
3. Select English (US); check ordinary tapping, long-press accents, swiping, candidates, correction, spaces, whole-word deletion and symbols.
4. Switch to a stock non-English locale: tapping/layouts must still work, with no false promise of non-English swipe recognition.
5. Try a native Wayland text field, browser field, terminal and an XWayland app. Record summon/text-input limitations separately from recognition bugs. Password fields must never be recorded, even with recording enabled.
6. Log out/in (and reboot once); confirm the chosen input method persists without changing tablet mode or panels.
7. Upgrade the package, including the tested Plasma/Qt update path. Verify layouts regenerate and both tapping/swiping still work.
8. Run `swipe-keyboard disable`; confirm prior settings are restored. Change a keyboard setting manually after enable and repeat disable: preserve the manual change.
9. Remove the native package after disabling it; stock keyboard must work. Removing without disabling is a separate recovery case, not a supported uninstall recipe.
10. Check incompatibility handling: unknown Plasma version, mismatched Qt, missing payload and a pre-existing custom layout must refuse activation without changing settings. A launch-time incompatibility should fall back to stock tapping rather than run a broken overlay.

## Publication gate

- Audit tracked files and history before changing repository visibility.
- Verify code/model/dictionary redistribution terms and provide complete corresponding source plus licenses where required; do not infer model terms from the code's GPL label.
- Publish only target/version combinations with recorded passing build **and** interactive gates. Failed/unrun cases are unverified, not supported.
- README install/update/removal commands refer to real published packages or clearly identify candidates.
- Provide known issues, release checksums, source links and an accessible issue-report route. Private GitHub issue links do not help outside users.
