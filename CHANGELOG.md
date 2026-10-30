# Changes

## Unreleased

### Swipes inside a sentence pick the right word a little more often

- **When there are words before the swipe, the keyboard now also asks the sentence model that suggests your next
  word** which of the swipe's candidate words fits best ("I can ___": "see" rather than "sea"). Both Plasma front ends
  (6.7/6.8 and 6.9). It changes nothing for the first word of a message or a field, and nothing in Gaming Mode.
- **Measured** (real English swipes with their sentences, never tuned on; first guess / right word in the top 3):
  bench-2000 90.10 / 93.10 -> 90.95 / 93.25 (17 more swipes right first time of 2,000), test-extra-10000 90.45 / 93.57
  -> 91.09 / 93.71 (64 more of 10,000). On the 1,946 swipes FUTO's engine was compared on, 91.47 -> 92.34 first guess
  (FUTO's engine: 93.32). About one swipe in 118 to 156 that needed a tap on the bar now comes out right; top 3 barely
  moves, because most of the words it fixes were already second or third (so the misses that remain are
  harder: the share of them whose right word is still among the alternatives falls, 30.30 -> 25.41 % on
  bench-2000 and 32.67 -> 29.41 % on test-extra-10000). Swipes without a sentence (HeliBoard sets)
  are unchanged. Spanish has no sentence test set, so its gain is not measured; French and German tuning swipes:
  88.55 -> 90.84 and 88.34 -> 90.16.
- **No extra memory and no slower swipe you can notice:** the sentence model is the one already loaded for the
  suggestion bar, borrowed for about 0.7 ms (worst measured 1.25 ms), which adds about 0.55 ms to a swipe. A swipe takes about 4.5 ms (worst measured 6 ms, 10 ms with
  the suggestion bar kept busy on purpose the whole time). If that model is busy for more than 5 ms, missing or damaged,
  the swipe simply ranks as before.
- **Words you taught the keyboard keep their place:** a word the sentence model does not know keeps its
  usual score instead of being pushed to the bottom.

### Better words to swap in

- **Tap a word to swap it, and the bar after a swipe, now offer real look-alike words ranked by the sentence.** The
  options are words whose swipe would look like yours, ordered by how well they fit the swipe and the words around them.
  In fields the keyboard may read, the words after the tapped one count too ("in the ___ states" offers "united").
- **Typed words get look-alikes too**, from a made-up swipe through their keys, not only one-letter-off spellings.
- **Tap-back lists may show rarer real words** when they fit your swipe well; the bar right after a swipe keeps the
  stricter rule.
- **Measured** (`bench/swap-scoreboard.tsv`, only swipes whose first guess was wrong, first option / among three):
  English, when the first guess was wrong, baseline 0.5.0 -> now. bench-2000 (198 cases): after a swipe 19.70 / 30.81
  -> 24.24 / 36.36, tap-back with the swipe 19.70 / 33.33 -> 36.36 / 42.42, tap-back without 12.12 / 16.67 -> 26.26 /
  32.32. test-extra-10000 (957 cases): 24.66 / 32.60 -> 28.63 / 36.36, 24.76 / 34.38 -> 35.01 / 42.01, 10.76 / 15.57 ->
  26.44 / 31.87. Rare or stretched words among the options go down (test-extra-10000 bar 6.29 % -> 4.90 %). Spanish,
  French and German also find the right word more often (HeliBoard, in-3 after a swipe: es 35.72 -> 40.72, fr 42.51 ->
  47.69, de 23.64 -> 29.48), but their tap-back lists show more rare words than before (es +4.68 and +9.03 points, fr
  +3.72 and +8.97, de +1.04 and +5.14, with and without the swipe), beyond our +3-point limit. A tap-back list takes
  about 9 ms (limit 20). Details: `docs/active/investigations/2026-10-24-swap-results.md`.
- **Learned words:** words the keyboard learned from you still move up in the bar after a swipe (at half strength, as
  before). Tap-back lists are ordered by the swipe and the sentence only; forgotten words never appear in either.

## v0.5.0-1

### Our own engine; no FUTO code or models

- Swipe recognition, completion, suggestions and learning are now entirely the project's own. FUTO's engine, models and notices are gone from the keyboard. The only FUTO material still used is its public swipe data (MIT licence), which trained our swipe model; its licence text ships with the model.
- New trained models, downloaded by checksum from the `models-2026-10-23` release (models trained and released by Destin Moss via youcoded.ai, 2026): four small neural next-word models (`LANG.nwp.bin`, ODC-By 1.0 AND CC BY 2.0 FR) and one swipe model (`swipe-model.bin`, MIT). Each file is checked against `assets.lock.json`. A missing or altered next-word model leaves n-gram suggestions only; a missing or altered swipe model leaves swiping on key geometry alone.
- **Swipe typing is a few points less accurate than the FUTO engine used up to 0.4.1 in English, Spanish and German:** English, on 1,946 real swipes inside sentences, first guess right 91.5 % against 93.3 % and top 3 94.6 % against 96.6 %; the other guesses after a swipe are real words far more often (stretched spellings, rare words and rare names fell from 44 % of them to 5 %). Real swipes without a sentence: Spanish 75.9 % against 77.7 %, German 64.7 % against 69.7 %, French better at 68.2 % against 64.6 %. All rows, with and without sentence context: `bench/swipe-scoreboard.tsv`. Suggestions are better: ahead of Gboard 14.2 (Android 35 emulator, learning off, tested 2026-10-22) on every recorded measure.
- Removed the optional neural re-rank of swipe guesses (SmolLM2 through llama.cpp, switched on by a `use-nlm` file). It was off by default, so typing behaves as before. (Owner decision 2026-10-23.)
- The shared language and session rules are MIT, so the Gaming Mode daemon and the tools are MIT as built. Only the two keyboard add-on layers (`plugin/`, `frontend-plasma69/`) use KDE's GPL choice.

### Plasma 6.9

- One package now carries two front ends, and `swipe-keyboard launch` starts the one that matches the installed Plasma Keyboard (6.7/6.8: as before; 6.9 and later 6.x: the new one). On 6.9 you keep swipe typing, the suggestion row, whole-word Backspace, the spacebar cursor slide, texting words and learning, in all four languages. It adds itself to KDE's own language keyboards without changing KDE's files. It uses neither FUTO nor Qt Virtual Keyboard (on Arch that library becomes optional; Fedora needs it only before 6.9).
- Differences on 6.9: no underlined word while typing (words are typed directly) and the row takes a little of the key height. If anything does not fit (Plasma 7, an unknown keyboard format, a damaged install), KDE's plain keyboard starts and typing keeps working.
- Tested against KDE's development code only; not yet tried on a real Plasma 6.9 device (`docs/plasma-6.9-front-end.md`).
- The Arch and Fedora packages no longer set an upper limit on `plasma-keyboard`, so a Plasma 6.9 upgrade is never refused because of Swipe Keyboard.

### Better words

- **Apostrophes:** swipes pick "were" or "we're", "its" or "it's", "well" or "we'll" from the sentence (language files `language-files-2026-10-23-apostrophes`). Rare junk spellings ("I's" for "is") never win. Fixes 0.4.0-era test builds that typed "i's", "wa's", "i'n". Measured on 2,000 held-out sentences: English apostrophe-pair words right first time 78.0 % to 87.7 %, French 86.3 % to 91.1 %.
- **Capitals:** "I", "I'm", names, places, days and months are shown with their capital in English, Spanish and French (lists in `packaging/capitals`). Ordinary words stay lower case, including ones web text often capitalises inside names ("banco", "hall"). German is unchanged.
- **Open language files:** word lists, swipe dictionaries and next-word files for all four languages are counted from FineWeb/FineWeb-2 text (ODC-By 1.0) and Tatoeba sentences (CC BY 2.0 France), replacing wordfreq; the English dictionary keeps Android's AOSP list (Apache-2.0). No share-alike licence applies any more. English now uses these files like the other languages. On our tests no typing measure got worse by more than 0.1 point; French completions improved by up to 2 points. The files are larger (about 24 to 38 MB each). A missing or damaged file turns off only that language's suggestions, never the language.
- **Texting words:** abbreviations and slang you can swipe and complete out of the box (about 110 English, 56 Spanish, 61 French, 50 German: wyd, lmao, rn, jaja, xq, mdr, jsp, lg, vllt ...). Our own hand-written lists (CC0, `packaging/texting-words/`). They only ever raise a word, appear when swiping or completing (not as unprompted next words), and swear-word abbreviations never appear unprompted. First-guess accuracy on real swipes changed by at most 0.02 points; on noisy swipes of the abbreviations themselves our engine now gets 83 to 92 % right first time (before: 32 to 52 %).

### New, off by default

- **Learning from your typing.** Turned on by a `learn-typing` file (`swipe-keyboard learning on`, or the `install.sh` question when run in a terminal). It remembers new words (after two uses), word pairs and favourites, with a fade of about 45 days and size caps, per language, in private files on your computer. Never in password, e-mail, web-address, number or no-prediction fields; never over a network. A long press on a learned suggestion removes it; `swipe-keyboard learning forget` erases everything.

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

Releases up to 0.4.1 used FUTO's swipe engine and models under their own licences and notices ("Powered by FUTO Swipe"); version 0.5.0 removes them. This project is not endorsed by FUTO or KDE.
