#!/usr/bin/env python3
"""Guarded patches for pinned build inputs; never weaken distro or metadata checks."""
from pathlib import Path
import argparse
import os
import subprocess
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


def apply_patch(root, patch):
    root = Path(root).resolve()
    patch = Path(patch).resolve()
    # WHY: archives lack .git. Upstream's `git -C executorch apply --check`
    # discovers CI's enclosing checkout and can silently skip every patch path,
    # returning success even for the reverse check on an unpatched tree.
    # Absolute --directory paths bind checks and writes to the extracted input.
    command = ['git', 'apply', '--unsafe-paths', '--directory=' + str(root)]
    if subprocess.run(command + ['--reverse', '--check', str(patch)],
                      capture_output=True).returncode == 0:
        return
    subprocess.run(command + ['--check', str(patch)], check=True)
    subprocess.run(command + [str(patch)], check=True)


def apply_swipe(root):
    # WHY: dependency commits/assets stay pinned. Persist the validator repair
    # here so clean builds reproduce it; reverse-check makes reruns idempotent
    # while unknown upstream inputs fail closed instead of silently skipping.
    root = Path(root).resolve()
    patch = Path(__file__).resolve().parent / 'patches/swipe-metadata.patch'
    apply_patch(root, patch)
    # Keep the pinned runtime's size optimization and its existing upstream
    # target-local exception fix, rather than enabling exceptions globally.
    apply_patch(root / 'third_party/executorch',
                root / 'patches/executorch-xnnpack-fexceptions.patch')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executorch')
    parser.add_argument('--swipe-library', type=Path)
    args = parser.parse_args()
    apply(args.executorch)
    if args.swipe_library is not None:
        apply_swipe(args.swipe_library)
