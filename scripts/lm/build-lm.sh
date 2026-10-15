#!/usr/bin/env bash
# build-lm.sh — builds assets/lm/en_ngram.bin, the word-sequence (3-gram) model that can rank swipe guesses
# by "how likely is this word after the previous two" (engine/include/swipekb/ngram.hpp).
#
# Corpus: FineWeb, sample-10BT (huggingface.co/datasets/HuggingFaceFW/fineweb), licence ODC-BY 1.0 — the
# first LM_SHARDS (6) parquet files, ~2.15 GB each, ~3.4 billion words in total.
# WHY FineWeb and not Wikipedia: the scoreboard's sentences come from Wikipedia, so a Wikipedia-trained model
# would have seen the test sentences. Pages from wiki sites are skipped too (scripts/lm/tokenize.py).
# WHY one shard at a time: the drive is nearly full. Each shard is downloaded, turned into word numbers,
# DELETED, counted, and the word numbers DELETED, before the next one; at the end the counts are deleted too
# (KEEP_LM_COUNTS=1 keeps them, ~12 GB, to rebuild other sizes quickly).
#
# Needs: curl, g++ with OpenMP, uv (makes a private Python environment with pyarrow; nothing system-wide),
# the dictionary (assets/dict/en_swipe.combined, scripts/fetch-assets.sh), ~25 GB free disk while it runs,
# ~25 GB RAM for the last step. Takes ~15 minutes on a fast connection and 32 cores.
#
# Usage: scripts/lm/build-lm.sh [--force]      (skips if the model exists, unless --force)
# Env:   SWIPEKB_ASSETS (assets/), LM_SHARDS (6), LM_T2 (15), LM_T3 (30): keep word pairs seen >= LM_T2 times
#        and triples seen >= LM_T3 times. WHY 15/30: 153 MB; larger models (up to 400 MB) scored within
#        ~0.2 points on the validation swipes (step-1 report).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
A="${SWIPEKB_ASSETS:-$ROOT/assets}"
W="$A/lm-build"
OUT="$A/lm/en_ngram.bin"
SHARDS="${LM_SHARDS:-6}" T2="${LM_T2:-15}" T3="${LM_T3:-30}"
URL=https://huggingface.co/datasets/HuggingFaceFW/fineweb/resolve/main/sample/10BT

if [ -s "$OUT" ] && [ "${1:-}" != --force ]; then
    echo "n-gram model already built: $OUT"
    exit 0
fi
DICT="$A/dict/en_swipe.combined"
[ -s "$DICT" ] || { echo "build-lm.sh: missing $DICT (run scripts/fetch-assets.sh)" >&2; exit 1; }
mkdir -p "$W/raw" "$W/runs" "$W/bin" "$A/lm"
# WHY check first: running out of disk half-way would leave a full drive and no model.
free_gb=$(( $(df --output=avail -k "$W" | tail -1) / 1024 / 1024 ))
[ "$free_gb" -ge 25 ] || { echo "build-lm.sh: needs ~25 GB free, only $free_gb GB" >&2; exit 1; }
command -v uv >/dev/null || { echo "build-lm.sh: needs uv (https://docs.astral.sh/uv/)" >&2; exit 1; }

# Private Python environment for reading parquet files. WHY pinned: same parsing on every machine.
[ -x "$W/venv/bin/python" ] || uv venv -q "$W/venv"
VIRTUAL_ENV="$W/venv" uv pip install -q pyarrow==26.0.0
PY="$W/venv/bin/python"

python3 -I "$ROOT/scripts/lm/make-vocab.py" "$DICT" "$W/vocab.tsv"
g++ -O3 -std=c++20 -fopenmp -D_GLIBCXX_PARALLEL -I"$ROOT/engine/include" \
    "$ROOT/scripts/lm/ngram-build.cpp" "$ROOT/engine/src/ngram.cpp" -o "$W/bin/ngram-build"

runs=()
for ((i = 0; i < SHARDS; i++)); do
    s=$(printf %03d "$i")
    runs+=("$W/runs/$s.run")
    [ -s "$W/runs/$s.run" ] && continue   # counted on an earlier (interrupted) run
    echo "== shard $s: download"
    curl -fL --retry 3 -sS -o "$W/raw/$s.parquet.part" "$URL/${s}_00000.parquet"
    mv "$W/raw/$s.parquet.part" "$W/raw/$s.parquet"
    # WHY `python -I` and the script outside $W: downloaded data never gets to supply code.
    "$PY" -I "$ROOT/scripts/lm/tokenize.py" "$W/vocab.tsv" "$W/raw/$s.parquet" "$W/ids/$s"
    rm -f "$W/raw/$s.parquet"
    "$W/bin/ngram-build" count "$W/runs/$s.run.part" "$W/ids/$s"/*.u32
    mv "$W/runs/$s.run.part" "$W/runs/$s.run"
    rm -rf "$W/ids/$s"
done

"$W/bin/ngram-build" build --vocab "$W/vocab.tsv" --out "$OUT" --order 3 --t2 "$T2" --t3 "$T3" "${runs[@]}"
[ "${KEEP_LM_COUNTS:-0}" = 1 ] || rm -rf "$W/runs" "$W/ids" "$W/raw"
echo "n-gram model ready: $OUT ($(du -h "$OUT" | cut -f1))"
