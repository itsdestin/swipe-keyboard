# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""swipekb-helper: Gaming Mode go-between (plan Sections 3, 6, 9). Standard library only.

M3 scope: find Steam's pages, check them against the shared contract, install the page layer frame (M4 adds the
gesture arbiter inside it), keep it installed through Steam restarts and UI reloads, and report status. Fail to nothing:
whatever goes wrong, Steam's own keyboard keeps working because we only ever add, and can always remove.
"""
# WHY a version string in the page: lets a newer helper replace an older stub, and lets a restarted helper
# see "already installed" and leave it alone (no double install).
LAYER_VERSION = '0.6.1'
