#!/usr/bin/env python3
"""Provision locked CPython 3.14/x86_64 build venvs; never install torch itself."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import urllib.request
import venv
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
LOCKS = ROOT/'requirements'

def fetch(item, cache):
    digest = item.get('sha256', '')
    if not re.fullmatch('[0-9a-f]{64}', digest):
        raise ValueError('Missing/invalid sha256 for wheel')
    filename = item['filename']
    if pathlib.Path(filename).name != filename:
        raise ValueError('Invalid wheel filename')
    cache.mkdir(parents=True, exist_ok=True)
    target = cache/filename
    def valid(path):
        if not path.is_file():
            return False
        with path.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest() == digest
    if valid(target):
        return target
    part = target.with_name(filename + '.part')
    try:
        with urllib.request.urlopen(item['url'], timeout=180) as response, part.open('wb') as stream:
            shutil.copyfileobj(response, stream)
        if not valid(part):
            raise ValueError('Wheel checksum mismatch: ' + filename)
        part.replace(target)
    finally:
        part.unlink(missing_ok=True)
    return target

def install(python, requirements, cache):
    subprocess.run([str(python), '-I', '-m', 'pip', '--isolated', 'install',
                    '--require-hashes', '--no-deps', '--no-index', '--only-binary=:all:',
                    '--find-links', str(cache), '-r', str(requirements)], check=True)

def provision(group, destination, cache):
    if sys.version_info[:2] != (3, 14) or platform.machine() != 'x86_64' or sys.platform != 'linux':
        raise RuntimeError('Locks target CPython 3.14 Linux x86_64 only')
    manifest = json.loads((LOCKS/'wheels.lock.json').read_text())
    requirements = LOCKS/(group + '.txt')
    # Validate requirements and manifest agree before fetching or executing anything.
    expected = ''.join(f"{name}=={manifest['wheels'][name]['version']} --hash=sha256:{manifest['wheels'][name]['sha256']}\n"
                       for name in manifest['groups'][group])
    if requirements.read_text() != expected:
        raise ValueError('Requirements/manifest lock mismatch')
    wheels = {name: fetch(manifest['wheels'][name], cache) for name in manifest['groups'][group]}
    torch = fetch(manifest['torchgen'], cache) if group == 'futo' else None
    fingerprint = hashlib.sha256((requirements.read_bytes() + (LOCKS/'wheels.lock.json').read_bytes())).hexdigest()
    marker = destination/'.swipe-build-lock'
    if marker.is_file() and marker.read_text() == fingerprint and (destination/'bin/python3').is_file():
        return
    # Do not trust an existing mutable venv or upstream Makefile's auto-provisioning.
    if destination.exists():
        shutil.rmtree(destination)
    venv.EnvBuilder(with_pip=False).create(destination)
    python = destination/'bin/python3'
    # Bootstrap pip by extracting the already hash-verified wheel. No ensurepip or
    # mutable installer runs; subsequent installation is entirely offline/hashed.
    site = pathlib.Path(subprocess.check_output([str(python), '-I', '-c',
        'import sysconfig; print(sysconfig.get_path("purelib"))'], text=True).strip())
    with zipfile.ZipFile(wheels['pip']) as archive:
        archive.extractall(site)
    install(python, requirements, cache)
    if torch:
        with zipfile.ZipFile(torch) as archive:
            for member in archive.infolist():
                path = pathlib.PurePosixPath(member.filename)
                if path.parts and path.parts[0] == 'torchgen' and '..' not in path.parts:
                    archive.extract(member, site)
        if not (site/'torchgen/__init__.py').is_file():
            raise ValueError('Pinned torch wheel lacks torchgen')
    subprocess.run([str(python), '-I', '-m', 'pip', '--isolated', 'check'], check=True)
    marker.write_text(fingerprint)
    print('Provisioned hash-locked', group, 'dependencies:', destination)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('group', choices=('futo', 'dictionary'))
    parser.add_argument('destination', type=pathlib.Path)
    parser.add_argument('--cache', type=pathlib.Path, default=pathlib.Path(os.environ.get('XDG_CACHE_HOME', pathlib.Path.home()/'.cache'))/'swipe-keyboard/build-wheels')
    args = parser.parse_args()
    provision(args.group, args.destination.resolve(), args.cache.resolve())

if __name__ == '__main__':
    main()
