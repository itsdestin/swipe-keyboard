# Offline language fixtures (not language certification)

`synthetic-centered-v1.json` is reproduced by `python3 bench/languages/make-seeds.py`.
These 30 hand-authored seeds have **not** been reviewed by native speakers. They are
not human swipes, a corpus sample, or a held-out accuracy estimate. No thresholds
are attached. `zzqnonword` is deliberately dictionary-absent; it remains a miss.
Other misses, including accents, apostrophes, inflections and German capitalization,
are retained. Do not tune/filter the recognizer to make this scoreboard look better.

Geometry is an authored three-row 100-pixel grid, with an extra Spanish ñ. It is
not a capture of stock/device layout. `traceKeys` explicitly chooses measured
base keys for accents/ligatures and omits apostrophes: e.g. `l’école` → `lecole`,
`Straße` → `strase`. Each segment has eight equally spaced points 16ms apart.
No timing/quality realism is implied by these choices.

Run a built `swipekb-language-bench --dataset <json> --model-root <verified English root>
--packs-root <verified packs root>`. The program loads actual verified profiles and
FUTO recognizers, with empty context, ten candidates and shared regional/text rules.
It emits per-case candidates and failures, dictionary identities, per-profile/layout
counts, and process CPU recognition time (not wall time, not a latency budget).
No runtime status is queried. Normal and lightweight CTest include deterministic
accounting and CLI validation tests; a baseline is an explicit offline command.

## Explicit supplied human fixtures

Provide a JSON object `{"schema":1,"cases":[...]}`. Every case requires unique `id`,
`locale`, physical `layout` (`qwerty`, `azerty`, `qwertz`), `target`, boolean
`nativeReviewed`, `provenance`, and explicit `keys`. Keys are objects with one
lowercase Unicode scalar `letter` and finite pixel `x,y,w,h` (positive dimensions).
Coordinates share a single keyboard-local system. Ordered ASCII key positions
must match the claimed layout; complete profile alphabet/popup coverage is required.
Do not infer geometry from locale. Malformed/mismatched cases are `invalid`, not
fabricated or silently skipped.

For a consented, explicitly supplied trace set `provenance: "supplied-human"`,
`points: [{"x":50,"y":50,"t":0}, ...]`, and **no** `traceKeys`. At least two finite
points, nonnegative monotonic milliseconds, and at most 10000 points are required.
For synthetic cases use `provenance: "synthetic-centered-v1"`, `traceKeys`, and no
`points`. This evaluator never records typing, reads logs/history, or fetches data.
Do not put private text/identities into datasets: output includes supplied targets
and candidates by design (unlike the privacy-safe runtime diagnostics).

The generated dataset and every report carry an explicit `matchingPolicy` object.
Supplied datasets may omit it to use the fixed policy, but a conflicting declared
policy is rejected (exit 2), not silently accepted as a different accuracy metric.
Reports include `datasetSha256` for exact input provenance.

`top1/top3` are NFC **case-sensitive** counts; `lookupTop1/lookupTop3` additionally
use shared language lookup (case folded, diacritics retained). `targetInDictionary`
uses lookup identity. `wrongDictionaryOutputs` counts outputs absent from that
profile's dictionary by lookup identity. `total` includes every input case;
`evaluated`, `unavailable`, `invalid` partition it. Ratios, if derived, must state
the denominator; dictionary absence and empty candidate lists are not exclusions.
Native-reviewed held-out data, stock-layout journeys and device acceptance remain
separate originating-session acceptance work.

## Synthetic noisy swipes (`synthetic-noisy-v1`) - provisional, NOT human

Roadmap 0.5 asks for real swipes from speakers of each language; until volunteers exist,
`make-noisy-swipes.py` makes **model-generated** traces so Spanish, French and German get a
provisional number. They are not human swipes and are never `nativeReviewed` (the bench
rejects a `synthetic-noisy-v1` case that claims it). The bench replays their explicit
`points` exactly like supplied ones; `traceKeys` is not allowed with them.

How: `noisy-params-v1.json` holds a small noise model fitted on real English swipes
(FUTO dataset, `dev.jsonl` only): where the finger lands and lifts relative to the key
centre, how far it misses keys in between, speed (slow at sharp turns and at lift-off),
sample gaps and jitter. The same model is applied to the es/fr/de layouts, so it assumes
people swipe other languages like English. Words are sampled (seed 1, 400 each, with
repeats) by how often they occur in `~/.cache/swipe-keyboard-train/<lang>/test/{everyday,web}.txt`;
tokens whose spelling is not in the language dictionary are skipped (so out-of-dictionary
words, which a real user would also type, are NOT measured). Accented letters are traced
through their base key and apostrophes are not swiped (same rules as `make-seeds.py`).
Regenerate: `make-noisy-swipes.py gen --lang es --params noisy-params-v1.json --text-dir <dir> --out ...`.

Calibration (English words of `bench-2000.jsonl`, 1669 plain a-z words, English profile):

| traces | first guess | top 3 |
|---|---|---|
| real swipes | 92.8% | 96.4% |
| synthetic, calibrated (noise x0.45) | 92.3% | 96.0% |
| synthetic, raw fit (noise x1.0, first 594 words) | 82.2% | 90.9% |

The raw fit measured in key units is much harsher than the real swipes the recogniser
actually sees, so one multiplier (0.45 on all spatial noise) was tuned against bench-2000
(a scoring set; a single number, shape fitted on dev only). Calibration is English-only
and aggregate: it says nothing about whether the errors are the same words, nor about
real es/fr/de handwriting habits. Treat the language numbers as +/- several points and
as a ceiling-ish estimate; replace with real volunteer swipes when available.

First provisional results (installed packs, 400 words each, bench CPU about 2 ms/word):
es first guess 85.0% / top 3 98.2%; fr 82.8% / 93.2%; de 86.8% / 96.5%. Most misses are short
words and near-neighbour words; Spanish `que`/`qué`, `como`/`cómo` and French `ou`/`où` are
accent-only confusions the swipe path cannot resolve (needs context).
