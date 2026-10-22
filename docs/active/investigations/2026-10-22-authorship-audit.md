---
status: active
cards: 179
---
# Authorship audit: what in swipe-keyboard is FUTO-derived (2026-10-22)

Scope: 239 tracked files outside `third_party/`, compared against FUTO swipe-library (`/home/destin/swipe-keyboard/third_party/swipe-library`, submodule empty in this worktree). Method: grep of every file for FUTO / swipe_decoder / ExecuTorch / model names, then reading the hits against FUTO's `src/engine.cpp`, `beam_search.cpp`, `models/scoring.json`. Files with no hit are classed CLEAN by absence of evidence, not line-by-line reading. Not legal advice.

## Counts
CLEAN: 158, DATA/ASSET: 12, FUTO-DERIVED: 53, UNSURE: 16

## Licence facts
- `LICENSE` = GPL-3.0 full text. No `LICENSES/` folder. `packaging/licenses/` holds CC-BY-SA-4.0 text, wordfreq Apache-2.0 notice and README.
- Only SPDX headers in the repo: `scripts/patches/backendpredictionpolicy.h` (GPL-3.0-only) and a fixture string in `tests/integration/test_layout.py`. No source file has a copyright/licence header. REUSE blockers: no SPDX on ~all files, no `LICENSES/`, no `REUSE.toml`/`.reuse/dep5` for data; FUTO-ish `LicenseRef-FUTO-Model-Weights-1.0` in PKGBUILD/spec; `LicenseRef-Tatoeba-CC-BY-2.0-FR` (SPDX has CC-BY-2.0 only); large binaries (`.ngram`, `.png`) need `.license` sidecars or REUSE.toml; vocab/contributors data (CC-BY-SA-4.0, CC-BY-2.0-FR, ODC-BY, Apache-2.0) must each be declared; GPL-3.0-or-later vs KDE preference for LGPL/GPL-2.0-or-later may need relicensing agreement.

## What is genuinely FUTO-derived logic (must be REWRITTEN, not just deleted)
1. `engine/src/recognizer.cpp`: path-fit formula in `rerankCandidates` (l.53, same form as FUTO `engine.cpp:446`: ctc/L^gamma + beta*length + lambda*freq) and the raw-shape guard (l.68, l.147; same behaviour as FUTO `engine.cpp` ~l.775-790 "preserve CTC top-1"). Paraphrased from FUTO. Rewrite from own design or drop.
2. `engine/include/swipekb/recognizer.hpp`: `RerankWeights` default numbers equal FUTO `scoring.json` rows (its own comment l.88 says they are FUTO scoring.json numbers; defaults 0.1126/2.2138/0.0060 are identical to FUTO row "encoder:honorable_sturgeon decoder:magic_macaw contextlm:hungry_jellyfish", verified); beam 100 default. Re-tune on our own data.
3. Everything else in the list below is glue/deletable: it only exists to call, build, download or test FUTO components.
4. NOT FUTO-derived (verified): `ngram.cpp`, `ngram_predictor.cpp`, `nlm.cpp` (llama.cpp, MIT), `nlm_loader.cpp`, `context.cpp`, `spelling.cpp`, `normalize.cpp` (mentions FUTO's 0..1 box only in comments), `prediction.cpp` n-gram branch. NOTE: a swipe DECODER replacement is required; today the whole swipe path (`Recognizer::recognize`) runs on FUTO's ExecuTorch encoder + trie beam search, so removal means writing a new decoder, not editing files.

## FUTO-DERIVED files (53) and what to do
- `.github/workflows/packages.yml`: builds ExecuTorch/swipe-library in CI (l.303 and surrounding steps); ~20%
- `.gitmodules`: only entry is gitlab.futo.org/keyboard/swipe-library; delete file and the gitlink
- `CMakeLists.txt`: l.20-24 add_subdirectory(third_party/swipe-library), executorch check, FATAL_ERROR; ~one block
- `bench/score.sh`: default runner = FUTO infer_jsonl on FUTO test split; ~40%
- `bench/tune-nlm.py`: FUTO_ALPHA=0.6387 and FUTO scoring.json rows (l.25-28); dumps from FUTO data; ~20%
- `bench/tune-rerank.py`: tunes around FUTO weights, futo_lm column (l.25,56-58); ~30%
- `engine/CMakeLists.txt`: l.7 links swipe_decoder + executorch; delete those two names
- `engine/include/swipekb/recognizer.hpp`: API shaped by FUTO: ModelPaths encoder/decoder/lm_model/lm_vocab, RawCandidate.futo_lm (l.83), RerankWeights defaults = FUTO scoring.json gamma/beta/lambda (l.87-90), beam_width default 100 = FUTO default (l.136), 0..1 letter-area coords (l.31). Header exposes no FUTO types; ~15% of file affected; rename/redefine fields and re-tune constants
- `engine/src/prediction.cpp`: ONE include (l.2 swipe_decoder/context_lm.hpp) + ContextLMScorer member + else-branch (l.30-32) running FUTO ExecuTorch ContextLM. ~25% of file; delete branch, keep n-gram path
- `engine/src/recognizer.cpp`: WHOLE-file glue (~60%): includes swipe_decoder engine/layout/trie (l.17-19); Impl holds SwipeEngine/Trie/KeyboardLayout; init() (~l.220-285) calls FUTO parse_combined_vocab, apply_parsed_to_trie, set_case_mapping, default_en_qwerty, SwipeEngine::Config/init; updateGeometry/sameGeometry/toRaw/recognize*/recognizeDetailed call engine.recognize. rerankCandidates l.53 path-fit formula = FUTO engine.cpp:446 (ctc/L^gamma + beta*len + lambda*freq); raw-shape guard l.68 and l.147 = FUTO engine.cpp ~l.775-790 "preserve CTC top-1". REWRITE: replace decoder; formula+guard are paraphrase, rewrite from own design. nlmOrder, poolSize, readSpellings, complete() are ours
- `engine/tests/test_normalize.cpp`: l.8-22 asserts our key centres equal FUTO en_qwerty numbers; test data constants from FUTO layout
- `engine/tests/test_prediction.cpp`: l.81,106-107 load FUTO hungry_jellyfish context_lm.pte + vocab for the neural predictor branch
- `engine/tests/test_recognizer.cpp`: needs FUTO encoder fixture (l.49,84,91,103-107); tests Recognizer->FUTO engine
- `install.sh`: l.52 checks libexecutorch.a and runs build-futo.sh; ~one step
- `integration/setup.py`: l.27 lists FUTO models + FUTO-MODEL-LICENSE; l.463 "Powered by FUTO Swipe"; ~10%
- `integration/swipe-keyboard.desktop`: Name=Swipe Keyboard (Powered by FUTO Swipe)
- `packaging/NOTICE`: FUTO attribution + FUTO Model Weights License text; rewrite
- `packaging/arch/PKGBUILD`: LicenseRef-FUTO-Model-Weights-1.0, runs build-futo.sh; has SPDX-style licence field
- `packaging/assets.lock.json`: 7 huggingface.co/futo-org/futo-swipe downloads (FUTO-MODEL-LICENSE.md, honorable_sturgeon, magic_macaw, hungry_jellyfish models/metadata/vocab); remaining AOSP/wordfreq/Tatoeba/FineWeb entries are ours. ~45%
- `packaging/fedora/swipe-keyboard.spec`: License field lists FUTO weights; runs build-futo.sh
- `packaging/flatpak/build-swipe.sh`: l.8-9 patches ExecuTorch/swipe-library and provisions futo deps
- `plugin/languageprofile.cpp`: language-pack verifier hard-wires FUTO: models/FUTO-MODEL-LICENSE.md hash check (l.75,137), compatibility "swipekb-futo-v1", futoRevision and executorchRevision pins (l.118-120), encoder/decoder/context asset roles. ~25% of file; rework pack format for FUTO-free (ngram+dict only)
- `plugin/swipeinputmethod.cpp`: l.104-110 fills ModelPaths encoder/decoder/contextModel/contextVocab, SWIPEKB_NO_DECODER env; ~3 spots in a 58 KB file; rest is ours (Qt plugin, QML capture)
- `plugin/tests/languagefixtures.h`: builds fake packs incl. FUTO-LICENSE, futoRevision, executorchRevision, swipekb-futo-v1 (l.28-44)
- `plugin/tests/tst_languagepacks.cpp`: asserts FUTO model files/license paths (l.28)
- `plugin/tests/tst_languageprofile.cpp`: FUTO-LICENSE / encoderLicense hash tests (l.119 etc.)
- `plugin/tests/tst_nextwordworker.cpp`: l.52-60 links FUTO hungry_jellyfish context_lm.pte/vocab/metadata as fixture
- `requirements/futo.txt`: pip hashes for FUTO/ExecuTorch build venv
- `requirements/kde-6.10/futo.txt`: same
- `requirements/native-aarch64/futo.txt`: same
- `scripts/apply-build-patches.py`: whole file: patches ExecuTorch flatbuffers/xnnpack for the FUTO build; delete
- `scripts/build-futo.sh`: whole file: builds ExecuTorch + FUTO swipe-library via its Makefile; delete
- `scripts/dict/build-candidates.sh`: downloads FUTO en_w2.combined (l.8-21; note says FUTO published it with no licence statement); experiment script, delete or drop candidate b
- `scripts/dict/language-pack.schema.json`: consts compatibility swipekb-futo-v1, futoRevision, executorchRevision; encoder roles
- `scripts/dict/language_pack.py`: packs FUTO encoder.pte/metadata/FUTO-MODEL-LICENSE into packs, pins futoRevision (l.108-149); ~40%
- `scripts/fetch-assets.sh`: downloads FUTO models + FUTO swipe dataset from huggingface futo-org (l.12-16); ~half (AOSP/wordfreq dict part is ours)
- `scripts/package-check.py`: l.101 expects FUTO model files + FUTO-MODEL-LICENSE in package; ~10%
- `scripts/package-source.sh`: l.22-28 bundles third_party/swipe-library + executorch in source archive
- `scripts/patches/swipe-metadata.patch`: patch against FUTO swipe-library (model_metadata.hpp include) and ExecuTorch json; whole file
- `scripts/provision-build-deps.py`: "futo" group = pip deps (torchgen etc.) for FUTO/ExecuTorch build; "dictionary" group may stay (~half)
- `tests/bench/score.bats`: tests bench/score.sh --runner futo
- `tests/features/CMakeLists.txt`: l.42-43 SWIPEKB_READONLY_UPSTREAM links FUTO symbols; first part (FUTO-free sources) is clean; ~30%
- `tests/packaging/test_build_patches.py`: tests ExecuTorch patch script; whole file
- `tests/packaging/test_metadata_contract.py`: tests FUTO encoder/decoder/contextlm metadata.json contract; whole file
- `tests/packaging/test_packaging.py`: l.131 expects FUTO .pte models in package; ~10%
- `tests/test_language_packs.py`: l.91 FUTO-MODEL-LICENSE round-trip, encoder root
- `tools/CMakeLists.txt`: l.35-47 SWIPEKB_FUTO_LAYOUT path to third_party/swipe-library/models/layouts/en_qwerty.json; bench/replay/nlm-score targets link FUTO-backed engine
- `tools/benchlayout.h`: reads FUTO layouts/en_qwerty.json (keys cx,cy,rx,ry, 0..1) l.1,27; whole file (~50 lines); deletable with bench
- `tools/swipekb-bench.cpp`: replays FUTO real-swipe test file (FUTO dataset, MIT per brief) through Recognizer; default layout is FUTO json; --no-ngram = FUTO context model; futo_lm column. ~30% affected (dataset/layout loaders, FUTO-mode flags)
- `tools/swipekb-language-bench.cpp`: l.127-128 fills FUTO encoder/decoder/context ModelPaths; 2 spots
- `tools/swipekb-replay.cpp`: --no-ngram/--no-decoder/--no-lm switch FUTO models (l.59,148,270-273 "futo" ranking labels); ~15%
- `tools/tests/tst_benchlayout.cpp`: tests against FUTO en_qwerty.json key centres; whole file
- `tools/tests/tst_replay.cpp`: l.90 requires honorable_sturgeon model; l.148 decoder/lm flags; tests FUTO-path only

## Table

| path | class | evidence |
|---|---|---|
| .gitattributes | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| .github/ISSUE_TEMPLATE/bug.yml | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| .github/ISSUE_TEMPLATE/idea.yml | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| .github/actionlint.yaml | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| .github/workflows/packages.yml | FUTO-DERIVED | builds ExecuTorch/swipe-library in CI (l.303 and surrounding steps); ~20% |
| .gitignore | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| .gitmodules | FUTO-DERIVED | only entry is gitlab.futo.org/keyboard/swipe-library; delete file and the gitlink |
| CHANGELOG.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| CMakeLists.txt | FUTO-DERIVED | l.20-24 add_subdirectory(third_party/swipe-library), executorch check, FATAL_ERROR; ~one block |
| CONTRIBUTING.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| LICENSE | DATA/ASSET | GPL-3.0 full text (FSF); no LICENSES/ folder |
| README.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| bench/history.tsv | DATA/ASSET | our benchmark log; rows reference FUTO baseline and en_w2; facts not code |
| bench/languages/README.md | CLEAN | mentions FUTO recognizers (l.18) |
| bench/languages/make-seeds.py | CLEAN | own |
| bench/languages/synthetic-centered-v1.json | DATA/ASSET | synthetic swipes generated by us (make-seeds.py); no external source |
| bench/make-slice.py | UNSURE | slices FUTO test split? check; 602 bytes |
| bench/nextword-es.tsv | DATA/ASSET | test set; source not stated in file - check bench/languages/README / make-nextword-testsets.py (Tatoeba?) |
| bench/score.sh | FUTO-DERIVED | default runner = FUTO infer_jsonl on FUTO test split; ~40% |
| bench/tune-nlm.py | FUTO-DERIVED | FUTO_ALPHA=0.6387 and FUTO scoring.json rows (l.25-28); dumps from FUTO data; ~20% |
| bench/tune-rerank.py | FUTO-DERIVED | tunes around FUTO weights, futo_lm column (l.25,56-58); ~30% |
| cmake/Install.cmake | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/active/plans/2026-10-20-multilingual-prediction.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/active/plans/2026-10-22-open-engine-roadmap.md | CLEAN | our plan |
| docs/cleanroom/gboard-report.md | UNSURE | Gboard clean-room study (not FUTO); mentions FUTO; licence/provenance of study text - ours; keep out of KDE tree |
| docs/compatibility-investigation.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/images/swipe-typing.png | DATA/ASSET | own screenshot, synthetic text |
| docs/install-details.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| docs/practice/words-1.txt | CLEAN | own practice words |
| docs/pre-reddit-verification.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/publication-audit.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| docs/release-acceptance.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/releases/v0.1.0-r3.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| docs/releases/v0.1.0.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| docs/releases/v0.2.0.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| docs/releases/v0.3.1-1.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| docs/releasing.md | CLEAN | our prose; mentions FUTO (attribution / licence statements) - must be edited when FUTO removed, not derived |
| docs/revision3-verification.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/standalone-verification.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/superpowers/plans/2026-10-14-m0-swipe-on-z13.md | CLEAN | our plan; mentions FUTO |
| docs/superpowers/plans/2026-10-16-standalone-packaging.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/superpowers/plans/2026-10-18-compatibility-first.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/superpowers/plans/2026-10-18-pre-reddit-hardening.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| docs/superpowers/specs/2026-10-14-context-search-design.md | UNSURE | same; mentions FUTO |
| docs/superpowers/specs/2026-10-14-swipe-typing-design.md | UNSURE | design doc built around FUTO engine; roadmap says it needs a "copied constants" review |
| docs/v1-handoff.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| engine/CMakeLists.txt | FUTO-DERIVED | l.7 links swipe_decoder + executorch; delete those two names |
| engine/include/swipekb/ngram.hpp | CLEAN | own modified-KN n-gram model; comments only (l.3,9,45) |
| engine/include/swipekb/ngram_predictor.hpp | CLEAN | own; comment says it must build without FUTO |
| engine/include/swipekb/nlm.hpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| engine/include/swipekb/prediction.hpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| engine/include/swipekb/recognizer.hpp | FUTO-DERIVED | API shaped by FUTO: ModelPaths encoder/decoder/lm_model/lm_vocab, RawCandidate.futo_lm (l.83), RerankWeights defaults = FUTO scoring.json gamma/beta/lambda (l.87-90), beam_width default 100 = FUTO default (l.136), 0..1 letter-area coords (l.31). Header exposes no FUTO types; ~15% of file affected; rename/redefine fields and re-tune constants |
| engine/src/context.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| engine/src/ngram.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| engine/src/ngram_predictor.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| engine/src/nlm.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| engine/src/nlm_loader.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| engine/src/normalize.cpp | CLEAN | only comments mention FUTO coordinate convention (0..1 letter-area box, l.21/36); the box-normalisation is a natural geometry choice but follows FUTO model input contract - REVIEW (low risk) |
| engine/src/prediction.cpp | FUTO-DERIVED | ONE include (l.2 swipe_decoder/context_lm.hpp) + ContextLMScorer member + else-branch (l.30-32) running FUTO ExecuTorch ContextLM. ~25% of file; delete branch, keep n-gram path |
| engine/src/recognizer.cpp | FUTO-DERIVED | WHOLE-file glue (~60%): includes swipe_decoder engine/layout/trie (l.17-19); Impl holds SwipeEngine/Trie/KeyboardLayout; init() (~l.220-285) calls FUTO parse_combined_vocab, apply_parsed_to_trie, set_case_mapping, default_en_qwerty, SwipeEngine::Config/init; updateGeometry/sameGeometry/toRaw/recognize*/recognizeDetailed call engine.recognize. rerankCandidates l.53 path-fit formula = FUTO engine.cpp:446 (ctc/L^gamma + beta*len + lambda*freq); raw-shape guard l.68 and l.147 = FUTO engine.cpp ~l.775-790 "preserve CTC top-1". REWRITE: replace decoder; formula+guard are paraphrase, rewrite from own design. nlmOrder, poolSize, readSpellings, complete() are ours |
| engine/src/spelling.cpp | CLEAN | own apostrophe/hyphen restoration; FUTO only named in comments (letters-only form is FUTO trie behaviour; the idea of the fix is ours) |
| engine/tests/test_context.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| engine/tests/test_ngram.cpp | UNSURE | Rerank tests (l.189 KeepsTopKAndFutosOrderOnTies) use FUTO weights via rerankCandidates; n-gram tests are ours. Edit ~1 test |
| engine/tests/test_nlm.cpp | UNSURE | l.116 haveAssets() gates on FUTO encoder path only; llama NLM tests are ours |
| engine/tests/test_normalize.cpp | FUTO-DERIVED | l.8-22 asserts our key centres equal FUTO en_qwerty numbers; test data constants from FUTO layout |
| engine/tests/test_prediction.cpp | FUTO-DERIVED | l.81,106-107 load FUTO hungry_jellyfish context_lm.pte + vocab for the neural predictor branch |
| engine/tests/test_recognizer.cpp | FUTO-DERIVED | needs FUTO encoder fixture (l.49,84,91,103-107); tests Recognizer->FUTO engine |
| engine/tests/test_spelling.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| install.sh | FUTO-DERIVED | l.52 checks libexecutorch.a and runs build-futo.sh; ~one step |
| integration/CMakeLists.txt | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| integration/config.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| integration/layout.py | CLEAN | own; has SPDX in test fixture only |
| integration/profilecheck.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| integration/setup.py | FUTO-DERIVED | l.27 lists FUTO models + FUTO-MODEL-LICENSE; l.463 "Powered by FUTO Swipe"; ~10% |
| integration/swipe-keyboard | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| integration/swipe-keyboard.desktop | FUTO-DERIVED | Name=Swipe Keyboard (Powered by FUTO Swipe) |
| packaging/NOTICE | FUTO-DERIVED | FUTO attribution + FUTO Model Weights License text; rewrite |
| packaging/arch/PKGBUILD | FUTO-DERIVED | LicenseRef-FUTO-Model-Weights-1.0, runs build-futo.sh; has SPDX-style licence field |
| packaging/assets.lock.json | FUTO-DERIVED | 7 huggingface.co/futo-org/futo-swipe downloads (FUTO-MODEL-LICENSE.md, honorable_sturgeon, magic_macaw, hungry_jellyfish models/metadata/vocab); remaining AOSP/wordfreq/Tatoeba/FineWeb entries are ours. ~45% |
| packaging/fedora/swipe-keyboard.spec | FUTO-DERIVED | License field lists FUTO weights; runs build-futo.sh |
| packaging/flatpak/README.md | UNSURE | l.35 mentions rebuilding ExecuTorch; docs |
| packaging/flatpak/build-swipe.sh | FUTO-DERIVED | l.8-9 patches ExecuTorch/swipe-library and provisions futo deps |
| packaging/flatpak/build.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/flatpak/launch.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/flatpak/launcher.sh | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/flatpak/org.itsdestin.SwipeKeyboard.desktop | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/flatpak/org.itsdestin.SwipeKeyboard.json | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/flatpak/runtime-lock.json | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/flatpak/setup-guide.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/language-models/README.md | CLEAN | ours; says no FUTO weights used (l.11) |
| packaging/language-models/TATOEBA-CC-BY-2.0-FR.txt | DATA/ASSET | Tatoeba licence notice |
| packaging/language-models/de-CONTRIBUTORS.txt | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/language-models/de.ngram | DATA/ASSET | our n-gram trained on Tatoeba (CC-BY 2.0 FR); contributors file ships |
| packaging/language-models/de.vocab.tsv | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/language-models/es-CONTRIBUTORS.txt | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/language-models/es.ngram | DATA/ASSET | same, Spanish Tatoeba |
| packaging/language-models/es.vocab.tsv | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/language-models/fr-CONTRIBUTORS.txt | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/language-models/fr.ngram | DATA/ASSET | same, French Tatoeba |
| packaging/language-models/fr.vocab.tsv | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| packaging/licenses/CC-BY-SA-4.0.txt | DATA/ASSET | licence text |
| packaging/licenses/LICENSE.txt | DATA/ASSET | wordfreq Apache-2.0 notice (Copyright 2022 Robyn Speer) |
| packaging/licenses/wordfreq-README.md | DATA/ASSET | wordfreq README, CC-BY-SA data notice |
| packaging/release-targets.json | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/CMakeLists.txt | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/SwipeCapture.qml | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/language-assets.qrc | CLEAN | resource list |
| plugin/languageprofile.cpp | FUTO-DERIVED | language-pack verifier hard-wires FUTO: models/FUTO-MODEL-LICENSE.md hash check (l.75,137), compatibility "swipekb-futo-v1", futoRevision and executorchRevision pins (l.118-120), encoder/decoder/context asset roles. ~25% of file; rework pack format for FUTO-free (ngram+dict only) |
| plugin/languageprofile.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/languagetextrules.cpp | CLEAN | comment only (l.38) |
| plugin/languagetextrules.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/livehintsquery.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/nextwordsession.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/nextwordsession.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/nextwordworker.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/nextwordworker.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/ownedhistory.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/predictiondiagnostics.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/predictiondiagnostics.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/predictionpolicy.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/predictionpolicy.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/preflight.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/profilediagnostics.cpp | UNSURE | diagnostic strings name encoder/decoder/context roles and FUTO pins (l.15-20); wording only, tied to languageprofile role model. Small edit |
| plugin/profilediagnostics.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/swipeinputmethod.cpp | FUTO-DERIVED | l.104-110 fills ModelPaths encoder/decoder/contextModel/contextVocab, SWIPEKB_NO_DECODER env; ~3 spots in a 58 KB file; rest is ours (Qt plugin, QML capture) |
| plugin/swipeinputmethod.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/swipeoutcome.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/swipeoutcome.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/swiperecorder.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/swiperecorder.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/swipesession.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/swipesession.h | CLEAN | comment only (l.36); constant 20-word context chosen because FUTO tested whole sentence - reconsider rationale |
| plugin/tests/languagefixtures.h | FUTO-DERIVED | builds fake packs incl. FUTO-LICENSE, futoRevision, executorchRevision, swipekb-futo-v1 (l.28-44) |
| plugin/tests/tst_backendpredictionpolicy.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_languagecapture.cpp | CLEAN | comment uses word "decoder-compatible"; no FUTO use |
| plugin/tests/tst_languagegeometry.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_languageintegration.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_languagepacks.cpp | FUTO-DERIVED | asserts FUTO model files/license paths (l.28) |
| plugin/tests/tst_languageprofile.cpp | FUTO-DERIVED | FUTO-LICENSE / encoderLicense hash tests (l.119 etc.) |
| plugin/tests/tst_livehintsquery.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_nextwordintegration.cpp | CLEAN | only asserts no "decoder" asset in profile (l.453) |
| plugin/tests/tst_nextwordsession.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_nextwordworker.cpp | FUTO-DERIVED | l.52-60 links FUTO hungry_jellyfish context_lm.pte/vocab/metadata as fixture |
| plugin/tests/tst_ownedhistory.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_pluginload.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_predictioncapture.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_predictiondiagnostics.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_predictionpolicy.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_profilediagnostics.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_stocklayout.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_swipeoutcome.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_swiperecorder.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_swipesession.cpp | CLEAN | comment only (l.42) |
| plugin/tests/tst_typedwordsession.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/tests/tst_windowblur.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/typedwordsession.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/windowblur.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| plugin/windowblur.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| requirements/README.md | UNSURE | describes futo.txt vs dictionary.txt |
| requirements/dictionary.txt | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| requirements/futo.txt | FUTO-DERIVED | pip hashes for FUTO/ExecuTorch build venv |
| requirements/kde-6.10/futo.txt | FUTO-DERIVED | same |
| requirements/kde-6.10/wheels.lock.json | UNSURE | wheel lock; has FUTO/ExecuTorch (torchgen) group, dictionary group may stay |
| requirements/native-aarch64/dictionary.txt | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| requirements/native-aarch64/futo.txt | FUTO-DERIVED | same |
| requirements/native-aarch64/wheels.lock.json | UNSURE | wheel lock; has FUTO/ExecuTorch (torchgen) group, dictionary group may stay |
| requirements/wheels.lock.json | UNSURE | wheel lock; has FUTO/ExecuTorch (torchgen) group, dictionary group may stay |
| scripts/apply-build-patches.py | FUTO-DERIVED | whole file: patches ExecuTorch flatbuffers/xnnpack for the FUTO build; delete |
| scripts/arch-dependencies.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| scripts/build-futo.sh | FUTO-DERIVED | whole file: builds ExecuTorch + FUTO swipe-library via its Makefile; delete |
| scripts/check-stock-layout.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| scripts/ci-publish.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| scripts/ci-source.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| scripts/dict/build-candidates.sh | FUTO-DERIVED | downloads FUTO en_w2.combined (l.8-21; note says FUTO published it with no licence statement); experiment script, delete or drop candidate b |
| scripts/dict/build-en-swipe.sh | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| scripts/dict/combined.py | CLEAN | AOSP .combined reader/writer; docstring mentions FUTO only |
| scripts/dict/language-pack.schema.json | FUTO-DERIVED | consts compatibility swipekb-futo-v1, futoRevision, executorchRevision; encoder roles |
| scripts/dict/language_pack.py | FUTO-DERIVED | packs FUTO encoder.pte/metadata/FUTO-MODEL-LICENSE into packs, pins futoRevision (l.108-149); ~40% |
| scripts/dict/wordfreq_combined.py | CLEAN | own; combines AOSP (Apache-2.0) + wordfreq (CC-BY-SA) |
| scripts/fetch-assets.py | UNSURE | drives language-pack build with --encoder-root (FUTO encoder); checksum-lock fetcher is generic |
| scripts/fetch-assets.sh | FUTO-DERIVED | downloads FUTO models + FUTO swipe dataset from huggingface futo-org (l.12-16); ~half (AOSP/wordfreq dict part is ours) |
| scripts/lm/build-fineweb2-model.py | CLEAN | own trainer; FineWeb-2 (ODC-BY) input |
| scripts/lm/build-lm.sh | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| scripts/lm/build-tatoeba-model.py | CLEAN | own trainer; Tatoeba (CC-BY 2.0 FR) input |
| scripts/lm/make-nextword-testsets.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| scripts/lm/make-vocab.py | CLEAN | own; letters-only form mirrors FUTO trie behaviour (comments l.2-13) |
| scripts/lm/ngram-build.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| scripts/lm/tokenize.py | CLEAN | own; comment only |
| scripts/package-check.py | FUTO-DERIVED | l.101 expects FUTO model files + FUTO-MODEL-LICENSE in package; ~10% |
| scripts/package-source.sh | FUTO-DERIVED | l.22-28 bundles third_party/swipe-library + executorch in source archive |
| scripts/patches/backendpredictionpolicy.h | CLEAN | own Plasma bridge; has SPDX GPL-3.0-only |
| scripts/patches/plasma-prediction-policy.patch | CLEAN | patch to KDE Plasma (not FUTO); licence of patched hunks = KDE upstream |
| scripts/patches/swipe-metadata.patch | FUTO-DERIVED | patch against FUTO swipe-library (model_metadata.hpp include) and ExecuTorch json; whole file |
| scripts/provision-build-deps.py | FUTO-DERIVED | "futo" group = pip deps (torchgen etc.) for FUTO/ExecuTorch build; "dictionary" group may stay (~half) |
| scripts/release.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/bench/score.bats | FUTO-DERIVED | tests bench/score.sh --runner futo |
| tests/features/CMakeLists.txt | FUTO-DERIVED | l.42-43 SWIPEKB_READONLY_UPSTREAM links FUTO symbols; first part (FUTO-free sources) is clean; ~30% |
| tests/integration/test_config.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/integration/test_consent.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/integration/test_layout.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/integration/test_live_switch.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/integration/test_setup.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/integration/test_visibility_policy.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/integration/watcher/CMakeLists.txt | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/integration/watcher/main.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/packaging/test_asset_fetch.py | UNSURE | generic fetch tests; may reference FUTO asset names |
| tests/packaging/test_build_dependencies.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/packaging/test_build_patches.py | FUTO-DERIVED | tests ExecuTorch patch script; whole file |
| tests/packaging/test_elf_compatibility.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/packaging/test_flatpak.py | UNSURE | mentions FUTO/ExecuTorch (grep hit); flatpak build test |
| tests/packaging/test_flatpak_setup.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/packaging/test_metadata_contract.py | FUTO-DERIVED | tests FUTO encoder/decoder/contextlm metadata.json contract; whole file |
| tests/packaging/test_packaging.py | FUTO-DERIVED | l.131 expects FUTO .pte models in package; ~10% |
| tests/packaging/test_prediction_bridge.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/packaging/test_publish.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/packaging/test_release.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/packaging/test_release_workflow.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/session/CMakeLists.txt | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/session/README.md | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/session/session-client.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/session/test_vm_touch.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/session/vm-touch.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tests/test_language_packs.py | FUTO-DERIVED | l.91 FUTO-MODEL-LICENSE round-trip, encoder root |
| tools/CMakeLists.txt | FUTO-DERIVED | l.35-47 SWIPEKB_FUTO_LAYOUT path to third_party/swipe-library/models/layouts/en_qwerty.json; bench/replay/nlm-score targets link FUTO-backed engine |
| tools/benchlayout.h | FUTO-DERIVED | reads FUTO layouts/en_qwerty.json (keys cx,cy,rx,ry, 0..1) l.1,27; whole file (~50 lines); deletable with bench |
| tools/languageevaluation.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tools/languageevaluation.h | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tools/swipe-stats | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tools/swipekb-bench.cpp | FUTO-DERIVED | replays FUTO real-swipe test file (FUTO dataset, MIT per brief) through Recognizer; default layout is FUTO json; --no-ngram = FUTO context model; futo_lm column. ~30% affected (dataset/layout loaders, FUTO-mode flags) |
| tools/swipekb-language-bench.cpp | FUTO-DERIVED | l.127-128 fills FUTO encoder/decoder/context ModelPaths; 2 spots |
| tools/swipekb-nextword-eval.cpp | CLEAN | next-word eval on our n-gram predictor, FUTO-free per ngram_predictor.hpp |
| tools/swipekb-nlm-score.cpp | UNSURE | scores on FUTO test sentences (data dependency only, l.2); logic uses our llama.cpp scorer; likely clean once test set is swapped |
| tools/swipekb-replay.cpp | FUTO-DERIVED | --no-ngram/--no-decoder/--no-lm switch FUTO models (l.59,148,270-273 "futo" ranking labels); ~15% |
| tools/tests/test_language_cli.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tools/tests/test_swipe_stats.py | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tools/tests/tst_benchlayout.cpp | FUTO-DERIVED | tests against FUTO en_qwerty.json key centres; whole file |
| tools/tests/tst_languageevaluation.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tools/tests/tst_nextwordeval.cpp | CLEAN | no FUTO / swipe_decoder / ExecuTorch reference found by grep; ours |
| tools/tests/tst_replay.cpp | FUTO-DERIVED | l.90 requires honorable_sturgeon model; l.148 decoder/lm flags; tests FUTO-path only |
