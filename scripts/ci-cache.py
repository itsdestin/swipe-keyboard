#!/usr/bin/env python3
"""Rules for the dependency-only CI caches (read policy, keys, validated Flatpak stage cache).

WHY a script: the rules decide whether a release-grade build may reuse earlier bytes,
so they are tested code rather than inline workflow shell. Nothing here caches the
keyboard's own objects, QRC or install output.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

IDENTITY = '.ci-cache-identity'

# Everything that changes the cached output. Directories are hashed recursively.
# A missing entry is an error, so a rename can never silently shrink a key.
_BUILD_SCRIPTS = ['CMakeLists.txt', 'cmake']
KEY_FILES = {
    # Flatpak Qt/Plasma module stages: manifest (sources, hashes, flags, jobs), exact SDK/runtime
    # commits, the Plasma patch inputs and the wrapper that fixes jobs/sandbox flags.
    'flatpak-stages': ['packaging/flatpak/org.itsdestin.SwipeKeyboard.json',
                       'packaging/flatpak/runtime-lock.json',
                       'packaging/flatpak/build.py',
                       'scripts/patches/plasma-prediction-policy.patch',
                       'scripts/patches/backendpredictionpolicy.h'],
    # Compiler results (content-addressed by ccache itself; the key only picks which store to start
    # from). The salt carries the toolchain.
    'flatpak-ccache': ['packaging/flatpak/org.itsdestin.SwipeKeyboard.json',
                       'packaging/flatpak/runtime-lock.json',
                       'packaging/flatpak/build-swipe.sh'] + _BUILD_SCRIPTS,
    'native-ccache': ['packaging/arch/PKGBUILD',
                      'packaging/fedora/swipe-keyboard.spec'] + _BUILD_SCRIPTS,
}


def policy(env):
    """Return (may_restore, may_save). Tags never use caches; only trusted pushes write."""
    ref = env.get('GITHUB_REF', '')
    event = env.get('GITHUB_EVENT_NAME', '')
    if env.get('GITHUB_REPOSITORY') != 'itsdestin/swipe-keyboard' or ref.startswith('refs/tags/'):
        return False, False
    if event in ('push', 'workflow_dispatch'):
        return True, ref.startswith('refs/heads/')
    if event == 'pull_request':
        # Read-only, and never for fork PRs (or when the fork flag is unknown).
        return env.get('PR_FROM_FORK') == 'false', False
    return False, False


def _expand(root, entries):
    files = []
    for entry in entries:
        path = Path(root)/entry
        if path.is_dir():
            files += sorted(p.relative_to(root).as_posix() for p in path.rglob('*') if p.is_file())
        elif path.is_file():
            files.append(entry)
        else:
            raise FileNotFoundError(f'cache key input missing: {entry}')
    return files


def digest_files(root, entries, salt):
    h = hashlib.sha256()
    h.update(salt.encode() + b'\0')
    for name in _expand(root, entries):
        h.update(name.encode() + b'\0' + hashlib.sha256((Path(root)/name).read_bytes()).digest())
    return h.hexdigest()[:32]


# WHY flatpak-ccache is v2: run 39827309452 saved an essentially empty store under the v1 key
# (the build had not run); GitHub keys are immutable, so v1 could never be refilled. v3/v2: the stores
# now also hold engine and product objects and use a per-commit key (see packages.yml).
KEY_VERSION = {'flatpak-ccache': 3, 'native-ccache': 2}


def key_prefix(name, label=''):
    # WHY a per-platform label: compiled caches are never shared across Arch, Fedora, ARM or Flatpak.
    version = KEY_VERSION.get(name, 1)
    return f'{name}-{label}-v{version}-' if label else f'{name}-v{version}-'


def cache_key(name, root, salt, label=''):
    return key_prefix(name, label) + digest_files(root, KEY_FILES[name], salt)


def pack_state(state, archive, identity):
    """Pack only flatpak-builder's stage cache (dependency stages), with an identity marker."""
    # WHY resolve: GNU tar applies each -C relative to the previous one.
    state, archive = Path(state).resolve(), Path(archive).resolve()
    with tempfile.TemporaryDirectory() as scratch:
        (Path(scratch)/IDENTITY).write_text(identity)
        # xattrs matter: ostree's bare-user stage store keeps ownership/mode in them.
        subprocess.run(['tar', '--xattrs', '--xattrs-include=user.*', '--posix', '-I', 'zstd -3 -T0',
                        '-cf', str(archive), '-C', scratch, IDENTITY, '-C', str(state), 'cache'], check=True)


def ostree_fsck(repo):
    try:
        # Output stays visible in the CI log so a rejection can be diagnosed.
        return subprocess.run(['ostree', 'fsck', '--repo=' + str(repo)]).returncode == 0
    except OSError:
        return False


def unpack_state(archive, state, identity, fsck=ostree_fsck):
    return restore_state(archive, state, identity, fsck)[0]


def restore_state(archive, state, identity, fsck=ostree_fsck):
    """Restore a stage cache only if layout, identity and object integrity all check out.

    Returns (usable, reason).
    """
    archive = Path(archive).resolve()
    state = Path(state).resolve()
    parent = state.parent
    parent.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(dir=parent, prefix='.restore-'))
    try:
        # Extract into a throwaway sibling first; GNU tar strips ../ and leading / itself.
        done = subprocess.run(['tar', '--xattrs', '--xattrs-include=user.*', '--no-same-owner',
                               '-xf', str(archive), '-C', str(scratch)], capture_output=True)
        if done.returncode != 0:
            return False, 'could not extract the archive: ' + done.stderr.decode(errors='replace')[:200]
        found = sorted(p.name for p in scratch.iterdir())
        if set(found) != {IDENTITY, 'cache'}:
            return False, f'unexpected layout {found}'
        if (scratch/IDENTITY).read_text() != identity:
            return False, 'identity marker does not match the expected key'
        # Symlinks inside the object store are file content (never followed here), so they are not checked.
        if not fsck(scratch/'cache'):
            return False, 'object integrity check (ostree fsck) failed'
        state.mkdir(parents=True, exist_ok=True)
        if (state/'cache').exists():
            shutil.rmtree(state/'cache')
        os.replace(scratch/'cache', state/'cache')
        return True, 'ok'
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
        if state.exists() and not (state/'cache').exists():
            try:
                state.rmdir()
            except OSError:
                pass


def emit(**values):
    """Write step outputs to $GITHUB_OUTPUT when running in Actions, else to stdout."""
    lines = ''.join(f'{k}={str(v).lower() if isinstance(v, bool) else v}\n' for k, v in values.items())
    target = os.environ.get('GITHUB_OUTPUT')
    if target:
        with open(target, 'a') as stream:
            stream.write(lines)
    else:
        print(lines, end='')


def main(argv):
    command = argv[1]
    if command == 'policy':
        restore, save = policy(os.environ)
        emit(restore=restore, save=save)
    elif command == 'key':
        # key NAME [SALT [LABEL]] -> prints the key (also the restore-keys prefix with --prefix)
        if argv[2] == '--prefix':
            print(key_prefix(argv[3], argv[4] if len(argv) > 4 else ''))
        else:
            print(cache_key(argv[2], Path(__file__).resolve().parents[1],
                            argv[3] if len(argv) > 3 else '', argv[4] if len(argv) > 4 else ''))
    elif command == 'pack-flatpak-state':
        pack_state(argv[2], argv[3], argv[4])
    elif command == 'unpack-flatpak-state':
        ok, reason = restore_state(argv[2], argv[3], argv[4])
        if not ok:
            print('::warning::Cached Flatpak dependency stages failed validation (' + reason + '); rebuilding them from source.')
        emit(usable=ok)
    else:
        raise SystemExit('usage: ci-cache.py policy|key NAME [SALT [LABEL]]|pack-flatpak-state STATE OUT ID|unpack-flatpak-state ARCHIVE STATE ID')


if __name__ == '__main__':
    main(sys.argv)
