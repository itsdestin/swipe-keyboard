#!/usr/bin/env python3
"""Assemble and check the single SteamOS download (plan Section 15, question 2).

One .tar.gz holding both Flatpak bundles, install.sh, uninstall.sh, the two systemd units, a VERSION file and a
SHA256SUMS that install.sh checks before it changes anything. Python standard library only.
"""
import argparse
import gzip
import hashlib
import io
from pathlib import Path
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
SETUP = ROOT / 'gaming/setup'
SCRIPTS = ('install.sh', 'uninstall.sh', 'switch-watch.sh')
UNITS = ('swipekb-engine.service', 'swipekb-helper.service', 'swipekb-switch-watch.service', 'swipekb-switch-watch.timer')


def archive_name(version):
    return f'swipe-keyboard-{version}-steamos-x86_64.tar.gz'


def top_name(version):
    return f'swipe-keyboard-steamos-{version}'


def bundle_names(version):
    return f'swipe-keyboard-{version}-x86_64.flatpak', f'swipe-keyboard-gaming-helper-{version}-x86_64.flatpak'


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def expected_files(version):
    main, helper = bundle_names(version)
    return {*SCRIPTS, 'VERSION', 'SHA256SUMS', *('units/' + u for u in UNITS), main, helper}


def assemble(main_bundle, helper_bundle, version, out_dir, setup_dir=SETUP):
    """Write the download into out_dir and return its path. Same inputs give identical bytes."""
    main_name, helper_name = bundle_names(version)
    files = {
        'install.sh': ((setup_dir / 'install.sh').read_bytes(), 0o755),
        'uninstall.sh': ((setup_dir / 'uninstall.sh').read_bytes(), 0o755),
        'switch-watch.sh': ((setup_dir / 'switch-watch.sh').read_bytes(), 0o755),
        'VERSION': ((version + '\n').encode(), 0o644),
        main_name: (Path(main_bundle).read_bytes(), 0o644),
        helper_name: (Path(helper_bundle).read_bytes(), 0o644),
    }
    for unit in UNITS:
        files['units/' + unit] = ((setup_dir / 'units' / unit).read_bytes(), 0o644)
    # WHY every file except the checksum file itself: install.sh runs `sha256sum -c` on this list before it
    # touches Steam's folder, so a damaged or edited file in the download stops the install.
    sums = ''.join(f'{sha256(data)}  {name}\n' for name, (data, _) in sorted(files.items()))
    files['SHA256SUMS'] = (sums.encode(), 0o644)
    top = top_name(version)
    raw = io.BytesIO()
    # WHY fixed times, owners and a fixed gzip header: the release checksum must not depend on when it was built.
    with gzip.GzipFile(fileobj=raw, mode='wb', mtime=0) as gz, tarfile.open(fileobj=gz, mode='w', format=tarfile.PAX_FORMAT) as tar:
        def add(name, data=None, mode=0o755):
            info = tarfile.TarInfo(name)
            info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, '', ''
            if data is None:
                info.type, info.mode = tarfile.DIRTYPE, 0o755
                tar.addfile(info)
            else:
                info.size, info.mode = len(data), mode
                tar.addfile(info, io.BytesIO(data))
        add(top)
        add(top + '/units')
        for name, (data, mode) in sorted(files.items()):
            add(f'{top}/{name}', data, mode)
    out = Path(out_dir) / archive_name(version)
    out.write_bytes(raw.getvalue())
    return out


def verify(archive, version, setup_dir=SETUP):
    """Check the download is exactly what assemble makes; return the hashes of the two embedded bundles."""
    top = top_name(version)
    contents, modes = {}, {}
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            if member.isdir():
                if member.name not in (top, top + '/units'):
                    raise ValueError('Unexpected folder in SteamOS download: ' + member.name)
            elif member.isfile():
                contents[member.name] = tar.extractfile(member).read()
                modes[member.name] = member.mode
            else:
                raise ValueError('Links and special files are not allowed in the SteamOS download: ' + member.name)
    wanted = {f'{top}/{name}' for name in expected_files(version)}
    if set(contents) != wanted:
        raise ValueError('SteamOS download has missing or unexpected files')
    for script in SCRIPTS:
        if not modes[f'{top}/{script}'] & 0o111:
            raise ValueError(script + ' is not executable in the SteamOS download')
    if contents[f'{top}/VERSION'] != (version + '\n').encode():
        raise ValueError('SteamOS download VERSION does not match the release version')
    listed = {}
    for line in contents[f'{top}/SHA256SUMS'].decode().splitlines():
        digest, _, name = line.partition('  ')
        listed[name] = digest
    names = {n[len(top) + 1:] for n in contents} - {'SHA256SUMS'}
    if set(listed) != names:
        raise ValueError('SHA256SUMS inside the SteamOS download does not list exactly its files')
    for name in names:
        if listed[name] != sha256(contents[f'{top}/{name}']):
            raise ValueError('Checksum mismatch inside the SteamOS download: ' + name)
    # WHY compare with the committed files: a self-consistent but stale or hand-edited installer would
    # otherwise pass, and it is the one thing in the download the user runs.
    for name in (*SCRIPTS, *('units/' + u for u in UNITS)):
        if contents[f'{top}/{name}'] != (setup_dir / name).read_bytes():
            raise ValueError(name + ' differs from the committed gaming/setup file')
    main, helper = bundle_names(version)
    return {'main_sha256': sha256(contents[f'{top}/{main}']), 'helper_sha256': sha256(contents[f'{top}/{helper}'])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    build = sub.add_parser('assemble')
    build.add_argument('--main-bundle', type=Path, required=True)
    build.add_argument('--helper-bundle', type=Path, required=True)
    build.add_argument('--version', required=True)
    build.add_argument('--out', type=Path, required=True)
    check = sub.add_parser('verify')
    check.add_argument('archive', type=Path)
    check.add_argument('--version', required=True)
    args = parser.parse_args()
    if args.command == 'assemble':
        print(assemble(args.main_bundle, args.helper_bundle, args.version, args.out))
    else:
        print(verify(args.archive, args.version))
    return 0


if __name__ == '__main__':
    sys.exit(main())
