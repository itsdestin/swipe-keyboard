#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
# SPDX-License-Identifier: BSD-2-Clause
"""Fails when a Plasma 6.9 front-end binary carries or needs FUTO's or ExecuTorch's code.

WHY: the owner decided (2026-10-23) that the 6.9 front end must not use FUTO at all. Symbols are read with nm
(defined and undefined, so a dynamic dependency is caught too) and libraries with readelf.
"""
import re
import subprocess
import sys

FORBIDDEN_SYMBOL = re.compile(r'swipe_decoder|executorch|ContextLMScorer|torch::|_ZN5torch')
FORBIDDEN_LIBRARY = re.compile(r'libswipekb_engine|executorch|libswipekeyboard\.so|VirtualKeyboard')


def main(paths):
    failures = []
    for path in paths:
        symbols = subprocess.run(['nm', '-C', '--dynamic', path], check=True, capture_output=True, text=True).stdout
        symbols += subprocess.run(['nm', '-C', path], capture_output=True, text=True).stdout
        for line in symbols.splitlines():
            if FORBIDDEN_SYMBOL.search(line):
                failures.append(f'{path}: symbol {line.strip()}')
                break
        dynamic = subprocess.run(['readelf', '-d', path], check=True, capture_output=True, text=True).stdout
        for line in dynamic.splitlines():
            if 'NEEDED' in line and FORBIDDEN_LIBRARY.search(line):
                failures.append(f'{path}: needs {line.strip()}')
    for failure in failures:
        print(failure)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
