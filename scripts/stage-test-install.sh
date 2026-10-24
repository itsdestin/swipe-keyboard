#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# stage-test-install.sh — turn a staged build (DESTDIR-style tree) into a throw-away Arch package for a first real
# test install, and roll back to the package that was installed before. Nothing here runs by itself: every command is
# an explicit sub-command, and only `install` / `rollback` touch the system (through sudo pacman).
#
# WHY a package and not copying files into /usr: the keyboard is a pacman package (swipe-keyboard 0.3.1-1, files under
# /usr/share/swipe-keyboard). A package keeps pacman's file list true, removes every file the old version had and the
# new one does not, and can be undone with one command. Copying over /usr would leave pacman and the disk disagreeing.
#
# Usage: scripts/stage-test-install.sh <command>
#   package   build <work>/pkg/swipe-keyboard-<ver>-<rel>.pkg.tar.zst from <work>/stage (no root needed)
#   verify    check the staged tree and the package against assets.lock.json (no root needed)
#   install   sudo pacman -U the test package, then ask the on-screen keyboard to restart (needs a terminal for sudo)
#   rollback  sudo pacman -U the saved previous package (checks its checksum first), then restart the keyboard
#   status    show the installed version and which trained models the installed copy contains
# Env: SWIPEKB_FULLBUILD (work folder, default ~/.cache/swipe-keyboard-train/fullbuild)
#      SWIPEKB_TEST_REL   (package release, default 1.1: newer than 1, so pacman treats it as an upgrade)
#      SWIPEKB_ROLLBACK_PKG (previous package file; default <work>/rollback/swipe-keyboard-0.3.1-1-x86_64.pkg.tar.zst)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SWIPEKB_FULLBUILD:-$HOME/.cache/swipe-keyboard-train/fullbuild}"
STAGE="$WORK/stage"
PKGDIR="$WORK/pkg"
VERSION="$(python3 -c "import json;print(json.load(open('$ROOT/packaging/release-targets.json'))['version'])")"
REL="${SWIPEKB_TEST_REL:-1.1}"
PKG="$PKGDIR/swipe-keyboard-$VERSION-$REL-x86_64.pkg.tar.zst"
ROLLBACK="${SWIPEKB_ROLLBACK_PKG:-$WORK/rollback/swipe-keyboard-0.3.1-1-x86_64.pkg.tar.zst}"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/swipe-keyboard"

pacman_install() {
    # WHY pkexec without a terminal: sudo can only ask for a password on a terminal; pkexec shows KDE's own password
    # dialog, so install/rollback also work when run from a chat shell. --noconfirm because there is no terminal to answer.
    if [ -t 0 ]; then sudo pacman -U "$1"; else pkexec pacman -U --noconfirm "$1"; fi
}

restart_keyboard() {
    # Same approach as install.sh: KWin starts a fresh keyboard after the old one exits; kill by pid only.
    local old; old=$(pidof plasma-keyboard || true)
    [ -n "$old" ] && { kill $old 2>/dev/null || true; }
    echo "on-screen keyboard asked to restart (KWin starts it again the next time a text field needs it)"
}

case "${1:-}" in
package)
    [ -d "$STAGE/usr/share/swipe-keyboard" ] || { echo "no staged tree at $STAGE (run the full build first)" >&2; exit 1; }
    python3 "$ROOT/scripts/package-check.py" "$STAGE"
    rm -rf "$PKGDIR/build"; mkdir -p "$PKGDIR/build"
    # WHY the installed package's own dependency list: the staged binaries are built on this machine against the same
    # libraries, and `pacman -Qi` is read-only.
    # WHY the upper bound is dropped: an installed older package still lists `plasma-keyboard<6.9`, which would make the
    # test build block a Plasma 6.9 upgrade just like that package does (packaging/arch/PKGBUILD no longer has it).
    DEPENDS=$(pacman -Qi swipe-keyboard | sed -n 's/^Depends On *: //p')
    {
        echo "pkgname=swipe-keyboard"; echo "pkgver=$VERSION"; echo "pkgrel=$REL"
        echo "pkgdesc='Swipe Keyboard test build (open engine + trained models)'"; echo "arch=('x86_64')"
        echo "url='https://github.com/itsdestin/swipe-keyboard'"
        # WHY the same licence list as packaging/arch/PKGBUILD (MIT where possible, KDE's GPL choice for the keyboard
        # add-on layers, each data file its own).
        echo "license=('MIT' 'GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL' 'Apache-2.0' 'ODC-By-1.0' 'CC-BY-4.0' 'CC0-1.0' 'custom:TATOEBA-CC-BY-2.0-FR.txt')"
        printf "depends=("; for d in $DEPENDS; do case "$d" in None|plasma-keyboard\<*) ;; *) printf "'%s' " "$d" ;; esac; done; echo ")"
        echo "options=('!strip' '!debug')"
        echo "package() { cp -a '$STAGE/usr' \"\$pkgdir/usr\"; }"
    } > "$PKGDIR/build/PKGBUILD"
    (cd "$PKGDIR/build" && PKGDEST="$PKGDIR" makepkg -f --nodeps --skipinteg)
    ls -l "$PKG"; sha256sum "$PKG" | tee "$PKG.sha256" ;;
verify)
    python3 "$ROOT/scripts/package-check.py" "$STAGE" && echo "staged tree OK (hashes, licences, trained models, profile checks)"
    [ -f "$PKG" ] && bsdtar -tf "$PKG" | grep -E "nwp.bin|swipe-model.bin|MODELS-NOTICE|SWIPE-DATA-LICENSE" ;;
install)
    [ -f "$PKG" ] || { echo "build the package first: $0 package" >&2; exit 1; }
    [ -f "$ROLLBACK" ] || { echo "rollback package missing: $ROLLBACK (refusing to install without a way back)" >&2; exit 1; }
    (cd "$(dirname "$ROLLBACK")" && sha256sum -c SHA256SUMS)
    echo "installed before: $(pacman -Q swipe-keyboard)"
    pacman_install "$PKG"
    restart_keyboard ;;
rollback)
    (cd "$(dirname "$ROLLBACK")" && sha256sum -c SHA256SUMS)
    # WHY no extra cleanup: pacman removes the test package's files as part of replacing it with the saved one.
    pacman_install "$ROLLBACK"
    restart_keyboard
    echo "now installed: $(pacman -Q swipe-keyboard)" ;;
status)
    pacman -Q swipe-keyboard
    pacman -Ql swipe-keyboard | grep -E "nwp.bin|swipe-model.bin" || echo "(no trained models in the installed copy)"
    ;;
*) sed -n 7,23p "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
