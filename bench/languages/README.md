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
