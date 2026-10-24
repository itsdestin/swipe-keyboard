#!/bin/sh

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# Installs the Gaming Mode helper (gaming/helper/swipekb_helper, plain Python, standard library only) into the
# GamingHelper Flatpak. Run by flatpak-builder from the copied source tree; SWIPEKB_HELPER_PREFIX exists only so
# the packaging tests can run this into a temporary folder instead of /app.
set -eu
PREFIX="${SWIPEKB_HELPER_PREFIX:-/app}"
# WHY the check comes first: the helper is written on another branch. Until it lands the manifest still parses
# (gaming/ always exists), so without this line the build would "succeed" with an empty app that does nothing.
if [ ! -f gaming/helper/swipekb_helper/__main__.py ]; then
    echo 'GamingHelper build: gaming/helper/swipekb_helper/__main__.py is missing. The helper source has not been merged into this branch yet; nothing was built.' >&2
    exit 1
fi
install -d "$PREFIX/lib/swipekb-helper" "$PREFIX/share/swipekb-helper" "$PREFIX/bin"
cp -a gaming/helper/swipekb_helper "$PREFIX/lib/swipekb-helper/"
# WHY: never ship compiled leftovers from a developer checkout; /app is read-only at run time.
find "$PREFIX/lib/swipekb-helper" -name __pycache__ -prune -exec rm -rf {} +
# WHY: the same contract file the daily canary checks (plan Section 11); the helper reads it at run time.
install -m644 gaming/contract/gaming-contract.json "$PREFIX/share/swipekb-helper/gaming-contract.json"
# The injected page layer lives in gaming/layer/ (confirmed by M4; plan Section 4). The helper reads
# $PREFIX/share/swipekb-helper/layer/gesture.js at run time (swipekb_helper/config.py default_layer_dir) and pastes it into
# the frame it sends to Steam's page. Without it the helper still installs the frame but there are no gestures.
if [ -d gaming/layer ]; then
    cp -a gaming/layer "$PREFIX/share/swipekb-helper/layer"
else
    echo 'GamingHelper build: note, gaming/layer is absent; the helper will have no page layer to inject.' >&2
fi
# WHY -I and an explicit folder (review fix 8): the helper talks to Steam's debug port, so it must never import code from a
# place the caller controls. `-s -m` still put the current folder and PYTHONPATH on the import path; -I leaves out both and the
# user's site folder, so the package folder is added by hand (-I ignores PYTHONPATH, which is why it is no longer set). -B: /app is
# read-only, so no bytecode is written.
cat > "$PREFIX/bin/swipekb-helper" <<'WRAPPER'
#!/bin/sh
# WHY: Flatpak sets XDG_STATE_HOME to the app's private folder; the status file and the installer's outside-access
# choice both live in the real folder (shared by permission in the manifest).
export SWIPEKB_STATE_DIR="$HOME/.local/state/swipekb-gaming"
exec python3 -I -B -c 'import runpy, sys; sys.path.insert(0, "/app/lib/swipekb-helper"); runpy.run_module("swipekb_helper", run_name="__main__", alter_sys=True)' "$@"
WRAPPER
chmod 755 "$PREFIX/bin/swipekb-helper"
