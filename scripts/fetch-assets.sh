#!/usr/bin/env bash
# fetch-assets.sh — download what the engine needs into assets/ (git-ignored: large and third-party):
#   FUTO Swipe models (FUTO Model Weights License 1.0 — products must say "Powered by FUTO Swipe"),
#   the English dictionary (AOSP word list, Apache-2.0, plus wordfreq words, CC-BY-SA 4.0; built by
#   scripts/dict/build-en-swipe.sh), and FUTO's real-swipe test split (MIT) for the scoreboard.
# Re-running skips files already present. Usage: scripts/fetch-assets.sh
# The optional n-gram model (assets/lm/en_ngram.bin, FineWeb, ODC-BY 1.0) is a separate, much larger build:
# scripts/lm/build-lm.sh (install.sh runs it when the model is missing).
# The optional neural word model (assets/nlm/en_nlm.gguf) is SmolLM2-135M by Hugging Face (Apache-2.0), 8-bit
# GGUF from ggml-org, 145 MB; it re-orders the best few guesses (step 1b) and needs llama.cpp installed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="${SWIPEKB_ASSETS:-$ROOT/assets}"
HF=https://huggingface.co/futo-org/futo-swipe/resolve/main
DS=https://huggingface.co/datasets/futo-org/swipe.futo.org/resolve/main

get() { # url dest — download once; .part first so an interrupted download never looks complete
    [ -s "$2" ] && return 0
    mkdir -p "$(dirname "$2")"
    curl -fL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"
}

for f in honorable_sturgeon/model_fp32.pte honorable_sturgeon/metadata.json \
         magic_macaw/model_fp32.pte magic_macaw/metadata.json \
         hungry_jellyfish/context_lm.pte hungry_jellyfish/metadata.json hungry_jellyfish/vocab.txt; do
    get "$HF/$f" "$A/models/$f"
done
get "$HF/LICENSE.md" "$A/models/FUTO-MODEL-LICENSE.md"

# WHY a build step instead of a download: the shipped dictionary is AOSP's list plus wordfreq words, which
# measured better on the scoreboard (see scripts/dict/build-en-swipe.sh).
"$ROOT/scripts/dict/build-en-swipe.sh" "$A"

get "$DS/test.jsonl" "$A/eval/test.jsonl"
# WHY never fatal: the keyboard works without the neural model (the best guesses are then simply not
# re-ordered), so a failed download must not stop an install.
get https://huggingface.co/ggml-org/SmolLM2-135M-GGUF/resolve/main/SmolLM2-135M-Q8_0.gguf "$A/nlm/en_nlm.gguf" ||
    echo "warning: neural word model not downloaded; the keyboard works without it" >&2
echo "assets ready in $A"
