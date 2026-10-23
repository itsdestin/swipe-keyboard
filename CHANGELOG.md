# Changes

## Unreleased

- New trained models, downloaded by checksum from the `models-2026-10-23` release (models trained and released by Destin Moss via youcoded.ai, 2026): a small neural next-word model per language (`LANG.nwp.bin`, English/Spanish/French/German, ODC-By 1.0 AND CC BY 2.0 FR) that improves next-word suggestions together with the n-gram files, and our own swipe model (`swipe-model.bin`, MIT; trained on FUTO's MIT swipe data and synthetic swipes), used only when the `own-engine` file exists. Each file is checked against `assets.lock.json`: a missing or altered next-word model only turns that model off (n-gram suggestions as before), a missing or altered swipe model leaves the own engine on geometry only. FUTO's engine, models and notices are unchanged.

- New, off by default: learn from your typing (open-engine roadmap Phase 2). Turned on by a `learn-typing` file in the keyboard data folder (`swipe-keyboard learning on`, or the `install.sh` question when run in a terminal). It remembers new words (after two uses), word pairs and favourites, with a ~45-day fade and size caps, per language, in private files; never in password, e-mail, web-address, number or no-prediction fields; never over a network. Learned words are added to both swipe word lists; a long press on a learned suggestion removes it; `swipe-keyboard learning forget` erases everything.

- Removed the optional neural re-rank of the top swipe guesses (the SmolLM2 model run through llama.cpp, switched on by a `use-nlm` file). It was off by default, so typing behaves exactly as before: the n-gram ranking, our own swipe engine, prediction and tap-to-swap are untouched. The install and download scripts no longer fetch or copy the model, and the build no longer looks for llama.cpp. A future sentence-reading model of our own is planned to replace it. (Owner decision 2026-10-23.)

- Word lists and the Spanish, French, German and English swipe dictionaries are now counted from FineWeb/FineWeb-2 text (ODC-By 1.0) and Tatoeba sentences (CC BY 2.0 FR) instead of wordfreq; the English dictionary keeps the AOSP list (Apache-2.0). No share-alike (CC BY-SA) licence applies any more. On our tests no typing measure got worse by more than 0.1 point; French completions improved by up to 2 points and the lists now spell German nouns, names and French elisions as written.
- Next-word suggestions, completions and swipe ranking for English, Spanish, French and German now come from open language files (FineWeb web text under ODC-By 1.0 plus Tatoeba sentences under CC BY 2.0 France), downloaded by checksum at build time from the `language-files-2026-10-22` release. They are larger (about 24 to 38 MB each) and noticeably better than the old Tatoeba-only packs.
- English uses its n-gram instead of FUTO's neural context model when the English files are installed and verified; otherwise it keeps FUTO's model. A missing or damaged Spanish, French or German file now turns off only that language's suggestions instead of the whole language.

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
