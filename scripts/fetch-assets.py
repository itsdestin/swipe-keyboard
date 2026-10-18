#!/usr/bin/env python3
"""Fetch only checksum-pinned base inputs. No optional model/evaluation downloads."""
import argparse
import hashlib
import json
import pathlib
import shutil
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]

def fetch(asset, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    def valid(path):
        if not path.is_file():
            return False
        with path.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest() == asset['sha256']
    if valid(target):
        return
    part = target.with_name(target.name + '.part')
    try:
        with urllib.request.urlopen(asset['url'], timeout=120) as response, part.open('wb') as out:
            shutil.copyfileobj(response, out)
        if not valid(part):
            raise ValueError('SHA256 mismatch: ' + asset['path'])
        part.replace(target)
    finally:
        part.unlink(missing_ok=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=pathlib.Path, default=ROOT/'assets')
    args = parser.parse_args()
    lock = ROOT/'packaging/assets.lock.json'
    for asset in json.loads(lock.read_text())['assets']:
        fetch(asset, args.assets / asset['path'])
    licenses = args.assets/'licenses'
    licenses.mkdir(parents=True, exist_ok=True)
    import base64
    encoded = licenses/'AOSP-NOTICE.b64'
    if encoded.exists():
        (licenses/'AOSP-NOTICE').write_bytes(base64.b64decode(encoded.read_bytes()))
        encoded.unlink()
    for source in (ROOT/'packaging/licenses').glob('*'):
        shutil.copy2(source, licenses/('wordfreq-' + source.name))
    dependency = ROOT/'third_party/swipe-library'
    for source in dependency.rglob('*'):
        if source.is_file() and source.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')):
            relative = source.relative_to(dependency)
            if not any(x.startswith(('.', 'build', 'cmake-out')) for x in relative.parts):
                target = licenses/'third-party'/relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
    shutil.copy2(ROOT/'packaging/NOTICE', licenses/'NOTICE')
    print('Verified base inputs in', args.assets)

if __name__ == '__main__':
    main()
