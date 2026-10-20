#!/usr/bin/env bash
# Local, unpublished source snapshot of selected Git-tracked files, including recursive submodules.
# New release files must be explicitly staged first; arbitrary untracked/private files never enter it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
if git -C "$ROOT" submodule status --recursive | grep -q '^[-+U]'; then
    echo 'Initialize the pinned submodules with git submodule update --init --recursive first' >&2
    exit 1
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
DEST="$TMP/swipe-keyboard-0.2.0"
mkdir "$DEST"
# WHY a tracked-file allowlist: a directory blacklist can leak personal untracked files or
# previously built packages. Read current contents only for explicitly selected Git source paths.
git -C "$ROOT" ls-files --cached --recurse-submodules -z | \
    python3 -c 'import sys; sys.stdout.buffer.write(b"".join(b"./" + p + b"\0" for p in sys.stdin.buffer.read().split(b"\0") if p))' > "$TMP/files"
# Exclude only known generated roots, never every directory named build (ExecuTorch tracks one).
tar -C "$ROOT" --exclude-vcs --exclude='./dist' --exclude='./assets' \
    --exclude='./build' --exclude='./build-*' \
    --exclude='./packaging/arch/src' --exclude='./packaging/arch/pkg' --exclude='./packaging/arch/*.pkg.tar.*' \
    --exclude='./third_party/swipe-library/build' \
    --exclude='./third_party/swipe-library/build-asan' \
    --exclude='./third_party/swipe-library/third_party/executorch/cmake-out-*' \
    --exclude='.venv*' --exclude='__pycache__' --exclude='*.pyc' \
    --null --verbatim-files-from --no-recursion -T "$TMP/files" -cf - | tar -C "$DEST" -xf -
mkdir -p "$DEST/packaging"
{
    printf 'Local unpublished snapshot\n'
    printf 'Repository: https://github.com/itsdestin/swipe-keyboard\n'
    git -C "$ROOT" rev-parse HEAD
    git -C "$ROOT" diff --stat HEAD
    git -C "$ROOT" submodule status --recursive
    printf '\nChanges from recorded parent revision:\n'
    git -C "$ROOT" diff --binary HEAD
} > "$DEST/packaging/SOURCE-PROVENANCE.txt"
tar -C "$TMP" -czf "$OUT/swipe-keyboard-0.2.0.tar.gz" swipe-keyboard-0.2.0
sha256sum "$OUT/swipe-keyboard-0.2.0.tar.gz" > "$OUT/swipe-keyboard-0.2.0.tar.gz.sha256"
printf 'Source snapshot: %s\n' "$OUT/swipe-keyboard-0.2.0.tar.gz"
