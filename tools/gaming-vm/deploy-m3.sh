#!/bin/bash

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# Copy the helper, the shared contract and the M3 rig into the guest at ~/m3 (same relative layout as the repo).
# usage: tools/gaming-vm/deploy-m3.sh   (run on the host; needs the lab's guests/steamos-gaming/ssh wrapper)
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SSH=/home/destin/.cache/swipe-keyboard-platforms/guests/steamos-gaming/ssh
tar -C "$REPO" --exclude=__pycache__ -cf - gaming/helper gaming/contract tools/gaming-vm/m3_acceptance.py \
  | "$SSH" 'rm -rf ~/m3 && mkdir -p ~/m3 && tar -C ~/m3 -xf -'
