# Changes

## 0.5.0 (unreleased)

- Swipes pick "were" or "we're", "its" or "it's", "well" or "we'll" from the sentence (language files `language-files-2026-10-23-apostrophes`, which keep apostrophes); rare junk spellings ("I's" for "is") never win. Fixes 0.4.0-era test builds that typed "i's", "wa's", "i'n".
- "I", "I'm", names, places, days and months are shown with their capital in English, Spanish and French swipes, completions, other spellings, tap-to-swap and next-word suggestions (built-in lists in `packaging/capitals`); ordinary words stay lower case, including ones web text often capitalises inside names ("banco", "hall"). German is unchanged.
- The shared language and session rules are MIT, so the Gaming Mode daemon and the tools are MIT as built.

- New trained models, downloaded by checksum from the `models-2026-10-23` release (models trained and released by Destin Moss via youcoded.ai, 2026): a small neural next-word model per language (`LANG.nwp.bin`, English/Spanish/French/German, ODC-By 1.0 AND CC BY 2.0 FR) that improves next-word suggestions together with the n-gram files, and our own swipe model (`swipe-model.bin`, MIT; trained on FUTO's MIT swipe data and synthetic swipes), used only when the `own-engine` file exists. Each file is checked against `assets.lock.json`: a missing or altered next-word model only turns that model off (n-gram suggestions as before), a missing or altered swipe model leaves the own engine on geometry only. FUTO's engine, models and notices are unchanged.

- New, off by default: learn from your typing (open-engine roadmap Phase 2). Turned on by a `learn-typing` file in the keyboard data folder (`swipe-keyboard learning on`, or the `install.sh` question when run in a terminal). It remembers new words (after two uses), word pairs and favourites, with a ~45-day fade and size caps, per language, in private files; never in password, e-mail, web-address, number or no-prediction fields; never over a network. Learned words are added to both swipe word lists; a long press on a learned suggestion removes it; `swipe-keyboard learning forget` erases everything.

- The Arch and Fedora packages no longer set an upper limit on `plasma-keyboard`, so a Plasma 6.9 system upgrade is never refused because of Swipe Keyboard.
- New: Swipe Keyboard for Plasma 6.9's rebuilt keyboard. One package now carries two front ends, and `swipe-keyboard launch` starts the one that matches the installed Plasma Keyboard (6.7/6.8: as before; 6.9 and later 6.x: the new one). On 6.9 you keep swipe typing, the suggestion row (swiped word's alternatives, completions, next words, tap-to-swap), whole-word Backspace, the spacebar cursor slide, texting words and learning, in English, Spanish, French and German. It adds itself to KDE's own language keyboards without changing KDE's files; your language choices keep working. It uses neither FUTO nor Qt Virtual Keyboard (on Arch that library becomes optional; Fedora needs it only before 6.9). Differences on 6.9: no underlined word while typing (words are typed directly) and the row takes a little of the key height. If anything does not fit (Plasma 7, an unknown keyboard format, a damaged install), KDE's plain keyboard starts and typing keeps working. Tested against KDE's development code; still to be tried on a real Plasma 6.9 device (see `docs/plasma-6.9-front-end.md`).

- New: texting abbreviations and slang you can swipe and complete out of the box, for English, Spanish, French and German (about 110 / 56 / 61 / 50 words: wyd, lmao, rn, pls, brb, smh; jaja, xq, porfa; mdr, jsp, tkt, bjr; lg, vllt, kp, iwie ...). They are our own hand-written lists (CC0, `packaging/texting-words/`), added to the swipe word list when the keyboard starts, for FUTO's engine and for our own engine alike. A listed word is only ever raised (never made weaker than the word list already has it), and words the list marked "not a word" (rn, pls, ya) now count. They appear when swiping and when completing typed letters, not as next-word suggestions out of the blue, and abbreviations of swear words (wtf, stfu, ntm) never appear unprompted. A damaged list file is ignored. Measured on real swipes (FUTO's English test sets and HeliBoard's en/es/fr/de sets): first-guess accuracy changed by at most 0.02 points on every set (our engine; FUTO's engine on the English sets: no change at all); on synthetic noisy swipes of the abbreviations themselves our engine now gets 83-92% right first time (before: 32-52%) and FUTO's engine about 40% (before 19%).

- Removed the optional neural re-rank of the top swipe guesses (the SmolLM2 model run through llama.cpp, switched on by a `use-nlm` file). It was off by default, so typing behaves exactly as before: the n-gram ranking, our own swipe engine, prediction and tap-to-swap are untouched. The install and download scripts no longer fetch or copy the model, and the build no longer looks for llama.cpp. A future sentence-reading model of our own is planned to replace it. (Owner decision 2026-10-23.)

- Word lists and the Spanish, French, German and English swipe dictionaries are now counted from FineWeb/FineWeb-2 text (ODC-By 1.0) and Tatoeba sentences (CC BY 2.0 FR) instead of wordfreq; the English dictionary keeps the AOSP list (Apache-2.0). No share-alike (CC BY-SA) licence applies any more. On our tests no typing measure got worse by more than 0.1 point; French completions improved by up to 2 points and the lists now spell German nouns, names and French elisions as written.
- Next-word suggestions, completions and swipe ranking for English, Spanish, French and German now come from open language files (FineWeb web text under ODC-By 1.0 plus Tatoeba sentences under CC BY 2.0 France), downloaded by checksum at build time from the `language-files-2026-10-22` release. They are larger (about 24 to 38 MB each) and noticeably better than the old Tatoeba-only packs.
- English uses its n-gram instead of FUTO's neural context model when the English files are installed and verified; otherwise it keeps FUTO's model. A missing or damaged Spanish, French or German file now turns off only that language's suggestions instead of the whole language.

## v0.4.1-1

- Gaming Mode works with Steam's stable client (0.4.0 attached only with the beta client): the keyboard page is accepted after it proves it is Steam's own origin; calls from any other document are ignored. Tested on SteamOS 3.9.5.
- Automatic updates on every platform, once a day, accepting only updates signed with the project key: Flatpak on the Steam Deck (set up by `install.sh`, now the Deck install path), the package's own signed dnf repository on Fedora, and a signed index plus `pacman -U` of that one package on Arch. Off switches: `bash install.sh --no-auto-update`, `sudo swipe-keyboard updates off`. 0.4.0 and older need 0.4.1 installed once by hand.

## v0.4.0-1

- Steam Deck Gaming Mode preview: swipe typing and word suggestions on top of Steam's own on-screen keyboard, from one opt-in SteamOS download (`install.sh` / `uninstall.sh`). Tested on a virtual Steam Deck only; relies on a Valve developer switch (see the README warning).
- Switching between the `?123` symbols page and the letters no longer pauses: the swipe dictionary stays loaded instead of reloading on every switch.
- Slide along the Space bar to move the text cursor in the direction your finger moves: one character at a time when sliding slowly, up to three times further when sliding fast, like a touchpad (also in right-to-left text, where it follows the screen, not the reading order). A small wobble or a normal tap still types a space; once the slide starts, no space is typed. Works with touch and mouse, in every language layout, and in password fields (only the cursor moves). Moving the cursor also clears the current word's suggestions so a later suggestion tap cannot replace the wrong text. Not available on the `?123` symbols page, and there is no setting to turn it off yet.

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
