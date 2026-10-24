#!/bin/sh

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# learning_choice DATA_DIR — decides whether "learn from my typing" is switched on, and says so plainly.
# Sourced by install.sh; kept in its own file so the question can be tested without installing anything.
#
# The choice is one file in the keyboard's data folder: `learn-typing` present = on. Nothing is learned without it. Later: `python3 DATA_DIR/learning.py on|off|forget|status`, or, for
# the native packages, `swipe-keyboard learning on|off|forget|status`.
#
# WHY default OFF everywhere except an explicit "y": typing is private, so only a person who answered yes at a
# terminal (or set SWIPEKB_LEARN_TYPING=yes on purpose) gets it. Packages (Arch/Fedora/Flatpak) and installs without a
# terminal never ask and never turn it on.
learning_choice() {
    data=$1
    flag="$data/learn-typing"
    mkdir -p "$data"
    if [ -f "$flag" ]; then
        echo "Learning from your typing is already ON (turn off: rm \"$flag\"; erase what it learned: python3 \"$data/learning.py\" forget)."
        return 0
    fi
    case "${SWIPEKB_LEARN_TYPING:-}" in
        yes|on|1) : > "$flag"; echo "Learning from your typing: ON (SWIPEKB_LEARN_TYPING)."; return 0 ;;
        no|off|0) echo "Learning from your typing: off (SWIPEKB_LEARN_TYPING)."; return 0 ;;
    esac
    if [ -t 0 ] && [ -t 1 ]; then
        cat <<TEXT

Learn from your typing?
  The keyboard can remember the words you use (names, slang), the words you often write together and your
  favourites, and offer them sooner. Everything stays on this computer, in $data/learning, readable only by you.
  It is never sent anywhere, never learned from password, e-mail, web-address or number fields, and fades if you
  stop using a word. You can turn it off or erase everything at any time.
TEXT
        printf 'Turn on learning from your typing? [y/N] '
        answer=
        read -r answer || answer=
        case "$answer" in
            y|Y|yes|YES|Yes) : > "$flag"
                echo "Learning is ON. Turn off: rm \"$flag\"   Erase what it learned: python3 \"$data/learning.py\" forget" ;;
            *) echo "Learning stays off. Turn on later: python3 \"$data/learning.py\" on" ;;
        esac
    else
        echo "Learning from your typing is off (no terminal to ask). Turn on later: python3 \"$data/learning.py\" on"
    fi
}
