# Real swipes for es / fr / de / en: the HeliBoard Gesture Data Project (TEST ONLY)

Source: https://codeberg.org/eclexic/heliboard-gesture-data (CC BY-SA 4.0), real gestures donated by HeliBoard users.

**Licence rule: evaluation only.** Never train on it. Never commit the gestures or any per-swipe file derived from
them (share-alike). Everything generated lives in `~/.cache/swipe-keyboard-train/heliboard/`; git holds only the
scripts and the summary numbers below. `heliboard_to_jsonl.py` refuses to write inside the checkout.

## Rebuild

```
git clone --depth 1 https://codeberg.org/eclexic/heliboard-gesture-data ~/.cache/swipe-keyboard-train/heliboard/repo
uv pip install --python ~/.cache/swipe-keyboard-train/venv/bin/python zstandard
~/.cache/swipe-keyboard-train/venv/bin/python -I scripts/swipe/heliboard_to_jsonl.py            # es fr de en -> .../heliboard/out
# today's keyboard (FUTO engine), one run per bench chunk (the bench refuses files over 16 MB), then add up:
for f in out/bench/*.json; do swipekb-language-bench --dataset $f --model-root <root> --packs-root <root>/language-packs > out/score/$(basename $f .json).out; done
~/.cache/swipe-keyboard-train/venv/bin/python -I scripts/swipe/heliboard_score_summary.py out/score
# our engine, per layout (each layout has its own key file in out/layouts/<layout id>.tsv):
swipekb-swipenet-bench --model m.bin --words words.tsv --layout out/layouts/<id>.tsv out/en/test.jsonl
```
(Use a `swipekb-language-bench` build whose profile format matches the installed packs; an older build answered "unavailable".)

## What is in the output
`<lang>/test.jsonl` (5000 swipes) and `dev.jsonl` (up to 20000), engine format with per-swipe `layout` id, x,y in 0..1 of
the letter area (bounding box of the letter keys; HeliBoard records every key's pixel box per gesture, which is used
exactly). `bench/<lang>-test-NN.json` are language-bench cases (`supplied-human`, explicit keys; qwerty/azerty/qwertz only).
Split: sha256(uuid), lowest hashes are test. It is not split by person (no contributor id exists).

Dropped: null target, more than one finger, under 2 letters, characters other than letters and apostrophes (hyphens,
digits), letters with no key even after accent-to-base and ß/æ/œ mapping, words that collapse to one key, under 3 points,
time going backwards. Apostrophes are kept in the target (not swiped), accents map to the base key.

## Counts and today's keyboard (FUTO engine), 2026-10-22
Case-insensitive, accents kept; denominator = evaluated cases. "In dictionary" = target exists in the pack, which caps the score.

| lang | read | dropped | kept | test | boards in test | first | top 3 | in dict |
|---|---|---|---|---|---|---|---|---|
| es | 30009 | 5 | 30004 | 5000 | qwerty ~100% | 77.7% | 85.1% | 89.1% |
| fr | 34096 | 1087 | 33009 | 5000 | azerty 89%, qwerty 9%, qwertz 1% | 64.6% | 76.9% | 84.9% |
| de | 50574 | 79 | 50495 | 5000 | qwertz 94%, qwerty 6% | 69.7% | 76.4% | 80.2% |
| en | 837202 | 3366 | 833836 | 5000 | qwerty (+ ~8% other boards, no bench) | 84.5% | 93.4% | 100% |

Case-sensitive first guess: es 76.0, fr 59.3, de 40.7 (German nouns), en 73.8. Invalid (bench refused geometry, e.g. a
Spanish board without ñ): es 210, de 7, fr 1. Our open engine (English model, 5000 en swipes, idealised qwerty): 72.0% / 81.4%;
on the biggest real board 81.0% / 90.6%.

## Caveats
Heavy selection bias (see the dataset README): users re-swipe failed words, mostly tech-savvy donors, a few people give
most data, word choice follows the dictionary's frequency, so targets are often rarer words than real typing. Apostrophe words
and many boards (20-90 distinct layouts per language) are included. `dev` and `test` may share contributors.
