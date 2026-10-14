#!/usr/bin/env bash
# fetch-assets.sh — download what the engine needs into assets/ (git-ignored: large and third-party):
#   FUTO Swipe models (FUTO Model Weights License 1.0 — products must say "Powered by FUTO Swipe"),
#   the AOSP English word list (Apache-2.0), and FUTO's real-swipe test split (MIT) for the scoreboard.
# Re-running skips files already present. Usage: scripts/fetch-assets.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="${SWIPEKB_ASSETS:-$ROOT/assets}"
HF=https://huggingface.co/futo-org/futo-swipe/resolve/main
DS=https://huggingface.co/datasets/futo-org/swipe.futo.org/resolve/main
AOSP=https://android.googlesource.com/platform/packages/inputmethods/LatinIME/+/refs/heads/main/dictionaries/en_US_wordlist.combined.gz

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

# WHY base64: googlesource serves raw files only as base64 text (?format=TEXT).
if [ ! -s "$A/dict/en_US_wordlist.combined" ]; then
    mkdir -p "$A/dict"
    curl -fsL "$AOSP?format=TEXT" | base64 -d | gunzip > "$A/dict/en_US_wordlist.combined.part"
    mv "$A/dict/en_US_wordlist.combined.part" "$A/dict/en_US_wordlist.combined"
fi

get "$DS/test.jsonl" "$A/eval/test.jsonl"
echo "assets ready in $A"
