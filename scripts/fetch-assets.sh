#!/usr/bin/env bash
# Build-time preparation only: checksum-pinned FUTO base models and English
# language files (dictionaries, n-gram models, word lists) and es/fr/de packs, including license notices. Not an end-user install.
# Optional evaluation/neural data requires SWIPEKB_FETCH_DEVELOPER_EXTRAS=1.
# Optional n-gram generation remains an explicit scripts/lm/build-lm.sh operation.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="${SWIPEKB_ASSETS:-$ROOT/assets}"
# Base payloads are immutable and verified, including cached files.
python3 "$ROOT/scripts/fetch-assets.py" --assets "$A"
echo "Powered by FUTO Swipe — base assets ready in $A"
# Evaluation and neural assets are intentionally excluded from the base package.
# Legacy developer fetches below require explicit opt-in and are never packaged.
[ "${SWIPEKB_FETCH_DEVELOPER_EXTRAS:-0}" = 1 ] || exit 0
DS=https://huggingface.co/datasets/futo-org/swipe.futo.org/resolve/main

get() { # url dest — download once; .part first so an interrupted download never looks complete
    [ -s "$2" ] && return 0
    mkdir -p "$(dirname "$2")"
    curl -fL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"
}

get "$DS/test.jsonl" "$A/eval/test.jsonl"
# WHY never fatal: the keyboard works without the neural model (the best guesses are then simply not
# re-ordered), so a failed download must not stop an install.
get https://huggingface.co/ggml-org/SmolLM2-135M-GGUF/resolve/main/SmolLM2-135M-Q8_0.gguf "$A/nlm/en_nlm.gguf" ||
    echo "warning: neural word model not downloaded; the keyboard works without it" >&2
echo "assets ready in $A"
