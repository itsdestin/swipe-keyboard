---
status: draft
cards: 170, 179, 181
---
# Swipe Keyboard: remaining work to "as good as Gboard, fully open, FUTO-free"

Owner goals (Destin, 2026-10-22):
1. Typing feels at least as good as Gboard in English, Spanish, French and German.
2. No FUTO code or model weights; KDE's licenses, so it can be merged into KDE's plasma-keyboard.
3. Learn the most decision-useful facts first, so effort goes where the gap really is.

Rules for every phase:
- **No regressions:** a change ships only if no language got worse on any scoreboard measure beyond
  noise (±1 point on ~700-word sets; widen sets where a decision hinges on a smaller gap).
- **Clean room:** new engine code is written from a design doc by a writer that has not read FUTO's
  source or the Gboard report; the design doc itself is reviewed for copied constants.

Revised 2026-10-22 after an independent review (findings folded in below).

## Where we stand (measured 2026-10-22)

"Right word among the suggestions", everyday sentences (Common Voice), ~700 words per language. Gboard
14.2 (2024, built into the Android simulator), learning off. After letters are typed Gboard always spends
one of its 3 slots on the letters themselves; ours is shown with 2 guesses there (3-guess figure in
brackets).

| Language | Measure | Gboard | Ours today | Ours, new file |
|---|---|---|---|---|
| Spanish | before typing / 1 letter / 3 letters | 24 / 46 / 59 | 22 / 45 (50) / 56 (64) | 27 / 51 (56) / 65 (73) |
| French | same | 17 / 41 / 60 | 20 / 42 (48) / 63 (68) | — |
| German | same | 18 / 42 / 60 | 21 / 40 (48) / 55 (61) | — |
| English | same | not measured | not measured | — |

Web text (FineWeb-2 test pages, ~1,300–1,650 words per language), same layout — Gboard / ours today /
ours new file (2-guess after letters):
Spanish 22 / 22 / 28 · 42 / 42 / 51 · 56 / 53 / 68; French 23 / 20 · 41 / 36 · 61 / 50;
German 16 / 14 · 38 / 32 · 54 / 47. Today's Tatoeba-only files trail Gboard on web text; the new Spanish
file (trained on other FineWeb-2 pages, so partly home ground) leads.

- Method check: 30 phrases typed with real key taps gave the same Gboard suggestions as the fast
  text-fill method in 27/30 cases (the 3 differences were single words, no direction).
- Limits: one Gboard version (the clean-room study looked at 18.5); 100 sentences per language; Common
  Voice text is read-aloud style (a web-text set is running too).
- Our completion figures above come from the n-gram file filtered by typed letters (scratch tool). The
  keyboard's real path was `Recognizer::complete()`: dictionary frequency only, **no sentence context**.
  Measured on the same Gboard queries (2 guesses, after 1 letter, everyday / web): Spanish 33 / 35,
  French 30 / 27, German 26 / 25 — 10–17 points behind Gboard. **Fixed 2026-10-22 (b805430):**
  Spanish/French/German completion now ranks by the sentence through the n-gram pack (English unchanged
  until it has one). Gboard comparison through the real code is `bench/scoreboard.sh`.

Swipe, first guess right: English 93.3% (94.5% opt-in AI mode) on 2,000 real swipes (FUTO's MIT test
split). Spanish/French/German: unknown. Gboard swipe: not measured.

## Upstream moved (verified 2026-10-22)

Already decided as card #181 (Destin 2026-10-22): rebuild on the new core and offer upstream AFTER the
FUTO-free models (#179); if Plasma 6.9 ships first, port first and upstream after. Not reopened here.

plasma-keyboard replaced Qt Virtual Keyboard with its own input engine (MR !109, merged 2026-10-15). Our
`plugin/` is a Qt Virtual Keyboard input method, so it works with today's released plasma-keyboard but not
with the next one, and cannot be what we offer upstream. Upstream's own plan: prediction as a new
`AbstractTextComposer` (issue #77, Presage idea), swipe typing as work item #5. Our `engine/` (prediction,
completion, context) carries over; the integration layer must be rewritten against the new engine. Public
research: `docs/active/investigations/2026-10-22-kde-upstream-research.md` (maintainers not contacted, by
Destin's choice). Also from it: keep weights/data out of git and download on demand (digiKam does this via
files.kde.org), keep the keyboard fully usable with no pack, make any ML runtime optional, Qt 6.11 / KF6
6.30 floors, `i18n()` strings, clang-format; KDE has no AI-contribution policy yet (a draft was withdrawn).

Pack size (Destin 2026-10-22: shrink + separate download): XS packs (t2=20/t3=40) are 23/24/29 MB vs
40/42/51 MB, cost ~0.5–1.5 points, and stay ≥ Gboard everywhere (worst: German everyday after 3 letters,
level); XXS (13–16 MB) falls behind Gboard in German.

## License: follow KDE (decided by Destin 2026-10-22)

Goal: eventual merge into KDE's plasma-keyboard, so the project uses KDE's licensing policy
(community.kde.org/Policies/Licensing_Policy):
- Keyboard/plugin code: `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL` (KDE's standard;
  plasma-keyboard's own files use it, with some GPL-3.0-only parts derived from Qt).
- Engine library (recognition, prediction): `LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL`.
- CMake files: BSD-2-Clause or BSD-3-Clause. Docs and images: CC-BY-SA-4.0. Trivial/generated data: CC0-1.0.
- REUSE-compliant: every file has an SPDX header; `LICENSES/` holds exactly the licenses used.
- KDE's policy says nothing about model weights or training data: ask the plasma-keyboard maintainers early.

Why FUTO still has to go: its model weights use FUTO's own license (mandatory "powered by FUTO Swipe"
notice, Texas-law contract terms) that is not a standard open-source license; FUTO's code is GPL-3 with no
"or later" notice in its files, which does not fit KDE's GPL-2-or-3 standard; and it pulls in ExecuTorch.

Facts verified in code:
- The plugin includes and links Qt Virtual Keyboard (`plugin/CMakeLists.txt:1`,
  `plugin/swipeinputmethod.h:7`), as plasma-keyboard itself does.
- `engine/src/recognizer.cpp` includes FUTO headers (lines 17–19) and reuses FUTO's path-fit formula
  (~53) and raw-shape guard (~68): it is FUTO-derived and must be replaced, not just unlinked.
- Data: FineWeb-2 (ODC-BY), Tatoeba (CC-BY 2.0 FR), AOSP lists (Apache-2.0) are attribution-only.
  wordfreq is CC-BY-SA (share-alike) — replace with word lists counted from FineWeb-2 + Tatoeba.
  Common Voice sentences (CC0) are test-only.

## Phase 0: facts first (≈1 week, items run in parallel)

| # | Item | Why early | Done when |
|---|---|---|---|
| 0.1 | **Authorship audit + upstream contact.** Mark every file clean / FUTO-derived / unsure. Open a conversation with the plasma-keyboard maintainers (shape of a merge: plugin vs built-in, model/data downloads, ExecuTorch-free requirement). | Their answers can change Phases 3–4; asking costs a message | File list committed; maintainers' answers recorded |
| 0.2 | **Geometric swipe spike** (2–3 days): a simple shape scorer with no neural model (the approach the published papers describe), on the English real swipes. | Decides whether our own swipe engine is ~2 or ~6 weeks, and whether we need trained weights at all | Score vs FUTO's 93.3% recorded |
| 0.3 | **Commit the Gboard rig** and freeze its results as reference files; add English; web-text set. | Gboard doesn't change; one-off cost | `bench/reference/` committed |
| 0.4 | **Scoreboard command** (`bench/scoreboard.sh`): next word, completion through the real code path, swipe, speed, memory, file sizes, per language; `--check` with the noise tolerance. | Every later change is judged by it | One command, < 15 min |
| 0.5 | **Real swipe test set, small:** 200–300 swipes per language from people who speak it, using the existing opt-in recorder (`plugin/swiperecorder.h`). Needs volunteers (Destin to find). Until then, synthetic swipes (generated from text with a noise model fitted to the real English swipes) give a provisional number. Test and training swipes use different generators. | Multilingual swipe is the biggest unknown; synthetic alone can flatter a decoder | ES/FR/DE swipe scores recorded (provisional, then real) |
| 0.6 | **Steam Deck / ARM check:** speed and memory of today's keyboard and the new Spanish file. | Size/speed budgets must be numbers, not "agreed later" | Budget table: MB per language, ms per suggestion |

**Checkpoint 0:** one table — 4 languages × {next word, completion, swipe, speed, size} × {ours, Gboard
where measured} — rebuilt by one command; maintainers asked; spike result known.

Phase 0 results (2026-10-22):
- 0.1 audit done: `docs/active/investigations/2026-10-22-authorship-audit.md` — 53 FUTO-derived files;
  the n-gram, context, spelling and normalising code is clean; ~60% of `recognizer.cpp` is FUTO glue; its
  default ranking weights equal FUTO's `scoring.json`. No SPDX headers anywhere yet. Maintainer contact: not
  sent (Destin to approve the message).
- 0.2 spike done (clean room, `RESULTS.md` kept in the session scratchpad): geometric decoder (DTW on key
  positions + letter coverage + frequency) = **82.0% first guess / 90.0% top 3** on the 2,000 real English
  test swipes vs FUTO's 93.3 / 96.6; ~23–46 ms per swipe in numpy. Expected ~86–88% with sentence context.
  Conclusion: geometry alone will not reach 93%; Phase 3 needs a small trained model (re-ranker over the
  geometric top ~20), so plan for the upper end of the estimate.
- 0.3 done: `bench/gboard/` (probe, tap check, frozen answers).
- 0.4 done: `bench/scoreboard.sh` (~2 min, 3 languages; swipe row when the swipe bench is built).
- 0.5 provisional done (c2978a2): synthetic swipes with noise fitted on real English dev swipes, scaled
  until synthetic English matched real (92.3 vs 92.8). Today's keyboard (FUTO engine), 400 words each:
  Spanish 85.0 / 98.2, French 82.8 / 93.2, German 86.8 / 96.5 (first guess / top 3); optimistic
  (dictionary words only). Main misses: accent twins (que/qué, ou/où) and words of ≤3 letters — both
  need sentence context. Real volunteer swipes still needed.
- 0.6: no Steam Deck/ARM device known; open question for Destin.

## Phase 1: quick wins (≈3–4 days)

- New web+Tatoeba suggestion files for French, German **and English** (English replaces FUTO's model, so it
  is also FUTO-removal work). Word lists counted from the same text, replacing wordfreq.
- Profanity filter on suggestions (never offered unless typed); no suggestions in password fields
  (keep `safePredictionContext()` behaviour, add a test).
- Cheap previous-word fix: after the next word, re-check the last swiped word with the n-gram file and swap
  only on a clear margin (undo-able).
- Ship together with the 6× faster suggestion code already on `feature/170-open-nextword`.

Item status (2026-10-22):
- Files: done for all four (published, installer fetches them). Word lists still wordfreq-derived — open.
- Swear words (Destin's choice): never offered as an unprompted next word; swiping and completing them
  still work (fd59e68). Password/private/hidden fields: test added (9e3cc55); four separate checks guard it.
- Speed: next word scores only plausible words, identical results, English 34 → 6.5 ms (b01db92).
- Previous-word fix: dropped (Destin 2026-10-22: no automatic swaps behind the cursor). Sentence-aware
  swipe search (Phase 3) and learning (Phase 2) address the same errors; tap-a-word-to-swap is its own card.

Status 2026-10-22: Spanish/French/German web+Tatoeba packs built (FineWeb-2 300M words + Tatoeba ×30,
t2=10/t3=20; `~/.cache/swipe-keyboard-train/LANG/mix30-S`, 40/42/51 MB vs 6/8/9 MB today). Scoreboard
(`bench/scoreboard.tsv`): ahead of Gboard on every query type in all three languages (+1.6 to +12 points;
web sets are partly home ground), no loss on Tatoeba beyond noise. Not yet installed into packaging;
Corpus-derived word lists still to do.

English pack built 2026-10-22 (FineWeb English 10BT sample shard 000 + Tatoeba ×30; shard 001 = web test
set; `~/.cache/swipe-keyboard-train/en/mix30-{S,XS}`, 65 / 38 MB, 232k words). Next word, right word in
top 3 (300 sentences per set): FUTO's neural model (today) everyday 20.0 / web 23.0 / Tatoeba 22.9; ours XS
25.9 / 26.1 / 40.5 (S the same within 0.3). English real swipes with our n-gram in place of FUTO's model:
93.3 → 93.1 (XS) / 93.3 (S) first guess — no change beyond noise (single-word swipes carry no sentence).
Gboard English reference: running.

Published 2026-10-22 (Destin approved): GitHub pre-release `language-files-2026-10-22` — XS packs for
en/es/fr/de, Tatoeba credits (`scripts/lm/tatoeba-credits.py`), licence texts, NOTICE
(`packaging/language-models/LANGUAGE-FILES-2026-10-22-NOTICE.md`), SHA256SUMS; download re-verified.
Done on `feature/170-open-nextword`: installer/packages fetch them by checksum (like the FUTO files), profiles pin them,
English switches to its n-gram (FUTO's model stays as the fallback); a missing file only removes suggestions. In-keyboard download UI waits for #181.

**Checkpoint 1:** every language ≥ Gboard on completion after 1 letter (2-guess count); no swipe
regression; within the Phase 0 size/speed budget.

## Phase 2: learn from the user (≈1–2 weeks; can start any time, independent of swipe)

Local only: often-typed words, new words (names, slang) after a few uses, favourite word pairs; decays over
time. Never learns from password/private fields; stored in the user's own folder; one-tap reset and
delete; no network.
Measured with a rough personal test: Tatoeba names each sentence's contributor — learn from a contributor's
first half, test on the second (measures repeated vocabulary, not real typing; a rough bar only).

**Checkpoint 2:** ≥ 5 points better on that test after 1 letter, every language; no loss on the generic
scoreboard; reset verified to erase everything.

## Phase 3: our own swipe engine (≈2–6 weeks, sized by the 0.2 spike)

- 3.1 Search (finding words that fit a path, with previous words used during the search), written from a
  design doc by a clean-room writer.
- 3.2 Shape scoring: the spike's geometric scorer if it is close enough; otherwise a small trained model
  on the 4070 from FUTO's MIT real English swipes plus synthetic ES/FR/DE (training generator ≠ test
  generator), with our own small runner (no ExecuTorch).
- Runs beside FUTO behind a setting until it wins.

**Checkpoint 3:** English real swipes ≥ 93.3% first guess (or Destin accepts a stated trade-off, e.g.
smaller download); ES/FR/DE ≥ FUTO's Phase 0 numbers on the real sets; within speed/size budget.

## Phase 4: remove FUTO (≈2 days)

Delete `third_party/swipe-library`, FUTO downloads (`packaging/assets.lock.json`), ExecuTorch and the
FUTO notice; apply KDE's licenses with REUSE headers (`reuse lint` clean); data license manifest
(attribution file per data package); follow KDE coding style where it differs.

**Checkpoint 4:** no FUTO file or dependency anywhere; full scoreboard green; card #179 done.

## Not now (revisit only if numbers ask)

Small neural (~5M) next-word model if n-gram files plateau below goal after Phases 1–2; newer Gboard
comparison; more languages; capitalization/apostrophe output checks beyond what the scoreboard catches.
