# How Gboard's swipe typing works (clean-room report, reviewed CLEAN 2026-10-14), and how it compares with FUTO Swipe

Clean-room study report. It describes ideas, not code: no code, disassembly, weights or dictionary data;
everything is in the study agent's own words.

Confidence labels: **Confirmed** = seen directly in the program's own code or call flow. **Likely** = strongly
suggested by names, structure or published papers, not traced end to end. **Guess** = informed inference.
Each item also names the kind of evidence behind it.

## 0. What was studied

- Gboard 18.5.1 beta, build 176049542, arm64, package `com.google.android.inputmethod.latin`, pulled over adb from
  Destin's phone (installed from Google Play). SHA-256 prefixes: base d6ac4708f59f6008…, brella split
  4c0879b3f67ef4f4…, config.xxhdpi 6d44c25ec7754ee2…, dictation 6393c79ef372816b…, tenoranimation ab05ac406cf3df0d….
- Static only: jadx on the Java side; full Ghidra auto-analysis of the 35 MB native library (~35 min), then
  decompiling specific functions found through error-message/source-path strings; string and symbol analysis;
  published Google papers; the FUTO paper and library. Phone not touched; nothing run; no models downloaded.
- Private working files: `work/` (decompiled output, Ghidra project, excerpts) and `notes/` (flag list, source paths).

**Headline.** Gboard's default Latin swipe decoder is not one big neural network. It is a hand-built geometric
scoring model, decoded by a finite-state search that blends a lexicon, an n-gram LM, personal LMs and optional
neural LM rescoring. The geometric model works like a hidden-Markov alignment: each finger position is scored as
"sitting on key X" or "travelling from key A to key B", using direction of travel, sharp corners and pauses. The
2017 neural spatial model (LSTM+CTC) still has a slot in the language-pack format, now marked obsolete. A newer
neural transducer decoder is built in but switched off by default. Gboard's edge over FUTO comes mostly from the system around the recogniser: sentence context inside the
search, per-user learning, streaming decoding, correcting the previous word after the next one, and careful
decisions about when a touch counts as a swipe.

## A. Map

- A1 (confirmed: symbols, source paths, JNI table). Almost everything is in `libintegrated_shared_object.so` (35 MB,
  stripped, NDK r29), bundling the keyboard decoder ("Delight5"/"runtime5") with unrelated engines (speech,
  handwriting, Mozc, emoji search…). The 18 KB JNI libs are stubs. Java class `Decoder` (delight5 package) talks to
  native through ~60 native methods exchanging serialised protobuf messages.
- A2 (confirmed: source paths). Gesture path areas: touch-sequence feature extraction ("base"); gesture spatial
  scoring + adaptive-scale helper; FST keyboard decoder ("fst": keyboard-weight combiner, bi-key model, two-stage
  decoder, dynamic LMs, neural rescoring); streaming + personalised decoder ("runtime5/fst"); gesture revert adapter
  and user-history learner ("runtime5"); per-user statistics cache ("userfeaturecache": spatial, typo, gesture
  stats). Alternatives in the binary: "tnt" (neural transducer, LiteRT model, own spatial model); "neural_decoder"
  (encoder-decoder "NSM", tap and glide search, apparently for transliteration); "neural_search_space" (neural LM
  inside the search). A separate HMM/LSTM gesture engine ("i18n/input/engine/gesture") serves Chinese and Korean,
  not the Latin path.
- A3 (confirmed: section-name table). Models download per language as a "KLP" file (v2/v3, lazy-loaded sections):
  lexicon FST, LM FST, literals FST, result-transform FST, key and word symbol tables, parameters blob; LM weight
  discounter, supplemental LM FST; `spatial_model_features`; neural rescoring model, NN LM (+ word symbols),
  sentence-piece NN LM; neural decoder models, TNT model data; small TFLite key-correction models (general,
  common-key, spacebar, punctuation, backspace, "smartkeys" heatmap). Several sections marked obsolete, notably the
  old neural spatial model. FST arc weight is a pair of tropical weights — likely spatial and language cost carried
  separately so they can be re-weighted (the keyboard-weight combiner).
- A4 (not available). Models live in private app storage. Likely sizes from Google papers: n-gram LM a few MB
  (~64k words in n-grams, ~170k unigrams); neural LMs TFLite LSTM/CIFG ~1–1.5M params. `libtensorflow_jni.so` in the
  brella split is likely federated learning, not decoding.
- A5 (confirmed: Java). A gesture motion handler collects touch points, decides when a touch becomes a swipe, draws
  the trail and sends "in progress"/"ended" events; the decoder wrapper turns these into native decode calls.

## B. Input processing

- B1 (confirmed: Java). Every touch sample is kept, including Android's batched history samples: pointer id, x, y,
  ms since gesture start, action type. Pressure only with a raw-logging flag. Distance filter: a sample is stored
  only if ≥ 0.1 key widths from the previous one (relaxed briefly around each streaming update). No time-based
  resampling in Java.
- B2 (confirmed: Java and native). While the finger moves, points go to the decoder about every 100 ms; final call
  on lift. A newer request cancels an unfinished older one. Native processes only new points, recomputing the last
  few (features look one point ahead). Effect: live best guess (floating preview), result ready almost at once on lift.
- B3 (confirmed: decompiled feature routine). Per point: position in keyboard pixels and the key it lies on;
  direction of travel; turning angle (abs change of direction, folded to 0–180°);
  time gap to neighbours; running path length.
- B4 (confirmed). Corner = turning angle is a local peak above a threshold. Pause = time gap is a local peak above
  a threshold. A running count of these landmarks lets the decoder ask how many corners lie between two points.
- B5. Confirmed: no rescale to a unit box — distances divided by key width and key height separately; on wide keys
  (space) distance is to the key's centre line, not centre point; per-axis Gaussian widths come from configured
  rules interpolating on keyboard-geometry ratios. Likely: default sample spacing 0.05 key widths. Confirmed:
  travel-direction scores are weighted by distance covered per point, not point count → insensitive to sampling
  rate and swipe speed.

## C. Recognition model

- C1 (likely; mechanism confirmed). Each point gets scores over two hidden-state kinds:
  1. "On key k": Gaussian log-likelihood of distance to key k (separate x/y, key units); penalty if not physically on
     the key; floor on the per-point spatial score (default −9); first point and lift point have their own weights
     (both default 1.0).
  2. "Travelling a→b" (bi-key): minus the squared angle between travel direction and the a→b direction; only pairs
     whose direction is near the travel direction considered, others capped; weighted by distance covered; extra
     term proportional to turning angle at corners; extra term at pauses; at slow points the direction is blended
     with the previous one to suppress jitter.
  The decoder aligns a word's letters to the trace: each letter "visited" with good on-key scores, travel between
  letters in the right direction, corners/pauses pushing letter placement there (a likely penalty
  for passing a corner without placing a letter). Matches Google's 2017 FST paper ("on key"/"in transit"). In essence the score rewards moving in the direction
  of a key pair and treats corners, pauses and slow points specially; the exact form is for the rebuild team to
  design and tune on its own data (K2, K8, K9) — this list is not a specification.
- C2 (mechanism confirmed). Adaptive score scale: spatial weight can decay along the gesture; several distance-since-corner
  measures; linear or base-and-ratio decay (defaults 0.9 /
  0.1). Off by default in Java; server or language pack can enable.
- C3. TNT (likely): neural transducer, RNN-T-like, searched over the same FST, continuous-decoding option,
  disabled by default. neural_decoder (guess): transliteration. No CTC network in the default Latin path; the
  2015/2017 BiLSTM+CTC (~1.5M params, 34 features/point) is the predecessor, section now obsolete.
- C4 (confirmed: Java). Decoder returns a ranked list; Java asks for 10; each candidate carries separate spatial and
  language scores.

## D. Decoding

- D1 (likely; structure confirmed). Weighted-FST beam search: static lexicon FST (key sequence → word, bi-key
  context) composed on the fly with the n-gram LM FST (compact/pruned trie FSTs, look-ahead composition). Spatial
  and LM costs travel as a pair, combined by the keyboard-weight combiner. Parameters: local beam, histogram pruning,
  must-end-in-final-state choice, graph expansion; a path-based state pruner for Latin. Beam numbers come from the
  language pack or server, not code.
- D2 (likely). Final score = spatial cost + weight × LM cost; separate weights/offsets for literal hypotheses,
  user-history candidates (threshold −2.0), dynamic LMs, email fields. Multi-language switching costs adapt to
  usage (scale 1, cap 8, unknown-word −15).
- D3 (exist; off in Java defaults). First FST pass → candidate list; neural LM (LSTM/CIFG or subword) rescoring, or
  inside the search; Google reports production use (2024).
- D4. Double letters (likely): optional repeated key in the lexicon; a flag merges duplicate-key scores. Long words:
  no fixed-length bottleneck; scores accumulate along the real path. Short words (guess): strong first/lift anchors
  + corner logic; start rules stop tiny one-key swipes. Multi-word swipes (likely): optional space in the lexicon.
- D5 (likely). Taps keep a literal candidate; gestures don't produce out-of-vocabulary output.

## E. Language model

- E1 (likely). Static n-gram LM as FST in the language pack (published: Katz 5-gram ~1.25M n-grams, 164k unigrams
  (2018); ~64k/170k (2024)).
- E2 (present; likely used). TFLite RNN LM (LSTM/CIFG), sentence-piece subword NN LM, neural rescoring model; local
  force-on flags default off (server enables).
- E3 (confirmed). Native decoder owns the text context (committed words, spans, their source: tap, gesture,
  suggestion, auto-correction…). Preceding words condition every swipe decode through the LM state in the search —
  the main context advantage over re-ranking-only.
- E4 (confirmed: names). Intrinsic capitalisation (capitalisation transducer + capitalisation LM, or TFLite model);
  compound-word capitalisation rules; result-transform FST; manual capitalisation protectable.
- E5. Mid-sentence punctuation flag; apostrophes in the lexicon ("dont" → "don't"); small TFLite punctuation models.

## F. Personalisation

- F1 (confirmed: names). n-gram user-history LM (mutable trie map); learner adds/increments n-grams, holds
  "intermediate" n-grams until text is final, can forget, flushes to disk, prunes when large. Likely per-n-gram
  timestamps (recency).
- F2 (confirmed: enum names). LM sources: contacts, user history, personal dictionary, shortcuts, email; contacts
  boostable; an unknown word appears to need ~4 uses before counting as intended (guess).
- F3 (confirmed: names). Per-user stats cache: auto-correction reverts, typo stats, per-key tap spatial stats,
  gesture stats, gesture-revert stats (rolling windows: reverted yes/no + two numeric per-swipe windows). Likely
  feeds the adaptive gesture score scale (C2) → more or less forgiving per user. Likely tap typing personalises key
  centres (2022 paper).
- F4 (likely). Federated learning shapes shipped models, not on-device decoding.

## G. Product behaviour

- G1 (confirmed: Java). Swipe detection: fast-move check above ~1.5 key widths/s; minimum distance 0.5 key widths;
  minimum time ~20 ms. Rhythm-aware: if some of the last 3 inputs were taps and the last key press was within
  650 ms, thresholds rise with recent taps (up to 1.5 key widths and 100 ms per recent tap), relaxing linearly over
  650 ms; after 2 swipes within 30 s the relaxed thresholds apply at once ("swipe mode"). Horizontal space-bar drag
  (slope < 0.4) is cursor control; swipes from most non-letter keys excluded. Optional checks off by default. Trail
  fades over ~1 s and tapers.
- G2 (likely). Auto space between swiped words, probably added when the next swipe starts; punctuation after a swipe
  swaps with the auto space.
- G3 (likely). Backspace deletes the whole swiped word; reverts logged as gesture-revert stats.
- G4 (partly confirmed). 10 candidates requested. Post-correction can replace the previous word after the next word
  arrives (spatial and language thresholds, minimum-improvement rule, can ignore single-letter words). Order of the
  3 visible words not confirmed.
- G5. Shift/caps carry into the gesture result; revertible shift mode.

## H. Tunables (meanings; Java defaults, server can override)

- Gesture spatial score: worst per-point score −9; first/end key weights 1.0; key-error sigma (0 = language pack);
  override scale 1.0; decay mode off, base 0.9, ratio 0.1; skip-last-frame off; corner skip score.
- Sampling: 0.05 key widths default sample distance; Java distance filter 0.1 key widths.
- Gesture start: min time/distance (0 = built-ins), key-edge threshold, min tap-to-gesture interval, bottom-row zone.
- Search: tap beam adjustment and decay; prediction branching limit; max/min words predicted 3/2; next-word beam 10.
- Post-correction: spatial and language thresholds (offset + slope each); minimum improvement; always-post-correct
  off; reject non-trivial post-corrections on.
- Personalisation: gesture-revert stats config, learning strategy, user-history threshold, contacts boost,
  dynamic-LM noise filter.
- Decoder choice: TNT off; TNT continuous off; two-stage off; neural on-the-fly rescoring off; neural search-space off.

## I. Comparison with FUTO Swipe

FUTO: path resampled to 60 Hz then a fixed 64 points (x,y only); 635k-param dilated CNN (any layout), per-frame key
probabilities trained with CTC; optional QWERTY refiner; trie beam search (width 100) scoring CTC cost + length bonus
+ 0–255 frequency; optional word-level neural LM re-ranks the top 200 afterwards; no learning, no punctuation/capital
handling, no streaming; built-in trie drops apostrophes and capitals.

| Area | Gboard | FUTO | Likely impact |
|---|---|---|---|
| Input | Every sample, distance-weighted, direction/turn/pause/corner features, real key sizes, wide-key handling | Fixed 64 points, key centres only | Medium: FUTO loses resolution on long/slow words |
| Recogniser | Engineered alignment HMM; neural TNT available | Learned CNN + CTC | Mixed: FUTO probably a better raw shape matcher; Gboard's lead is elsewhere |
| Decoding | Context LM + lexicon **inside** the beam | LM **after** the beam | **High**: dropped words can't be recovered in FUTO |
| LM | 5-gram class + neural rescoring + user LMs | Frequency + optional 32k neural re-rank | **High** for ambiguous pairs |
| Personalisation | User n-grams, contacts, personal dict, revert stats, adaptive scale | None | **High** over weeks |
| Post-correction | Revises previous word using the next | None | Medium (paper: 11.5% → 9.2% gesture error) |
| Swipe start | Rhythm-aware, space-bar exclusion | Host keyboard's job | Medium for perceived accuracy |
| Streaming | Every 100 ms, preview | On lift | Feel, not accuracy |
| Output formatting | Apostrophes, capitals, punctuation spacing | Lowercase, no apostrophes | High for perceived accuracy |

## J. Top-5 ideas to rebuild (ranked)

1. **Sentence context and dictionary inside the search.** Score each beam hypothesis as recogniser cost + weighted
   n-gram LM (2–3-gram over previous 1–2 words) while expanding; keep spatial and LM as a pair, tunable per user.
   High benefit; medium effort (1–2 weeks); a trie + n-gram lookup per word end suffices, no FST library needed.
2. **Personal learning loop.** User word pairs and accepted words as a second LM with recency decay; learn unknown
   words after a few uses; "swipe immediately erased" signal adjusts forgiveness per user. High over time; medium
   effort (1–2 weeks) + privacy-safe local storage.
3. **Revise the previous word after the next one.** Re-decode the previous swiped word with right-hand context;
   replace only above a confidence margin and if the user hasn't edited it. Medium-high (~20% relative cut in
   gesture error per Google); ~1 week; needs undo/revert tracking.
4. **Path-aware alignment features alongside the CNN.** Corner/pause detection; distance-weighted direction-vs-key-
   pair score; true key-size Gaussians; centre-line distance for wide keys; as a second scorer blended with FUTO's
   CTC score or as extra inputs; stop squeezing long swipes into 64 points. Medium; 2–3 weeks incl. tuning on
   FUTO's open dataset.
5. **Rhythm-aware swipe detection + streaming preview.** Raise thresholds right after fast taps, relax over ~650 ms;
   "swipe mode" after 2 swipes in 30 s; never treat horizontal space-bar drags as swipes; decode every ~100 ms with
   a preview, cancelling stale decodes. Medium; low effort (days).

Honourable mentions (low effort): whole-word backspace after a swipe; auto space before the next swipe, swapped
away by punctuation; apostrophe and capital restoration from real word forms.

## K. Unknowns and phone black-box test plan

Unknown statically: beam sizes, LM order/size, sigma (language pack/server); whether TNT, neural rescoring or decay
are on for English; exact spacing/punctuation/strip ordering; how revert stats change scoring.

Setup: phone over adb, Gboard in a blank plain-text field of a simple notes app; record version. Inject swipes via
`input motionevent DOWN/MOVE/UP` (or `sendevent`, or a small instrumentation app); read results with
`uiautomator dump` or the clipboard; calibrate key centres from a screenshot; repeat ×10, randomised; fresh profile
(clear Gboard data) and personalised suggestions off for non-learning tests.

- K1 Swipe-start thresholds: drags 0.2–1.5 key widths at 0.5–5 key widths/s; after 3 fast taps and 1 s later.
- K2 Corners/pauses: "pin" vs "pain"/"pan" with/without 150 ms pause on "i"; sharp vs rounded corners "cat"/"cart".
- K3 Context: identical trace for ambiguous pairs (in/on, of/if, pit/pot/put) after different preceding words.
- K4 Strip: record all 3 candidates for 50 ambiguous swipes.
- K5 Post-correction: ambiguous word then disambiguating next word.
- K6 Spacing/punctuation: swipe, then tap . , ? ' and swipe again; inspect exact characters.
- K7 Backspace: swipe + backspace; swipe, letter, backspace.
- K8 Speed/sampling: same path at 0.5×/1×/2× speed and 60/120/240 Hz.
- K9 Long words and doubles: 12–15-letter words precise vs sloppy; letter/later, too/to, happy/hapy.
- K10 Learning: made-up word corrected 1–6 times; ambiguous pair always picking the 2nd choice ×10.
- K11 Revert adaptation: swipe-and-backspace ×20, then retest K3/K9.
- K12 Streaming: 60 fps screen recording of slow swipes.

Keep every injected path and output as our own private evaluation dataset.

## Sources (published)

Alsharif et al., ICASSP 2015; Ouyang, Rybach, Beaufays, Riley 2017 (arXiv 1704.03987); Google AI blog 2017 "The
Machine Intelligence Behind Gboard"; Hard et al. 2018 (arXiv 1811.03604); Sivek & Riley 2022 (arXiv 2209.11311);
Zhang et al. 2024 (arXiv 2410.15575); FUTO Swipe (arXiv 2606.25247) and its open library.
