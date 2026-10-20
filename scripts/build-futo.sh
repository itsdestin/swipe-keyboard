#!/usr/bin/env bash
# build-futo.sh — build ExecuTorch and FUTO's swipe-library with FUTO's own Makefile (`make host`).
# WHY -fPIC: our engine is a shared library (.so) that swallows these static archives; without
# position-independent code the link fails. CMake picks CFLAGS/CXXFLAGS up on the first configure.
# First run downloads a PyTorch CPU wheel (~200 MB; only its code generator is used) and compiles
# ExecuTorch (~10-20 min). Usage: scripts/build-futo.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export CFLAGS="${CFLAGS:-} -fPIC" CXXFLAGS="${CXXFLAGS:-} -fPIC"
python3 "$ROOT/scripts/apply-build-patches.py" "$ROOT/third_party/swipe-library/third_party/executorch" \
    --swipe-library "$ROOT/third_party/swipe-library"
# Populate the Makefile's venv target ourselves so its mutable pip setup is skipped.
python3 "$ROOT/scripts/provision-build-deps.py" futo "$ROOT/third_party/swipe-library/.venv"
# Configure first: upstream's make host configure has no portable-default switch.
# The cache value survives its subsequent cmake invocation.
cmake -S "$ROOT/third_party/swipe-library" -B "$ROOT/third_party/swipe-library/build" \
    -DCMAKE_BUILD_TYPE=Release -DUSE_MARCH_NATIVE=OFF
make -C "$ROOT/third_party/swipe-library" host
