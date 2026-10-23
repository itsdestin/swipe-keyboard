---
status: draft
cards: 179
---
# Own swipe engine: geometric shape decoder + small learned re-ranker

Written without reference to FUTO code, weights or documents. Sources: published literature only (Kristensson & Zhai,
SHARK2, 2004; dynamic time warping; Gaussian key models) plus a clean-room spike (82.0% first guess / 90.0% top 3 on the
2,000 real English test swipes, numpy). Training/test data: FUTO's MIT-licensed swipe recordings (data only).

## 1. Problem and pipeline

Input: a trace (x, y, t) in layout coordinates (0..1 of the letter area), canvas pixel size (for aspect), a layout (key
rectangles) and a word list (spelling, frequency 0..255). Output: ranked words with scores.

1. **Normalise.** Scale to key-width units: X = 10x, Y = 10·y·H/W (so a QWERTY key is ~1.0 wide, ~1.3 tall). Drop
   consecutive points closer than 1e-4. Resample the trace to N=40 points equally spaced along its length.
2. **Candidates.** Take the 6 keys nearest the first touch and the 6 nearest the last touch; candidates are words whose
   first letter is in the first set and last letter in the second (bucket table keyed by first/last key). Then drop words
   whose path length is outside 0.55·L−1 .. 1.8·L+1.5 (L = trace length; skipped for taps).
3. **Template.** A word's template is the polyline through its key centres (consecutive repeated letters collapsed),
   resampled to N=40 equal-length points (built on demand per candidate, so memory is just the word list).
4. **Geometric score (lower is better)** = DTW/80 + 0.01·coverage − 0.0005·frequency:
   - *DTW (location channel):* banded dynamic time warping (half-band 8 samples) between trace and template. Point cost
     is max(0, distance − 0.45) key widths: a touch inside a key's core is free. This lets users speed up/slow down and
     cut corners, which the SHARK2 point-by-point match cannot (spike: 66% → 85%).
   - *Coverage:* for each letter of the word, the trace's closest approach to that key, minus 0.4, floored at 0; summed.
     A letter the finger never visited costs its full miss distance (DTW alone dilutes it).
   - *Prior:* frequency, linear (the list's scale is already log-like).
   - Exactness-preserving speed-ups: candidates are visited in order of (coverage − prior); DTW rows are abandoned as soon
     as the best reachable total exceeds the current 24th-best; the loop stops when the cheap terms alone exceed it.
5. **Re-rank (section 3).** The best 24 are re-scored by a small boosted-tree model on richer features. Without a model
   file the geometric order is returned unchanged.

## 2. Interfaces (engine/include/swipekb/shape_decoder.hpp)

- `ShapeLayout`: keys as `{char32 label, cx, cy, halfW, halfH}` in 0..1, plus `alias(from, to)`. `ShapeLayout::foldLatin()`
  folds accented letters to their base key when the layout has no key of their own (é→e, ñ→n only if no ñ key).
- `ShapeDecoder::setLayout`, `loadWords(path | vector)`, `setReranker(path)`, `decode(trace, canvasW, canvasH, topK)` →
  `vector<ShapeCandidate{ word, score, geoScore }>`. Word list input is a plain TSV (`word<TAB>freq`) so the list is
  pluggable; words with characters that map to no key (digits, apostrophes, hyphens) lose those characters ("can't"
  is matched as "cant", displayed as "can't"). Duplicates by key string keep the most frequent display form.
- Context hook (not wired yet): `decode(..., contextScore)` takes an optional per-word callback returning a sentence-context
  log-score; it becomes one more re-ranker feature (`NgramPredictor`-style score of the word after the previous words).
  Single-word swipe data cannot train or measure it, so it is simulated later.

## 3. Learned re-ranker

Why: geometry alone has a ceiling (spike: 82/90; 8% of swipes have the right word at rank 2–3, mostly near-identical
paths where only frequency/timing separate them; ~8% are sloppy paths).
- Model: gradient-boosted trees (LightGBM, trained offline, build-time tool only) on the geometric top 24, exported as
  flat arrays and evaluated in plain C++ (no ML runtime). Objective: lambdarank / softmax over the candidate list.
- Features per candidate: geometric score parts (DTW, coverage, frequency), rank and gap to best, lengths and length
  ratio, start/end distance to first/last key, open-end/trimmed DTW (sloppy lift-off), per-letter alignment misses
  (max/mean/count of letters missed), double-letter flag and dwell at the doubled key, speed at letters and number of
  slow points/corners versus letters, frequency rank among the 24, how many candidates share the same key string,
  trace-level context (duration, length, best DTW = how sloppy this swipe is).
- Training data: dev.jsonl (54k real swipes, other users than the test sessions); selection on devhold-5000 and tune sets;
  bench-2000 and test-extra-10000 are scored once at the end.

## 4. Other layouts and alphabets

Nothing is QWERTY-specific: the layout is the key list. AZERTY (fr), QWERTZ (de) and QWERTY-ñ (es) are different key
rectangles. Accents map to the base key (é/è/ê→e, ü→u, ç→c, ñ→n or its own key), so French and German words and
Spanish words are matched on their base letters; accent choice is left to the word list/frequency/sentence context
(the que/qué, ou/où twins are a context problem, not a shape problem). Alphabets with different key counts only change
the 26-key assumptions in the bucket table (sized from the layout). Scripts with long-press variants, or space-bar
and punctuation keys inside words (apostrophes in French l'école), are handled by dropping unmapped characters.
Constants are in key-width units, so they transfer to other key sizes; they should be re-tuned (and the re-ranker
retrained) on real swipes per language. Only English has real recordings today; ES/FR/DE will start from synthetic
traces (generator ≠ test generator).

## 5. Constants (starting values from the spike, tuned on tune-2000 only)

N=40, band=8, r=0.45, coverage radius 0.4, coverage weight 0.01, prior weight 0.0005, nk=6, candidate list K=24,
length window 0.55/1.8. Targets: ≥ spike accuracy (82/90 geometric) and ≤ 10 ms per swipe single-thread.
