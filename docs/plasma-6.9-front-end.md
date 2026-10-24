# Swipe Keyboard on Plasma 6.9's rebuilt keyboard

Plasma 6.9 (due 23 February 2027; feature freeze 5 January, beta 19 January) replaces Plasma Keyboard's Qt Virtual
Keyboard base with its own keyboard core. Swipe Keyboard therefore ships **two front ends in one package**:

| Installed plasma-keyboard | Files the launcher finds | Front end started |
|---|---|---|
| 6.7.0 – 6.8.79 | Qt Virtual Keyboard layouts (`/usr/share/plasma/keyboard/layouts/en_US`) | `qml/SwipeKeyboard` (plugin/) |
| 6.8.80 – 6.x (6.9 dev, beta, release, 6.10 …) | KDE keyboard packages (`plasma/keyboard/keyboardpackages/org.kde.plasma.keyboard.en`) | `qml/SwipeKeyboard69` (frontend-plasma69/) |
| 7.0 or newer, or version and files disagree, or unknown version with both/neither | — | KDE's plain keyboard (one log line says why) |

`integration/setup.py` decides (`detect_front_end`). Both the version and the files must agree, so a half-finished
upgrade or a distro patch never starts the wrong code. On 6.9 the launcher also preflights the 6.9 module
(`swipekb-preflight` with eager symbol binding) and generates the keyboard packages; any failure starts KDE's plain
keyboard. The 6.9 path needs no FUTO file and no Qt Virtual Keyboard.

## How the 6.9 front end works

1. **Packages.** At every launch `integration/plasma69.py` copies KDE's English, Spanish (ES/MX), French (FR/CA) and
   German packages into `~/.local/share/swipe-keyboard/overlay/plasma/keyboard/keyboardpackages/`, with the same ids.
   Each letter layout's file (`"Files": {"main": …}` on master, `"File": …` with MR !179) points at a generated wrapper
   `SwipeKeyboard<Name>.qml` next to KDE's file. KDE's files are never modified. The launcher puts the overlay first in
   `XDG_DATA_DIRS` for the keyboard process only, and passes the generated ids in `SWIPEKB_PLASMA69_PACKAGES`.
2. **Wrapper.** It instantiates KDE's layout unchanged and then creates `SwipeAttach` from our module *at run time*
   (`Qt.createComponent("SwipeKeyboard69", "SwipeAttach")`). If our module cannot load, the layout is still KDE's.
3. **Hook** (`src/swipehook.cpp`). Finds KDE's Keyboard item around the layout, creates one `SwipeController` per
   input engine, and on every layout of our packages that KDE shows (letters and symbol pages) adds our suggestion row
   as the first row and our swipe layer over the keys. The panel keeps KDE's height (symbol pages are Loaders whose
   height factor cannot be raised, so a taller letters page would jump at every "?123").
4. **Layer** (`qml/SwipeLayer.qml`). Listens passively to the finger; when a press that started on a letter travels
   half a key it makes that key forget the press (no letter typed) and collects the path. Reports every key press and
   finished key to the controller. Right after a swipe it takes over only Backspace (erases the whole swiped word), and
   right after an automatic space only the punctuation keys (". " instead of " .").
5. **Controller** (`src/swipecontroller.cpp`). The same rules and session classes as the 6.7/6.8 front end, on the
   FUTO-free `swipekb::LanguageEngine` and `resolveProfile()`. Edits use UTF-8 byte offsets (input-method-v1).
   Field policy: password/hidden fields get nothing; e-mail, web-address, number, date fields no prediction or
   learning; an app that declares completion+correction (no `ImhNoPredictiveText`) may have its text read (sentence
   context, tap-to-swap); otherwise only words this keyboard typed and saw echoed count (today's stock-Plasma rule).

## What works, what is missing compared with 6.7/6.8

Works: swipe typing (en, es, fr, de; QWERTY/AZERTY/QWERTZ, desktop variants), automatic spaces, capital from Shift,
the swiped word's alternatives, whole-word Backspace, completion with one-letter corrections, next-word suggestions,
tap-to-swap, texting words, the swear-word rule, learning (same `learn-typing` switch, same `learning/LANG.swklearn`
store, same `swipe-keyboard learning` command), long-press removal of a learned word, language switching.

Missing or different:
- **No preedit.** Every word is committed outright (KDE's composer slot is internal). Fixing a word relies on the app
  reporting its text; where it does not, alternatives are offered only while they can be proven safe, else dropped.
- **Own suggestion row**, not KDE's (KDE's row only shows built-in composers' candidates); keyboard navigation does not
  reach it. It takes ~13% of the key height.
- **Spacebar cursor slide** is not ported (KDE MR !69 adds its own; port after it settles to avoid doing it twice).
- **Swipe recorder / outcome lines** (opt-in tuning data) are not written by this front end.
- **Window blur** and the accent-colour trail probe are not ported (cosmetic).
- Dvorak layouts: tap only (the language packs declare QWERTY/AZERTY/QWERTZ).
- No settings page inside KDE's keyboard settings (the `swipe-keyboard` command stays).

## KDE internals we rely on (checked by tests)

None of this is public API; each is verified by `tests/harness` → `kdeInternalsWeRelyOnArePresent` and by
`SwipeHook::missingInternals()` at run time (missing → stay inactive, log a warning, KDE keyboard works).

| Internal (plasma-keyboard source) | Used for |
|---|---|
| Package lookup: first `plasma/keyboard/keyboardpackages/<dir>/metadata.json` with a given KPlugin Id wins (`keyboardlayoutmetadata.cpp`, `keyboardpackageresolver.cpp`) | our packages shadow KDE's |
| Layout file key: `"Files": {"main"}` (master) or `"File"` (MR !179) | where the wrapper goes |
| Keyboard item properties `inputEngine`, `keyboardController`, `layoutType`, `packageId`, `layoutId` (`components/Keyboard.qml`) | finding the keyboard, page type, language |
| The layout `Loader` one level inside the Keyboard's ColumnLayout; layouts are `KeyboardLayout` columns (`panelHeightFactor`) or `KeyboardLayoutLoader` pages | where the row and layer go |
| `InputEngine` properties `surroundingText`, `cursorPosition`, `anchorPosition`, `inputMethodHints`, `preeditText`, `uppercase`, `capsLockActive`, `shiftActive`, `locale`; signals `…Changed()`; invokables `commit(QString,int,int)`, `sendTextComposerKey(int,QString)` (`inputengine.h`) | reading the field, typing, Shift |
| `commit()` deletes **bytes** relative to the cursor, passed unchanged to input-method-v1 (`inputengine.cpp`, `src/inputmethodconnection.cpp`) | correct replacements |
| Keys: `Key.qml` items with `key`, `text`, `commitText`, `pressedVisual`, signal `clicked()`; child `KeyMouseArea` (MultiPointTouchArea) with `pressStarted(var)`; cancel on ungrab | taps, swipe takeover |
| Window property `alternativeKeysPopupItem` with `active` (`src/qml/main.qml`) | no swipe while the accent popup is open |
| Hint mapping `src/inputmethodhints.cpp` (password → `ImhSensitiveData`, hidden → + `ImhHiddenText`, no completion/correction → `ImhNoPredictiveText`, e-mail/url/number…) | field policy |

## Re-checking against KDE (master now, the beta in January)

All of this runs without touching the installed keyboard:

```sh
cd ~/.cache/swipe-keyboard-train/plasma-keyboard-master && git pull            # or: git checkout v6.8.90 (beta tag)
git fetch origin 'refs/merge-requests/179/head:refs/remotes/mr/179'            # any open MR worth tracking
cmake -S frontend-plasma69 -B ~/.cache/swipe-keyboard-train/pk69/build-fe -G Ninja \
      -DSWIPEKB_PLASMA_KEYBOARD_SOURCE=$HOME/.cache/swipe-keyboard-train/plasma-keyboard-master \
      -DSWIPEKB_TEST_MODEL_DATA=<installed-layout asset folder: dict/, language-packs/>
ninja -C ~/.cache/swipe-keyboard-train/pk69/build-fe && ctest --test-dir ~/.cache/swipe-keyboard-train/pk69/build-fe
```

`tests/harness` builds KDE's `virtualkeyboard/` core, components, shared layouts and language packages **unmodified**
from that tree (the application-only module is a stub; a fake Wayland backend applies byte-based edits like KWin +
an app), then drives it with touches offscreen. A KDE rename fails `kdeInternalsWeRelyOnArePresent` with the missing
names; a format change fails `ourPackageWinsAndAttaches` or the launcher test `test_unknown_package_format…`. Last
green: master `030ce04` and MR !179 `8f03a5f` (2026-10-23), 16 end-to-end tests each. For the beta, also run the
launcher tests (`python3 -m unittest discover -s tests/integration`) after setting the beta's real `--version` string
in `test_each_version_gets_the_matching_front_end` if it uses a new form.

## Real-device test (KWin + real plasma-keyboard)

Not yet done: this machine runs Plasma 6.7.5 and the full plasma-keyboard app needs PlasmaQuick ≥ 6.7.90. Run it in a
VM (never on the owner's working system); a VM disk needs ~25–40 GB, so put it on external storage.

1. **Now:** KDE neon Unstable (Plasma git master, ordinary writable `/usr`) in QEMU/virt-manager. KDE Linux also runs
   master but its `/usr` is read-only, so installing our package there needs a system extension; prefer neon.
   **January:** the 6.9 beta in an Arch VM with the `kde-unstable` repository. Add `-device virtio-multitouch-pci`
   (QEMU ≥ 8.1) for touch; the mouse also swipes.
2. In the VM: build and install from this branch: on Arch `makepkg -si` in `packaging/arch` (after
   `scripts/package-source.sh`); on neon `cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build &&
   sudo cmake --install build` (after `scripts/fetch-assets.sh`).
3. `swipe-keyboard check` must report `Front end: Plasma 6.9 keyboard core`; then `swipe-keyboard enable --yes`.
4. In System Settings → Virtual Keyboard, choose "Swipe Keyboard"; open Kate or a browser text field.
5. Check, in order: tap `q` types "Q"; swipe "hello" types "Hello" and the row shows alternatives; tap one → replaced;
   Backspace right after a swipe erases the word; type "hel" → completions; Space → next words; tap into an earlier
   word in Kate → alternatives → tap → swapped; a password field (a browser login form) → no trail, no row, plain taps; globe key → German → swipe "hallo"; ?123 page → no swipe,
   same panel height; `journalctl --user -b | grep -i swipe` shows no warning.
6. Break-checks on the device: `mv /usr/share/swipe-keyboard/qml/SwipeKeyboard69 /tmp/` → relaunch keyboard → KDE's
   plain keyboard still types (log: "Swipe Keyboard is unavailable"); move it back. `swipe-keyboard disable` restores
   the previous keyboard.
