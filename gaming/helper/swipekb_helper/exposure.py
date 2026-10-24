# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Is the outside door to Steam's debug port closed? (owner decision 2026-10-22; plan Section 9)

SteamOS can forward Steam's debug port to every network interface (port 8081). The installer closes that with
an administrator step; this module lets the helper confirm, on its own and without any network call, that it
is still closed. It only reads the kernel's list of listening sockets (/proc/net/tcp and tcp6).

Address text in those files is hexadecimal, a 32-bit word at a time, least significant byte first. So the IPv4
loopback address is the word ending in 7F, and the IPv6 loopback is the word 01000000 after three zero words.
"""
from pathlib import Path

WATCHED_PORTS = (8080, 8081)   # Steam's own port, and SteamOS's all-interfaces forward of it
_LISTEN = '0A'
_V6_LOOPBACK = '00000000000000000000000001000000'
_V6_MAPPED_PREFIX = '0000000000000000FFFF0000'   # an IPv4 address carried inside an IPv6 socket


def _is_loopback(addr):
    if len(addr) == 8:
        return addr.endswith('7F')
    if len(addr) == 32:
        return addr == _V6_LOOPBACK or (addr.startswith(_V6_MAPPED_PREFIX) and addr.endswith('7F'))
    return False   # unknown shape: treat as reachable, the cautious reading


def open_doors(proc_dir='/proc/net', ports=WATCHED_PORTS):
    """Sorted list of watched ports that are listening on something other than loopback. Empty = door closed.

    WHY unreadable files give []: a missing or odd /proc means we cannot tell, and a helper that crashed (or
    refused to run anywhere unusual) would be worse than one that cannot check; /proc/net/tcp exists on any Linux.
    """
    found = set()
    for name in ('tcp', 'tcp6'):
        try:
            lines = (Path(proc_dir) / name).read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            f = line.split()
            if len(f) < 4 or f[3] != _LISTEN or ':' not in f[1]:
                continue
            addr, _, port_hex = f[1].partition(':')
            try:
                port = int(port_hex, 16)
            except ValueError:
                continue
            if port in ports and not _is_loopback(addr.upper()):
                found.add(port)
    return sorted(found)


def read_choice(state_dir):
    """What the person chose in the installer: 'open' (leave other devices able to reach Steam) or 'blocked'.

    WHY anything unreadable or unexpected means 'blocked': the strict reading is the safe one, and only the
    installer writes the word 'open', after the person picked it.
    """
    try:
        return 'open' if (Path(state_dir) / 'outside-access').read_text().strip() == 'open' else 'blocked'
    except OSError:
        return 'blocked'
