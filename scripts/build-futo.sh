#!/usr/bin/env bash
# build-futo.sh — build ExecuTorch and FUTO's swipe-library with FUTO's own Makefile (`make host`).
# WHY -fPIC: our engine is a shared library (.so) that swallows these static archives; without
# position-independent code the link fails. CMake picks CFLAGS/CXXFLAGS up on the first configure.
# First run downloads a PyTorch CPU wheel (~200 MB; only its code generator is used) and compiles
# ExecuTorch (~10-20 min). Usage: scripts/build-futo.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export CFLAGS="${CFLAGS:-} -fPIC" CXXFLAGS="${CXXFLAGS:-} -fPIC"
make -C "$ROOT/third_party/swipe-library" host
