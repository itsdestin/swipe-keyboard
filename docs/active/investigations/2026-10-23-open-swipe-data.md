---
status: active
cards: 179
---
# Open swipe data for fr/de/es (and others), 2026-10-23

Bottom line: no other permissively licensed, human, non-English swipe corpus found. The only usable human non-English data is FUTO's swipe-5 (about 10k swipes). More needs our own collection, plus synthetic data.

## Ranked table

| Rank | Dataset | Verdict | Languages / size | Licence (exact) | Catch |
|---|---|---|---|---|---|
| 1 | [futo-org/swipe.futo.org](https://huggingface.co/datasets/futo-org/swipe.futo.org), swipe-5 | **Usable** | Counted from swipe5.jsonl (59,247 rows): de 5,207 (german 2,594, qwertz 1,410, qwerty 1,203); fr 3,124 (azerty 2,542, qwerty 510, qwertz 72); es 2,029 (spanish layout); pl 1,019; en ~47k; lt 52; toki pona 450 | Card front matter `license: mit` (verified via HF API). No separate LICENSE text read. | Unfiltered (use `distance` field); some are two-finger "dual_finger" swipes (`data` is `{"L","R"}`); formal Wikipedia text; volunteers skew techy. Has timestamps, canvas size, layout JSON (key geometry), word, sentence. |
| 2 | [futo-org/swipe-negatives](https://huggingface.co/datasets/futo-org/swipe-negatives) | Usable, but not swipes | English hard-negative word lists | `license: apache-2.0` | English only; no traces. |
| 3 | [How-We-Swipe (Leiva et al., MobileHCI 2021)](https://osf.io/sj67f/) | **Usable for training (corrected 2026-10-23)** | English only; 1,338 users, 11,318 words, 8.8M touch points; one layout (QWERTY, 360x205 canvas); has timestamps, x/y, radius | **MIT License** — the OSF node's licence record (api.osf.io/v2/licenses/563c1cf88c5e4a3877f9e972) is "MIT License" with copyright holders L. A. Leiva, S. Kim, W. Cui, X. Bi, A. Oulasvirta, 2021. The first pass read only the holders field and missed it. Credit required. | English only, so it helps English variety, not fr/de/es. Files: swipelogs.zip (70MB), metadata.tsv, stats-*.tsv. |
| 4 | [swipetest collector](https://github.com/luileito/swipetest) (PHP) | **Unusable as-is** | n/a; has gettext translation (`translate.sh es_ES`) | GitHub API reports no licence; README has none. | No licence means all rights reserved. We would need to ask the author (not done). Useful as a design reference only. |
| 5 | [HeliBoard Gesture Data](https://codeberg.org/eclexic/heliboard-gesture-data) | Test-only | Many languages | README: "licensed it under the CC BY-SA-4.0 license" | No permissive parts found; the sibling [word-list CSVs](https://codeberg.org/eclexic/heliboard-word-list-csvs/) were not checked (unverified). |
| 6 | [IndicSwipe](https://swipe.ai4bharat.org/docs/datasets/) | Unusable | 7 Indic languages, 193,658 + 104,412 words, **synthetic** | No licence stated on site; GitHub API shows none for iitmnlp/indic-swipe or emilbiju/indic-swipe | Wrong languages and unlicensed. Its method (below) is the useful part. |
| - | Zhai/Kristensson SHARK2, Alsharif et al. (Google), Typewise, FlorisBoard, Thumb-Key, AnySoftKeyboard | Not found | none released | none | Searches found no public data. Absence only means my searches (HF, GitHub, OSF, general web) turned up nothing. HF search for "gesture typing", "shape-writing" returned no relevant datasets. Kaggle/Zenodo/figshare were not directly searched. |

## Synthetic-generation knowledge
- **Minimum-jerk paths between key centres plus noise**: described by the IndicSwipe site (Gaussian noise on start/end points and on the path), citing Quinn and Zhai (2018). Papers can be freely read and a method re-implemented; a method is not copyrightable, but do not copy their unlicensed scripts.
- **Sigma-lognormal**: [andrew-healey/sigma-lognormal](https://github.com/andrew-healey/sigma-lognormal) is a handwriting extractor, not a swipe generator; licence not checked (unverified). Standard extraction tool (ScriptStudio) is not open source.
- **WordGesture-GAN** (Leiva et al., CHI 2023) generates word-gestures; I could not read the paper (403) so its code/licence is unverified. Worth checking.
- Our own best calibration source: FUTO English swipes (MIT) to fit speed profiles, overshoot and corner-cutting, then regenerate for fr/de/es word lists and layouts (AZERTY, QWERTZ).

## Alternatives
1. **Own volunteer collection with CC0 consent page.** Easiest start: reproduce the swipe.futo.org flow (random Wikipedia sentences from Common Voice `wiki.fr/de/es.txt`; those sentence files are Common Voice's CC0 data, from memory, verify). FUTO's collector code licence is **not found**: swipe.futo.org links nothing, and FUTO's other repos use FUTO Temporary/Source-First licences, which are non-commercial. Do not reuse; assume no code reuse.
2. **swipetest (Leiva)**: only reusable if the author grants a licence; asking is a contact action I did not take.
3. **Ask FUTO** whether more fr/de/es swipes are planned (they say they accept "contributions for other languages and layouts") and whether collector code can be released.
4. **Write a small collector ourselves** (MIT/Apache) that stores the same fields as FUTO's jsonl (canvas size, layout, per-point x/y/t) so data merges directly, with a CC0 dedication shown before submission.
5. **Mix**: real FUTO fr/de/es (~10k) for fine-tuning and validation, synthetic for pretraining.

## Unverified
Leiva data licence; swipe-5 full licence file; CC0 status of Common Voice sentence files; all licences of sigma-lognormal and WordGesture-GAN code; existence of data on Kaggle/Zenodo/figshare.
