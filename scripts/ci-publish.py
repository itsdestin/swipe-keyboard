#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Trusted final same-run job only; never overwrite an existing release (including v0.2.0)."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import urllib.request

import release


def publish():
    sha = os.environ['GITHUB_SHA']
    contract = release.load_contract()
    tag = 'v' + release.version(contract)
    if (os.environ.get('GITHUB_EVENT_NAME') != 'push' or
            os.environ.get('GITHUB_REPOSITORY') != 'itsdestin/swipe-keyboard' or
            os.environ.get('GITHUB_REF') != 'refs/tags/' + tag):
        raise ValueError('Publication requires the matching trusted repository tag push')
    subprocess.run(['git', 'merge-base', '--is-ancestor', sha, 'refs/remotes/origin/main'], check=True)
    if subprocess.check_output(['git', 'rev-parse', tag + '^{commit}'], text=True).strip() != sha:
        raise ValueError('Tag/source mismatch')
    directory = Path('release')
    manifest = release.verify(directory, sha, contract)
    if not (directory/'SHA256SUMS').is_file():
        raise ValueError('Required checksums missing')
    # Refuse both published and draft releases. A failed draft needs explicit owner review,
    # not a rerun that silently replaces previously uploaded bytes.
    exists = subprocess.run(['gh', 'release', 'view', tag], capture_output=True)
    if exists.returncode == 0:
        raise ValueError('Release already exists; no replacement permitted')
    names = [r['name'] for r in manifest['files']] + ['release-manifest.json', 'SHA256SUMS']
    # WHY: all current keyboard releases are test releases; do not silently promote
    # one to the stable/latest channel when replacing the former manual procedure.
    subprocess.run(['gh', 'release', 'create', tag, '--verify-tag', '--target', sha, '--draft', '--prerelease',
                    '--title', 'Swipe Keyboard ' + release.version(contract), '--notes',
                    'Arch/CachyOS x86_64, Fedora 44 x86_64/aarch64 (ARM64 Linux Plasma Mobile) and SteamOS Desktop Flatpak x86_64. '
                    'Complete corresponding source and SHA256SUMS included. CI checks package/build/loader contracts; '
                    'physical compositor/input acceptance is separate and must identify package hashes. '
                    'ARM session acceptance is required; emulated ARM input does not certify physical-phone performance. '
                    'Android, iOS, musl and Gaming Mode are not certified.\n\n'
                    f'[Install and enable](https://github.com/itsdestin/swipe-keyboard/blob/{tag}/README.md#install) · '
                    f'[Release scope](https://github.com/itsdestin/swipe-keyboard/blob/{tag}/docs/releases/{tag}.md)'], check=True)
    # Manifest-derived exact list, no glob, --clobber, rebuild, or unrelated run download.
    subprocess.run(['gh', 'release', 'upload', tag, *(str(directory/n) for n in names)], check=True)
    with tempfile.TemporaryDirectory() as tmp:
        destination = Path(tmp)
        subprocess.run(['gh', 'release', 'download', tag, '--dir', tmp], check=True)
        if {p.name for p in destination.iterdir()} != set(names):
            raise ValueError('Remote release asset set differs from verified set; leaving draft')
        for name in names:
            if release.digest(destination/name) != release.digest(directory/name):
                raise ValueError('Remote asset hash differs; leaving draft: ' + name)
        release.verify(destination, sha, contract)
    subprocess.run(['gh', 'release', 'edit', tag, '--draft=false'], check=True)
    print('Published exact verified release:', tag, sha)


def broken_readme_links(text, tag, open_url=urllib.request.urlopen):
    """README download links for this release that do not answer (card #200). Run after the release is public:
    a draft's files are not downloadable, so earlier every link would look broken."""
    links = sorted(set(re.findall(r'https://github\.com/itsdestin/swipe-keyboard/releases/download/' + re.escape(tag) + r'/[^\s)"\']+', text)))
    if not links:
        return ['README has no download link for ' + tag]
    broken = []
    for link in links:
        try:
            with open_url(urllib.request.Request(link, method='HEAD'), timeout=60) as answer:
                if answer.status != 200:
                    broken.append(f'{link} ({answer.status})')
        except Exception as error:  # noqa: BLE001 - any failure to fetch is a broken link
            broken.append(f'{link} ({error})')
    return broken


if __name__ == '__main__':
    publish()
    # WHY after publishing: the release stands either way; a red run here means the README sends people to a
    # missing file and must be fixed at once.
    broken = broken_readme_links(Path('README.md').read_text(), 'v' + release.version(release.load_contract()))
    if broken:
        raise SystemExit('README download links that do not work:\n  ' + '\n  '.join(broken))
    print('Every README download link for this release works')
