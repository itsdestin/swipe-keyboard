#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# Swipe Keyboard for Steam Deck: daily automatic update (run by swipekb-update.timer; plan:
# docs/active/plans/2026-10-23-auto-updates.md). It asks Flatpak to update only our two apps from the signed update
# site that install.sh set up. Flatpak refuses anything not signed with the Swipe Keyboard update key.
# Turn it off: bash install.sh --no-auto-update (in the download), or uninstall.sh to remove everything.
set -uo pipefail
MAIN_ID=org.itsdestin.SwipeKeyboard
HELPER_ID=org.itsdestin.SwipeKeyboard.GamingHelper

commit() { flatpak info --user --show-commit "$1" 2>/dev/null || true; }

apps=()
before_main="$(commit "$MAIN_ID")"; [ -n "$before_main" ] && apps+=("$MAIN_ID")
before_helper="$(commit "$HELPER_ID")"; [ -n "$before_helper" ] && apps+=("$HELPER_ID")
[ "${#apps[@]}" -gt 0 ] || { echo "Swipe Keyboard is not installed; nothing to update."; exit 0; }

# WHY --noninteractive -y: nobody is there to answer. A failure (offline, site down) leaves the installed version
# working and is simply tried again tomorrow.
# WHY one app at a time: one refused update (seen in the VM: Flatpak refused the helper as "older") must not hold
# back the other app.
failed=0
for app in "${apps[@]}"; do
    flatpak update --user --noninteractive -y "$app" || failed=1
done

after_main="$(commit "$MAIN_ID")"; after_helper="$(commit "$HELPER_ID")"
if [ "$after_main" != "$before_main" ] || [ "$after_helper" != "$before_helper" ]; then
    echo "Swipe Keyboard was updated."
    # WHY try-restart: the Gaming Mode services run the app's files from when they started; restart them onto the new
    # version (only if they are running). The Desktop Mode keyboard picks it up at the next login, never mid-typing.
    systemctl --user try-restart swipekb-engine.service swipekb-helper.service 2>/dev/null || true
    if command -v notify-send >/dev/null 2>&1; then
        notify-send --app-name='Swipe Keyboard' 'Swipe Keyboard was updated' \
            'The new version is used from your next login. In Gaming Mode it is already in use.' 2>/dev/null || true
    fi
else
    echo "Swipe Keyboard is up to date."
fi
exit "$failed"
