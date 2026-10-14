# Swipe Typing for the Z13 — Design

**Date:** 2026-10-14 · **Status:** awaiting Destin's review · **Machine:** z13 (ROG Flow Z13, KDE Plasma 6.7, Wayland)

## Goal

Gboard-quality swipe typing on the Linux on-screen keyboard Destin already uses (`plasma-keyboard`
with touch-profile's rounded-key look). Modeled heavily on how Gboard behaves, rebuilt from scratch
as Linux-native code. English only. Target: beat the best open swipe engine (FUTO Swipe) on real
human swipes, and approach Gboard as measured by the study report.

## What Destin will experience (v1)

1. **Swipe a word.** Dragging across letter keys draws a glowing trail; on lift the best guess is
   typed. Taps behave exactly as today — only a drag that leaves the starting key starts a swipe.
2. **Guess bar.** A strip above the keys (about one finger-height; the keyboard grows by that much)
   shows the next two guesses. Tapping one swaps it in for the typed word.
3. **Smart spacing.** Swiped words get a space between them automatically; typing punctuation right
   after a swiped word pulls the space back (`word.` not `word .`). Exact rules follow the study report.
4. **Backspace removes the whole swiped word** when pressed right after a swipe; otherwise one letter.
5. **Spacebar slide moves the cursor.** A deliberate horizontal slide on the spacebar (past a
   threshold, so a sloppy tap still types a space) moves the cursor one character per step.
6. **Learns your words.** A word typed by tapping that isn't in the dictionary, committed twice,
   becomes swipeable. Stored only on this computer.
7. **Practice mode (optional).** A small app shows words to swipe for ~10 minutes; those swipes
   tune the engine to a 13" screen, which differs from the phone swipes in public data.
8. **Learning from corrections (optional, off by default).** When a guess is swapped or fixed, the
   swipe and the right word are saved locally for later tuning. Never uploaded.

## Legal approach: clean room

Rests on the EU Software Directive (2009/24/EC) Art. 5(3): a lawful user may observe, study and test
a program to learn its ideas and principles. No Google code, model weights, or dictionaries are
copied. (Not legal advice.)

- **Source copy:** Gboard pulled from Destin's own Android phone over USB (adb) — a lawful copy.
- **Study agent** uses REA (`morluto/rea`, MIT) plus hands-on observation, in a private workspace
  outside the repo (`~/swipe-cleanroom-private/`, never pushed). Output: a plain-language
  behavior and architecture report, `docs/cleanroom/gboard-report.md`, containing no code,
  disassembly, weights or data excerpts.
- **A review pass** checks the report for anything copied before it enters the repo.
- **Build agents** only see the report, Google's published papers (e.g. Alsharif et al. 2015,
  LSTM gesture decoding), and open sources. They never read the private workspace.

## Architecture

```
finger trace ─► plasma-keyboard (unchanged) ─► SwipeInputMethod (our Qt plugin)
                                                   │  trace points + key geometry + text before cursor
                                                   ▼
                                               libswipe (our engine, no Qt)
                                                   │  ranked words
                                                   ▼
                                    guess bar + commit text via Qt Virtual Keyboard
```

### Units

| Unit | Does | Depends on |
|---|---|---|
| `engine/` — **libswipe** (C++20, plain C API) | Trace + key layout + context → ranked candidates | dictionary/LM files; ONNX Runtime (Stage B only) |
| `engine` Stage A — **shape decoder** | SHARK2-style: resample trace, compare location and shape channels against each word's ideal path, prune by start/end keys, weight by language model | lexicon, n-gram LM |
| `engine` Stage B — **neural decoder** | Small network reads per-point features (position relative to keys, speed, timing) and outputs letter probabilities; beam search over a lexicon trie fused with the n-gram LM | ONNX model file trained by `training/` |
| `lm/` — **dictionary + language model** | English lexicon with frequencies, compact bigram/trigram model, user-word overlay | built by scripts from openly licensed word lists / text (license recorded per source) |
| `plugin/` — **SwipeInputMethod** (Qt 6 QML module) | Subclasses `QVirtualKeyboardAbstractInputMethod` with `TraceInput`; feeds traces to libswipe; fills the word-candidate list; smart spacing; whole-word backspace; user-word capture | libswipe, Qt Virtual Keyboard |
| `layouts/en_US` — **layout override** | Our en_US layout selecting `SwipeInputMethod` and a sliding spacebar | installed into `~/.local/share/plasma/keyboard/layouts/` (the folder touch-profile already uses) |
| `training/` — **training pipeline** (Python/PyTorch, ROCm or CPU) | Builds Stage B from real swipes, plus synthetic swipes only to fill rare-word gaps | FUTO swipe dataset (MIT), practice data |
| `bench/` — **scoreboard** | Replays held-out *real* swipes; reports top-1, top-3, latency | engine, datasets, FUTO model as a yardstick |
| `practice/` — **practice app** (small Qt app) | Prompts words; pairs them with traces the keyboard logs while practice is on | plugin's local trace log |

Hooking in reuses existing pieces: the Breeze style already draws the trace trail
(`traceCanvasDelegate`) and the guess bar (`selectionListDelegate`). The touch-profile wrapper
`tp-plasma-keyboard` gains one import path so plasma-keyboard finds the plugin; that change lives in
the touch-profile repo. KDE's own packages are not modified.

### Data on disk (all under `~/.local/share/swipe-keyboard/`)

- `user-words.tsv` — learned words. Deleting it forgets them.
- `traces/` — practice and (opt-in) correction swipes. Deleting it resets personal tuning data.

## Accuracy discipline

1. The scoreboard grades **only real human swipes** the engine never trained on (FUTO data split by
   user, plus Destin's practice swipes).
2. Training uses real swipes first; synthetic swipes are used only for rare words, and only after
   their speed / corner-cutting / overshoot statistics are checked against real ones.
3. A change ships only if it does not lower the real-swipe score.
4. Yardsticks: Stage A, FUTO's released model (scored, never copied into ours), and Gboard behavior
   described in the study report.
5. Latency target: candidates shown within 30 ms of finger lift on the Z13 (95th percentile).

## Failure behavior

- Stage B model missing or fails to load → engine falls back to Stage A silently.
- Engine error on a swipe → nothing is typed for that swipe; tapping keeps working.
- Plugin fails to load → plasma-keyboard falls back to its built-in input method (normal tapping).
- `install.sh --uninstall` removes the layout override and import path, restoring today's keyboard
  exactly. Current files are backed up before the first install.
- Training runs heat the Z13 and spin its fans; they run only on AC power.

## Milestones

| # | Delivers | Destin can |
|---|---|---|
| M0 | Clean-room study report | Read how Gboard works, in plain words |
| M1 | Stage A engine + plugin + scoreboard | Swipe words with trail and guess bar |
| M2 | Smart spacing tuned to the report, whole-word backspace, spacebar slide, learned words | Use the full v1 feature set |
| M3 | Practice app + opt-in correction logging | Tune the engine to his screen |
| M4 | Stage B neural decoder | Feel the jump in forgiveness of sloppy swipes |
| M5 | Tuning loop against FUTO + report | Get the "best on the planet" push |

## Later list (not v1)

Next-word suggestions before typing · autocorrect for tapped words · other languages · forgetting a
learned word from the guess bar · emoji suggestions · upstreaming to KDE.

## Testing

- Engine: unit tests on fixed traces (exact expected top candidates); scoreboard run in CI-style
  script on every engine change.
- Plugin: tests with recorded traces driving `SwipeInputMethod` headless (commit text, spacing,
  backspace, user-word capture).
- Live check on the Z13 in tablet mode: swipe into Chrome, Konsole, Steam (XWayland), confirm taps
  unchanged and uninstall restores stock behavior.
