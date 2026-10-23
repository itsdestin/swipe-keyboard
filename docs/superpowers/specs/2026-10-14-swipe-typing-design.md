# Swipe Typing for the Z13 — Design

**Date:** 2026-10-14 · **Status:** awaiting Destin's review · **Machine:** z13 (ROG Flow Z13, KDE Plasma 6.7, Wayland)

## Goal

Gboard-quality swipe typing on the Linux on-screen keyboard Destin already uses (`plasma-keyboard`
with touch-profile's rounded-key look). English only.

- **Swipe recognition** starts from FUTO Swipe, the best open swipe model (93% first-guess, 98%
  top-3 on held-out real swipes per its paper), then is fine-tuned for the Z13's 13" screen.
- **Word choice and behavior** (which candidate wins given the sentence so far, spacing, backspace)
  are modeled on Gboard through a clean-room study, and built by us.
- **Target:** beat stock FUTO Swipe on real human swipes, and approach Gboard as the study report
  describes it.

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
   fine-tune the model to a 13" screen, which differs from the phone-thumb swipes it was trained on.
8. **Learning from corrections (optional, off by default).** When a guess is swapped or fixed, the
   swipe and the right word are saved locally for later fine-tuning. Never uploaded.
9. **Credit line.** The practice app's About box and the README say "Powered by FUTO Swipe" (license
   requirement, below).

## Building blocks we reuse

| Piece | License | What we must do |
|---|---|---|
| FUTO Swipe model weights (encoder 635K params, English QWERTY decoder 304K) | FUTO Model Weights License 1.0 | Show "Powered by FUTO Swipe"; our fine-tuned model is marked as modified and ships with the license text |
| FUTO swipe-library (C++: input processing, inference, beam search) | GPL | Our code that links it is GPL too. Fine while private; if the keyboard is ever shared, its source must be shared |
| FUTO swipe dataset (1M+ real swipes, 12k+ sessions) | MIT | Credit in README |
| FUTO Swipe paper (method details) | CC BY 4.0 | Cite |

If swipe-library will not build or run on Linux x86-64, we write our own thin inference + beam
search around the same model files with the ExecuTorch runtime (the format FUTO ships).

## Legal approach for the Gboard study: clean room

Rests on the EU Software Directive (2009/24/EC) Art. 5(3): a lawful user may observe, study and test
a program to learn its ideas and principles. No Google code, model weights, or dictionaries are
copied. (Not legal advice.)

- **Source copy:** Gboard pulled from Destin's own Android phone over USB (adb) — a lawful copy.
- **Study agent** uses REA (`morluto/rea`, MIT) plus hands-on observation, in a private workspace
  outside the repo (`~/swipe-cleanroom-private/`, never pushed). Focus: how Gboard ranks candidates
  using the preceding words, how it uses word frequency and personal history, and its spacing,
  correction and backspace behavior. Output: a plain-language report,
  `docs/cleanroom/gboard-report.md`, with no code, disassembly, weights or data excerpts.
  **Withdrawn 2026-10-23 (Destin):** the report is removed from the repository and no later work uses it.
  Gboard comparisons are made only by testing the app from the outside (`bench/gboard/`).
- **A review pass** checks the report for anything copied before it enters the repo.
- **Build agents** only see the report, Google's published papers, and open sources. They never read
  the private workspace.

## Architecture

```
finger trace ─► plasma-keyboard (unchanged) ─► SwipeInputMethod (our Qt plugin)
                                                   │  trace points + key geometry + text before cursor
                                                   ▼
                                     swipe engine: FUTO model (fine-tuned) → candidate words
                                                   │
                                     ranker: our Gboard-style context model + your words
                                                   ▼
                                    guess bar + commit text via Qt Virtual Keyboard
```

### Units

| Unit | Does | Depends on |
|---|---|---|
| `engine/` — **swipe engine** (C++, plain C API) | Trace + key layout → candidate words with swipe scores, via FUTO's encoder/decoder and beam search over the lexicon | FUTO swipe-library or ExecuTorch; model files |
| `ranker/` — **context ranker** | Re-scores candidates using the previous words, word frequency and your learned words — the Gboard-modeled part | n-gram model built from openly licensed English text (license recorded per source); `user-words.tsv` |
| `plugin/` — **SwipeInputMethod** (Qt 6 QML module) | Subclasses `QVirtualKeyboardAbstractInputMethod` with `TraceInput`; feeds traces to the engine; fills the guess bar; smart spacing; whole-word backspace; learned-word capture | engine, ranker, Qt Virtual Keyboard |
| `layouts/en_US` — **layout override** | Our en_US layout selecting `SwipeInputMethod` and a sliding spacebar | installed into `~/.local/share/plasma/keyboard/layouts/` (the folder touch-profile already uses) |
| `training/` — **fine-tuning** (Python/PyTorch, ROCm or CPU) | Continues training FUTO's model on Destin's practice/correction swipes, keeping FUTO data in the mix so general accuracy doesn't drop | FUTO dataset, practice data |
| `bench/` — **scoreboard** | Replays held-out *real* swipes; reports first-guess %, top-3 %, latency for stock FUTO vs ours | engine, ranker, datasets |
| `practice/` — **practice app** (small Qt app) | Prompts words; pairs them with traces the keyboard logs while practice is on; About box credit | plugin's local trace log |

Hooking in reuses existing pieces: the Breeze style already draws the trace trail
(`traceCanvasDelegate`) and the guess bar (`selectionListDelegate`). The touch-profile wrapper
`tp-plasma-keyboard` gains one import path so plasma-keyboard finds the plugin; that change lives in
the touch-profile repo. KDE's own packages are not modified.

### Data on disk (all under `~/.local/share/swipe-keyboard/`)

- `user-words.tsv` — learned words. Deleting it forgets them.
- `traces/` — practice and (opt-in) correction swipes. Deleting it resets personal tuning data.
- `models/` — the stock and fine-tuned models; the stock one is always kept so we can roll back.

## Accuracy discipline

1. The scoreboard grades **only real human swipes** the model never trained on: FUTO's held-out
   test split, plus a held-back set of Destin's practice swipes.
2. Two scores, both must hold: **general** (FUTO test split — fine-tuning must not make it worse for
   ordinary swiping) and **Z13** (Destin's held-back swipes — must improve).
3. A change ships only if it does not lower either score. Yardstick: stock FUTO Swipe.
4. The context ranker is scored on swipes taken from full sentences, since it only helps when it
   sees the preceding words.
5. Latency target: guesses shown within 30 ms of finger lift on the Z13 (95th percentile).

## Failure behavior

- Fine-tuned model missing or broken → engine loads the stock FUTO model.
- Engine error on a swipe → nothing is typed for that swipe; tapping keeps working.
- Plugin fails to load → plasma-keyboard falls back to its built-in input method (normal tapping).
- `install.sh --uninstall` removes the layout override and import path, restoring today's keyboard
  exactly. Current files are backed up before the first install.
- Fine-tuning runs heat the Z13 and spin its fans; they run only on AC power.

## Milestones

| # | Delivers | Destin can |
|---|---|---|
| M0 | FUTO engine running on Linux + plugin + scoreboard | Swipe words with trail and guess bar |
| M1 | Smart spacing, whole-word backspace, spacebar slide, learned words | Use the full v1 feature set |
| M2 | Clean-room Gboard study report | Read how Gboard picks words, in plain language |
| M3 | Gboard-style context ranker | Get better picks from sentence context |
| M4 | Practice app + opt-in correction logging + fine-tuning | Get a model tuned to his screen and hand |
| M5 | Tuning loop against stock FUTO and the report | Get the "best on the planet" push |

M0 comes before the study because swiping does not depend on it. The study only feeds M1's
spacing/backspace details (adjusted afterward if needed) and M3.

## Later list (not v1)

Next-word suggestions before typing (Gboard-modeled, reusing the M3 context model) · autocorrect for
tapped words · other languages · forgetting a learned word from the guess bar · emoji suggestions ·
upstreaming to KDE.

## Testing

- Engine: unit tests on fixed traces (exact expected top candidates); scoreboard run on every engine,
  ranker or model change.
- Plugin: tests with recorded traces driving `SwipeInputMethod` headless (commit text, spacing,
  backspace, learned-word capture).
- Live check on the Z13 in tablet mode: swipe into Chrome, Konsole, Steam (XWayland), confirm taps
  unchanged and uninstall restores stock behavior.
