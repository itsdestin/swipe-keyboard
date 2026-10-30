#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT
# build.sh TARGET [--install-deps] — build the Kubuntu / Debian package (card #502) in a source tree on that system.
#   TARGET        kubuntu-26.10 or debian-testing: which distro release the package is for, and so which builds the
#                 updater may install (deb/TARGET/latest.json on the update site; integration/update.py)
#   --install-deps  install the build tools from debian/control first (needs sudo; for clean VMs and CI images)
# Output: ../swipe-keyboard_VERSION_amd64.deb next to the source tree.
# WHY a script around dpkg-buildpackage: the version and the target come from one place (release-targets.json and the
# argument), so the recipe in packaging/debian/ never carries a version that can drift from the other packages.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TARGET="${1:-}"
case "$TARGET" in
    kubuntu-26.10|debian-testing) ;;
    *) echo "usage: $0 kubuntu-26.10|debian-testing [--install-deps]" >&2; exit 2 ;;
esac
cd "$ROOT"
read -r VERSION RELEASE < <(python3 -c 'import json; t=json.load(open("packaging/release-targets.json")); print(t["version"], t["release"])')
# WHY the target in the version: the two builds are different files (each built against its own distro), and their
# names must differ on a release page. Letters, digits and dots only: GitHub rewrites "~" and "+" in file names.
DEBVERSION="$VERSION-$RELEASE.${TARGET//-/}"
rm -rf debian && cp -a packaging/debian debian && rm -f debian/build.sh
cat > debian/changelog <<EOF
swipe-keyboard ($DEBVERSION) unstable; urgency=medium

  * Swipe Keyboard $VERSION-$RELEASE for $TARGET. Changes: CHANGELOG.md.

 -- Destin Moss <itsdestin@users.noreply.github.com>  $(date -R -d "@${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct 2>/dev/null || date +%s)}")
EOF
if [ "${2:-}" = --install-deps ]; then
    # WHY no sudo as root: CI containers run as root and have no sudo.
    SUDO=sudo; [ "$(id -u)" = 0 ] && SUDO=
    $SUDO apt-get update -q
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get build-dep -y -q ./
fi
SWIPEKB_UPDATE_TARGET="$TARGET" dpkg-buildpackage -b -uc -us
echo "built: $(cd .. && pwd)/swipe-keyboard_${DEBVERSION}_amd64.deb"
