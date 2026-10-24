#!/usr/bin/env python3
"""Fetch only checksum-pinned base inputs. No optional model/evaluation downloads."""
import argparse
import base64
import hashlib
import json
import os
import pathlib
import shutil
import sys
import time
import urllib.error
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
    if not target.exists() and asset.get('path') == 'licenses/AOSP-NOTICE.b64':
        decoded = target.with_suffix('')
        # WHY: prepared archives retain the decoded notice, not its encoded download.
        # Re-encoding must match the ORIGINAL pin before it can authorize offline reuse.
        if decoded.is_file() and decoded.stat().st_size <= 1024 * 1024:
            encoded_digest = hashlib.sha256(base64.b64encode(decoded.read_bytes())).hexdigest()
            if encoded_digest == asset['sha256']:
                return
    part = target.with_name(target.name + '.part')
    for attempt in range(3):
        try:
            with urllib.request.urlopen(asset['url'], timeout=120) as response, part.open('wb') as out:
                shutil.copyfileobj(response, out)
            if not valid(part):
                raise ValueError('SHA256 mismatch: ' + asset['path'])
            part.replace(target)
            return
        except urllib.error.HTTPError as error:
            # WHY bounded retries: temporary service errors must not discard an entire
            # preparation run. Hash failures and permanent HTTP errors still fail closed.
            if error.code not in (408, 429, 500, 502, 503, 504) or attempt == 2:
                raise
            error.close()
            print(f"Retrying pinned input {asset.get('path', target.name)} after HTTP {error.code}", file=sys.stderr)
        finally:
            part.unlink(missing_ok=True)
        time.sleep(2 ** attempt)

def language_file_assets(lock):
    """The pinned next-word files (en/es/fr/de) and their credit/licence texts, from the public release.

    WHY from the release rather than the repository: the n-gram files are 100+ MB, too big to commit;
    they are published once (tag in the lock) and every build re-verifies them by SHA-256.
    """
    files = lock['language_files']
    base = files['baseUrl']
    assets = []
    for language, pin in lock['multilingual_context_models'].items():
        for name, key in ((f'{language}.ngram', 'modelSha256'), (f'{language}.vocab.tsv', 'vocabSha256'),
                          (f'{language}-CONTRIBUTORS.txt', 'contributorsSha256')):
            assets.append({'path': name, 'url': base + name, 'sha256': pin[key]})
    # The swipe dictionaries (es/fr/de, and English's dict/en_swipe.combined) are counted from the same text as
    # the n-gram files and ship in the same release.
    for pin in lock['language_dictionaries'].values():
        assets.append({'path': pin['path'], 'url': base + pin['path'], 'sha256': pin['sha256']})
    for key in ('odcTerms', 'notice', 'apacheTerms'):
        entry = files[key]
        assets.append({'path': entry['path'], 'url': base + entry.get('remote', entry['path']), 'sha256': entry['sha256']})
    return assets


def trained_model_assets(lock):
    """The trained models (release `trained_models` in the lock), as fetch() assets with their install-tree paths.

    WHY pinned: they are downloaded once from the public release and every build re-verifies them by SHA-256.
    Paths are inside <assets>/language-files (the language packs are generated from there).
    Test-only: SWIPEKB_TEST_MODELS_MIRROR (a folder or URL holding the same file names) replaces the release URL, so
    the install path can be exercised before the release exists; the pins still decide whether a file is accepted.
    """
    models = lock['trained_models']
    base = models['baseUrl']
    mirror = os.environ.get('SWIPEKB_TEST_MODELS_MIRROR', '')
    if mirror and '://' not in mirror:
        mirror = pathlib.Path(mirror).resolve().as_uri()
    def url(remote):
        return (mirror.rstrip('/') + '/' + remote) if mirror else base + remote
    assets = []
    for pin in models['nextWord'].values():
        assets.append({'path': 'language-files/' + pin['path'], 'url': url(pin['path']), 'sha256': pin['sha256']})
    swipe = models['swipe']
    assets.append({'path': swipe['path'], 'url': url(swipe['remote']), 'sha256': swipe['sha256']})
    for key in ('notice', 'swipeDataTerms'):
        entry = models[key]
        assets.append({'path': 'language-files/' + entry['path'], 'url': url(entry.get('remote', entry['path'])), 'sha256': entry['sha256']})
    return assets


def fetch_trained_models(assets, lock):
    for asset in trained_model_assets(lock):
        fetch(asset, assets/asset['path'])
    # WHY in licenses/ too: the MIT notice of the swipe model's training data and the models' credit/source notice
    # must ship with them.
    (assets/'licenses').mkdir(parents=True, exist_ok=True)
    for key in ('notice', 'swipeDataTerms'):
        name = lock['trained_models'][key]['path']
        shutil.copy2(assets/'language-files'/name, assets/'licenses'/name)


def fetch_language_files(assets):
    """Download into <assets>/language-files; generate_packs() copies them into the installed layout."""
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    for asset in language_file_assets(lock):
        fetch(asset, assets/'language-files'/asset['path'])
    # WHY also in licenses/: the ODC-By and Apache-2.0 terms must ship with the package's other licence texts.
    (assets/'licenses').mkdir(parents=True, exist_ok=True)
    for name in ('ODC-By-1.0.txt', 'Apache-2.0.txt'):
        shutil.copy2(assets/'language-files'/name, assets/'licenses'/name)
    # English's swipe dictionary is the pinned download itself (it used to be built from wordfreq at install time).
    english = lock['language_dictionaries']['en']
    (assets/'dict').mkdir(parents=True, exist_ok=True)
    shutil.copy2(assets/'language-files'/english['path'], assets/'dict/en_swipe.combined')
    fetch_trained_models(assets, lock)


def prepare_language_packs(assets):
    """Build-time only; the downloaded files are authenticated by fetch() and again by the generator."""
    import importlib.util
    if shutil.disk_usage(assets).free < 21 * 1024**3:
        raise ValueError('less than 21 GiB free')
    spec = importlib.util.spec_from_file_location('language_pack', ROOT/'scripts/dict/language_pack.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for language in ('es', 'fr', 'de'):
        size = module.generate(language, assets, assets/'language-packs')
        print(f'{language}: {size} dictionary bytes; quality not native-reviewed')
    # WHY after the three packs: English next-word files and the shared swipe model live beside them, so one folder
    # holds every language's files.
    module.generate_english_context(assets, assets/'language-packs')
    module.generate_shared(assets, assets/'language-packs')


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
    encoded = licenses/'AOSP-NOTICE.b64'
    if encoded.exists():
        (licenses/'AOSP-NOTICE').write_bytes(base64.b64decode(encoded.read_bytes()))
        encoded.unlink()
    shutil.copy2(ROOT/'packaging/NOTICE', licenses/'NOTICE')
    # WHY before the packs: they embed these files (and refuse to build on a hash mismatch).
    fetch_language_files(args.assets)
    # WHY normal preparation includes all selectable packs; no runtime network or user gate.
    prepare_language_packs(args.assets)
    print('Verified base inputs and language packs in', args.assets)

if __name__ == '__main__':
    main()
