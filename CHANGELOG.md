# Changes

## Unreleased

- Slide along the Space bar to move the text cursor in the direction your finger moves: one character at a time when sliding slowly, up to five times further when sliding fast, like a touchpad (also in right-to-left text, where it follows the screen, not the reading order). A small wobble or a normal tap still types a space; once the slide starts, no space is typed. Works with touch and mouse, in every language layout, and in password fields (only the cursor moves). Moving the cursor also clears the current word's suggestions so a later suggestion tap cannot replace the wrong text. Not available on the `?123` symbols page, and there is no setting to turn it off yet.

## v0.1.0-r3 — package revision 0.1.0-3, safety and compatibility test release

- Build the public Arch package against the older July 2026 repository baseline, then test the same bytes on newer Qt/KDE libraries.
- Derive direct native-library dependencies and actual GLIBC/GLIBCXX/CXXABI floors instead of copying recursive build-machine package versions, duplicate providers and fakeroot injection.
- Remove generated QML AOT code that introduced private, Qt-minor-specific ABI imports; reject unexpected private Qt imports in final ELF files.
- Accept a same-major Qt runtime at or above the build-minor baseline only after eager native-plugin loading succeeds, before activation modifies settings. Older or unresolved ABIs still fail safely.
- Includes the safety changes first verified in the local revision-2 candidate below. The final Arch archive passed old/new runtime checks and installed/activated on the maintainer's existing CachyOS libraries without an OS update; the final Fedora RPM passed real KDE touchscreen checks. Full evidence and limitations are in `docs/revision3-verification.md`.
- Automated ongoing maintenance/update channels are explicitly deferred; this remains a manually downloaded public test release.

## Local package release 0.1.0-2 — verified safety candidate (not publicly released)

- Cancel stale/interrupted swipes across focus/privacy changes and geometry changes; revalidate sensitive context before recognition or insertion.
- Reject word-alternative replacement when a selection or unsafe field context makes multi-character editing uncertain.
- Add a read-only version/session/selection support summary and optional agent-assistance links on errors.
- Document uninstall recovery, desktop-upgrade compatibility and symptom-based troubleshooting. AI-agent assistance, including YouCoded, remains optional.
- Candidate packages have a new native release number; published 0.1.0-1 downloads are not silently replaced. Verification details belong in the candidate acceptance report, not the historical v0.1.0 results below.

## 0.1.0 — first public test release

- Standalone English (US) swipe typing for Plasma Keyboard; no Z13, touch-profile, theme or YouCoded dependency.
- Native Arch/CachyOS and Fedora 44 packages with compiled recognition code, models and dictionary. No user-side compilation or optional training-corpus download.
- Reversible per-user activation, compatibility checks, private layout regeneration, and preservation of later user setting changes.
- First ordinary Backspace after a swipe or candidate correction undoes that word when exact context is available. Further typing/cursor movement, selections and uncertain/sensitive context use normal deletion. Only swipe-generated spacing is undone.
- Launch waits briefly for setup to release its lock instead of spuriously falling back to stock during activation.
- Recording is opt-in, owner-only and excluded for sensitive/password input.
- Pinned/hash-checked model and build inputs, portable CPU defaults, corresponding source, third-party notices and native package CI checks.

### Known limits

- KDE Wayland, x86_64, Plasma Keyboard 6.7/6.8 candidate range; Qt major/minor must match the package. Initial native targets are current Arch and updated Fedora 44 KDE, not every Linux distribution or release ISO.
- English swipe recognition only. Application text-input support still affects keyboard summoning and editing.
- Firefox physical Ctrl+A/Delete may leave stale field context, causing an extra leading space on the next swipe. Accepted as a minor v1 limitation. Apps that hide or misreport their text cannot guarantee automatic word deletion/correction.
- Downloads are provided through GitHub Releases; no AUR/COPR package repository or automatic update feed is configured.

**Powered by FUTO Swipe.** Code, model weights and dictionary data have different licenses; see README and the installed notices. This project is not endorsed by FUTO or KDE.
