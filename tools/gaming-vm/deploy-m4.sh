#!/bin/bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Copy the helper, the page layer, the shared contract and the M4 rig into the guest at ~/m4 (same relative layout as the repo).
# usage: tools/gaming-vm/deploy-m4.sh   (run on the host; needs the lab's guests/steamos-gaming/ssh wrapper)
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SSH=/home/destin/.cache/swipe-keyboard-platforms/guests/steamos-gaming/ssh
tar -C "$REPO" --exclude=__pycache__ -cf - gaming/helper gaming/layer gaming/contract tools/gaming-vm/m4_acceptance.py \
  | "$SSH" 'rm -rf ~/m4 && mkdir -p ~/m4 && tar -C ~/m4 -xf -'
