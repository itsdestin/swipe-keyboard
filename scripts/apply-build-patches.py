#!/usr/bin/env python3
"""Guarded build-only patch for the pinned ExecuTorch source; never weaken distro linker flags."""
from pathlib import Path
import os
import sys
import tempfile


def apply(root):
    path = Path(root) / 'third-party/CMakeLists.txt'
    source = path.read_text()
    old = '-DFLATBUFFERS_BUILD_TESTS=OFF\n             -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>'
    # WHY: ExecuTorch's flatbuffers ExternalProject discards inherited CXXFLAGS, but
    # retains Fedora's hardened PIE LDFLAGS. Like its neighboring flatcc tool, flatc
    # must compile position-independent code or Fedora fails with R_X86_64_32 relocations.
    new = '-DFLATBUFFERS_BUILD_TESTS=OFF\n             -DCMAKE_POSITION_INDEPENDENT_CODE=ON\n             -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>'
    if source.count(new) == 1:
        return
    if source.count(old) != 1:
        raise ValueError('Unknown ExecuTorch flatbuffers configuration; refusing to patch it.')
    # Replace rather than write in place, so build input copies cannot share a mutated inode.
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(source.replace(old, new, 1))
    temporary.chmod(path.stat().st_mode & 0o777)
    os.replace(temporary, path)


if __name__ == '__main__':
    apply(sys.argv[1])
