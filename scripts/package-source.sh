#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Local, unpublished source snapshot of selected Git-tracked files.
# New release files must be explicitly staged first; arbitrary untracked/private files never enter it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$ROOT/packaging/release-targets.json")"
DEST="$TMP/swipe-keyboard-$VERSION"
mkdir "$DEST"
# WHY a tracked-file allowlist: a directory blacklist can leak personal untracked files or
# previously built packages. Read current contents only for explicitly selected Git source paths.
git -C "$ROOT" ls-files --cached -z | \
    python3 -c 'import sys; sys.stdout.buffer.write(b"".join(b"./" + p + b"\0" for p in sys.stdin.buffer.read().split(b"\0") if p))' > "$TMP/files"
# Exclude only known generated roots.
tar -C "$ROOT" --exclude-vcs --exclude='./dist' --exclude='./assets' \
    --exclude='./build' --exclude='./build-*' \
    --exclude='./packaging/arch/src' --exclude='./packaging/arch/pkg' --exclude='./packaging/arch/*.pkg.tar.*' \
    --exclude='.venv*' --exclude='__pycache__' --exclude='*.pyc' \
    --null --verbatim-files-from --no-recursion -T "$TMP/files" -cf - | tar -C "$DEST" -xf -
mkdir -p "$DEST/packaging"
{
    printf 'Local unpublished snapshot\n'
    printf 'Repository: https://github.com/itsdestin/swipe-keyboard\n'
    git -C "$ROOT" rev-parse HEAD
    git -C "$ROOT" diff --stat HEAD
    printf '\nChanges from recorded parent revision:\n'
    git -C "$ROOT" diff --binary HEAD
} > "$DEST/packaging/SOURCE-PROVENANCE.txt"
tar -C "$TMP" -czf "$OUT/swipe-keyboard-$VERSION.tar.gz" "swipe-keyboard-$VERSION"
sha256sum "$OUT/swipe-keyboard-$VERSION.tar.gz" > "$OUT/swipe-keyboard-$VERSION.tar.gz.sha256"
printf 'Source snapshot: %s\n' "$OUT/swipe-keyboard-$VERSION.tar.gz"
