#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Arch dependencies from direct ELF needs, not the recursive build-host library graph."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def command(args):
    return subprocess.run(args, check=True, text=True, capture_output=True,
                          env={**os.environ, 'LC_ALL': 'C'}).stdout


def version(value):
    return tuple(int(part) for part in value.split('.'))


def gcc_floor(tags):
    # WHY explicit ABI releases: a compiler package revision says nothing about which
    # libstdc++ symbols we import. Unknown future tags fail packaging, not user activation.
    # GCC libstdc++ ABI policy: https://gcc.gnu.org/onlinedocs/libstdc++/manual/abi.html
    glibcxx = ['3.4', '3.4.1', '3.4.2', '3.4.3', '4', '4.0.1', '4.0.2', '4.0.3',
               '4.1.1', '4.2', '4.3', '4.4', '4.4.1', '4.4.2', '4.5', '4.6', '4.6.1',
               '4.7', '4.8', '4.8.3', '4.9', '5.1', '6.1', '7.1', '7.2', '8.1',
               '9.1', '9.2', '9.3', '11.1', '12.1', '13.1', '13.2', '14.1', '15.1']
    cxxabi = ['3.4', '4', '4.3', '4.4', '4.5', '4.6', '4.7', '4.8', '4.9',
              '5.1', '6.1', '7.1', '9.1', '11.1', '13.1', '14.1', '15.1', '16.1']
    floors = []
    for tag in tags:
        # WHY x86_64 only: GCC documents 1.3.16 for RISC-V, not an x86 symbol floor.
        # Guessing a compiler version would claim availability we have not established.
        if tag == 'CXXABI_1.3.16':
            raise ValueError('RISC-V-only CXXABI_1.3.16 is unsupported for this x86_64 package')
        if tag.startswith(('GLIBCXX_', 'CXXABI_')):
            prefix, number = tag.split('_', 1)
            base, table = ('3.4', glibcxx) if prefix == 'GLIBCXX' else ('1.3', cxxabi)
            if number == base:
                index = 0
            elif number.startswith(base + '.') and number[len(base)+1:].isdigit():
                index = int(number[len(base)+1:])
            else:
                raise ValueError('Unknown C++ ABI requirement: ' + tag)
            if index >= len(table):
                raise ValueError('Unknown C++ ABI requirement: ' + tag)
            floors.append(table[index])
        elif re.fullmatch(r'GCC_\d+(?:\.\d+)*', tag):
            floors.append(tag[4:])
    return max(floors, key=version) if floors else None


# WHY not a hard dependency: only the Plasma 6.7/6.8 front end (qml/SwipeKeyboard) links Qt Virtual Keyboard, and on those
# versions plasma-keyboard itself requires it. Plasma 6.9's rebuilt keyboard has no Qt Virtual Keyboard; requiring it
# would keep (or block) a package the system no longer needs. The PKGBUILD lists it as an optional dependency.
LEGACY_ONLY_SONAMES = {'libQt6VirtualKeyboard.so.6'}


def dependencies(root, run=command):
    base = Path(root) / 'usr/share/swipe-keyboard'
    # Both front ends: Plasma 6.7/6.8 (SwipeKeyboard) and Plasma 6.9 (SwipeKeyboard69).
    libraries = sorted((base / 'qml/SwipeKeyboard').glob('*.so*')) + sorted((base / 'qml/SwipeKeyboard69').glob('*.so*'))
    if not libraries:
        raise ValueError('No staged native libraries')
    local = {p.name for p in libraries}
    # WHY both executables: the ownership probe can introduce independent direct
    # libraries and newer symbol needs that none of the QML libraries import.
    for name in ('swipekb-preflight', 'swipekb-config'):
        helper = base / 'integration' / name
        if helper.is_file():
            libraries.append(helper)
    needed, tags = set(), set()
    for library in libraries:
        dynamic = run(['readelf', '-W', '--dynamic', str(library)])
        needed.update(re.findall(r'\(NEEDED\).*\[([^]]+)\]', dynamic))
        # Only version NEEDS, never definitions exported by our own libraries.
        info = run(['readelf', '-W', '--version-info', str(library)])
        needs = info.split('Version needs section', 1)[-1] if 'Version needs section' in info else ''
        tags.update(re.findall(r'Name: (\S+)', needs))
    if any('PRIVATE' in tag for tag in tags):
        raise ValueError('Unexpected private ABI dependency: ' + ', '.join(sorted(tags)))
    cache = run(['ldconfig', '-p'])
    providers = {}
    for line in cache.splitlines():
        match = re.search(r'^\s*(\S+) \([^)]*x86-64[^)]*\) => (\S+)', line)
        if match:
            providers.setdefault(match[1], match[2])
    qt = json.loads((base / 'runtime.json').read_text())['qt']
    if not re.fullmatch(r'6\.\d+', qt) or version(qt) < (6, 8):
        raise ValueError('Invalid Qt build baseline: ' + str(qt))
    glibc = [tag[6:] for tag in tags if re.fullmatch(r'GLIBC_\d+(?:\.\d+)*', tag)]
    cxx = gcc_floor({tag for tag in tags if tag.startswith(('GLIBCXX_', 'CXXABI_'))})
    gcc = gcc_floor({tag for tag in tags if tag.startswith('GCC_')})
    result = {}
    for soname in sorted(needed - local - LEGACY_ONLY_SONAMES):
        if soname not in providers:
            raise ValueError('Unresolved direct ELF dependency: ' + soname)
        owner = run(['pacman', '-Qoq', providers[soname]]).strip()
        if not re.fullmatch(r'[a-zA-Z0-9@_+.-]+', owner):
            raise ValueError('Invalid package owner: ' + owner)
        floor = None
        if soname.startswith('libQt6'):
            floor = qt
        elif owner == 'glibc' and glibc:
            floor = max(glibc, key=version)
        # WHY sonames, not provider names: Arch also splits gcc-libs into libstdc++
        # and libgcc packages. Their required ABI is unchanged by that packaging split.
        elif soname == 'libstdc++.so.6':
            floor = cxx
        elif soname == 'libgcc_s.so.1':
            floor = gcc
        if owner not in result or (floor and (not result[owner] or version(floor) > version(result[owner]))):
            result[owner] = floor
    return sorted(owner + ('>=' + floor if floor else '') for owner, floor in result.items())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root')
    print('\n'.join(dependencies(parser.parse_args().root)))
