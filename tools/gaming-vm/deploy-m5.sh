#!/bin/bash
# Copy the helper, the page layer, the shared contract and the M5 rig into the guest at ~/m5 (same relative layout as the repo).
# usage: tools/gaming-vm/deploy-m5.sh   (run on the host; needs the lab's guests/steamos-gaming/ssh wrapper)
# The engine daemon is NOT copied: it runs on the host and is forwarded in (tools/gaming-vm/m5_host.py up).
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SSH=/home/destin/.cache/swipe-keyboard-platforms/guests/steamos-gaming/ssh
# WHY not "rm -rf ~/m5": ~/m5/run holds the forwarded socket and token while the rig is up
tar -C "$REPO" --exclude=__pycache__ -cf - gaming/helper gaming/layer gaming/contract tools/gaming-vm/m5_acceptance.py \
  | "$SSH" 'mkdir -p ~/m5 && rm -rf ~/m5/gaming ~/m5/tools && tar -C ~/m5 -xf -'
