#!/usr/bin/env python3
"""Prepare shared, complete source and a portable external Flatpak recipe (CI only)."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.parse

import release

ROOT = release.ROOT


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def external_manifest(manifest, source_archive, inputs, fetcher):
    """Materialize URL-pinned inputs and bind an already finalized source archive."""
    for mod in manifest['modules']:
        for item in mod['sources']:
            if 'url' in item:
                name = item['sha256'] + '-' + urllib.parse.unquote(Path(urllib.parse.urlparse(item['url']).path).name)
                fetcher.fetch({**item, 'path': name}, inputs/name)
                if item['type'] == 'file':
                    item.setdefault('dest-filename', urllib.parse.unquote(Path(urllib.parse.urlparse(item['url']).path).name))
                del item['url']
                item['path'] = name
            elif item.get('path') == 'swipe-source.tar.xz':
                # WHY reuse the separately required source file, not another gigabyte copy.
                item['path'] = '../' + source_archive.name
                item['sha256'] = release.digest(source_archive)
            elif 'path' in item:
                original = (ROOT/'packaging/flatpak'/item['path']).resolve()
                shutil.copy2(original, inputs/original.name)
                item['path'] = original.name
    pins = json.loads((ROOT/'packaging/flatpak/runtime-lock.json').read_text())
    manifest['sdk-commit'] = pins['org.kde.Sdk']
    manifest['runtime-commit'] = pins['org.kde.Platform']
    return manifest


def prepare(output):
    release.check_versions(release.load_contract())
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=ROOT, text=True):
        raise ValueError('CI source must be a clean committed checkout')
    sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    contract = release.load_contract()
    output.mkdir(parents=True)
    subprocess.run(['bash', str(ROOT/'scripts/package-source.sh'), str(output)], check=True)
    source_name = release.filename(contract['targets']['source']['files'][0], contract)
    work = output/'preparation'
    work.mkdir()
    # Trusted archive just emitted from our tracked-file allowlist, not downloaded input.
    with tarfile.open(output/source_name) as archive:
        archive.extractall(work, filter='data')
    source = work/('swipe-keyboard-' + contract['version'])
    (source/'packaging/source-identity.json').write_text(json.dumps({'source_sha': sha, 'version': release.version(contract)}) + '\n')
    wheels = module('wheels', ROOT/'scripts/provision-build-deps.py')
    for profile in ('', 'kde-6.10'):
        lock = json.loads((ROOT/'requirements'/profile/'wheels.lock.json').read_text())
        for item in [*lock['wheels'].values(), lock['torchgen']]:
            wheels.fetch(item, source/'wheelcache')
    # WHY the existing wrapper: fetch-assets.py alone does not build the pinned English dictionary.
    # Run copied source helpers so their ROOT resolves the prepared authenticated wheelcache.
    environment = {**os.environ, 'SWIPEKB_ASSETS': str(source/'assets'), 'SWIPEKB_FETCH_DEVELOPER_EXTRAS': '0'}
    subprocess.run(['bash', str(source/'scripts/fetch-assets.sh')], check=True, env=environment)
    if release.digest(source/'assets/dict/en_swipe.combined') != json.loads((source/'packaging/assets.lock.json').read_text())['dictionary_sha256']:
        raise ValueError('Prepared English dictionary differs from the pinned output')
    # Preserve raw dictionary source inputs; only the interpreter-specific tool venv is discarded.
    shutil.rmtree(source/'assets/dict-src/.venv', ignore_errors=True)
    fetcher = module('assets', ROOT/'scripts/fetch-assets.py')
    # Replace only after all downloads/generation passed. No generated recipe inside this archive:
    # its source hash would otherwise refer to itself.
    with tarfile.open(output/source_name, 'w:gz') as archive:
        archive.add(source, arcname=source.name)
    inputs = work/'flatpak-inputs'
    inputs.mkdir()
    manifest = json.loads((ROOT/'packaging/flatpak/org.itsdestin.SwipeKeyboard.json').read_text())
    # All SDK wheels are already authenticated inside the required complete source archive.
    # Avoid uploading the same torch code-generator wheel again as a second Flatpak source.
    manifest['modules'][-1]['sources'] = [s for s in manifest['modules'][-1]['sources'] if s.get('dest') != 'wheelcache']
    manifest = external_manifest(manifest, output/source_name, inputs, fetcher)
    # The source includes the full SDK lock cache; preserve manifest's checked upstream inputs too.
    (inputs/'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    shutil.copy2(ROOT/'packaging/flatpak/runtime-lock.json', inputs/'runtime-lock.json')
    name = release.filename(contract['targets']['flatpak']['files'][1], contract)
    with tarfile.open(output/name, 'w:gz') as archive:
        archive.add(inputs, arcname='flatpak-inputs')
    (output/(source_name + '.sha256')).unlink()
    shutil.rmtree(work)
    records = [{'name': n, 'size': (output/n).stat().st_size, 'sha256': release.digest(output/n)}
               for n in sorted(release.input_names(contract))]
    (output/'input-manifest.json').write_text(json.dumps({'source_sha': sha, 'version': release.version(contract), 'files': records}, indent=2) + '\n')
    release.verify_inputs(output, sha, contract)
    print('Prepared complete source and external Flatpak inputs:', sha)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    prepare(parser.parse_args().output.resolve())
