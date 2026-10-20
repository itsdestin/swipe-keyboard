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

def prepare_language_packs(assets, wheel=None, cc_license=None):
    """Build-time only; cached inputs are authenticated by the generator as well as fetch()."""
    import importlib.util
    import os
    if shutil.disk_usage(assets).free < 21 * 1024**3:
        raise ValueError('less than 21 GiB free')
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    cache = pathlib.Path(os.environ.get('SWIPEKB_BUILD_WHEEL_CACHE', pathlib.Path.home()/'.cache/swipe-keyboard/build-wheels'))
    wheel = pathlib.Path(wheel) if wheel else cache/'wordfreq-3.1.1-py3-none-any.whl'
    cc_license = pathlib.Path(cc_license) if cc_license else assets/'licenses/CC-BY-SA-4.0.txt'
    fetch({**lock['wordfreq_wheel'], 'path': 'wordfreq wheel'}, wheel)
    fetch({**lock['cc_license_source'], 'path': 'CC-BY-SA-4.0'}, cc_license)
    # WHY clean builders need the pinned parser too; never depend on an undeclared system msgpack.
    # Reuse the existing dictionary-only build venv rather than install packages system-wide.
    if importlib.util.find_spec('msgpack') is None:
        import subprocess
        import sys
        venv = assets/'dict-src/.venv'
        subprocess.run([sys.executable, str(ROOT/'scripts/provision-build-deps.py'), 'dictionary', str(venv)], check=True)
        for language in ('es', 'fr', 'de'):
            subprocess.run([str(venv/'bin/python'), str(ROOT/'scripts/dict/language_pack.py'),
                            '--wheel', str(wheel), '--language', language, '--encoder-root', str(assets),
                            '--cc-license', str(cc_license), '--out', str(assets/'language-packs')], check=True)
        return
    spec = importlib.util.spec_from_file_location('language_pack', ROOT/'scripts/dict/language_pack.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for language in ('es', 'fr', 'de'):
        size = module.generate(wheel, language, assets, cc_license, assets/'language-packs')
        print(f'{language}: {size} dictionary bytes; quality not native-reviewed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=pathlib.Path, default=ROOT/'assets')
    args = parser.parse_args()
    # WHY fail before growth; keep the reserved workspace floor even on normal provisioning.
    args.assets.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(args.assets).free < 21 * 1024**3:
        raise ValueError('less than 21 GiB free')
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
    # WHY normal preparation includes all selectable packs; no runtime network or user gate.
    prepare_language_packs(args.assets)
    print('Verified base inputs and language packs in', args.assets)

if __name__ == '__main__':
    main()
