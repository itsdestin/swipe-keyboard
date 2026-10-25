#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Build the signed update website from one verified release (plan: docs/active/plans/2026-10-23-auto-updates.md).

    update_site.py build --release DIR --tag TAG --key-id FPR --out SITE [--base-url URL] [--downloads URL] [--test-key]
    update_site.py verify --site SITE --keyring KEY.gpg --armored KEY.asc

DIR holds the verified release files (the same bytes the release published). The site holds only the newest release:
  repo/                       Flatpak repository with both Deck apps, every commit and the summary signed
  fedora/<arch>/repodata/     dnf metadata, repomd.xml signed; the packages themselves stay on the release page
  arch/latest.json(.sig)      what the Arch updater reads: version, URL and SHA256 of the package
  swipe-keyboard.gpg/.asc     the public update key, plus .flatpakrepo/.flatpakref files carrying it
Signing uses the key already imported into gpg (CI imports it from the secret; the rehearsal uses a throwaway key).
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE_URL = 'https://itsdestin.github.io/swipe-keyboard'
DOWNLOADS = 'https://github.com/itsdestin/swipe-keyboard/releases/download'
MAIN_ID = 'org.itsdestin.SwipeKeyboard'
REDIRECT_FILE = ROOT / 'packaging/updates/flatpak-redirect.txt'
HELPER_ID = 'org.itsdestin.SwipeKeyboard.GamingHelper'


def run(*command, **kwargs):
    subprocess.run(command, check=True, **kwargs)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def contract():
    return json.loads((ROOT / 'packaging/release-targets.json').read_text())


def names(version_release):
    version = version_release.rsplit('-', 1)[0]
    return {
        'flatpak': f'swipe-keyboard-{version_release}-x86_64.flatpak',
        'steamos': f'swipe-keyboard-{version_release}-steamos-x86_64.tar.gz',
        'helper': f'swipe-keyboard-gaming-helper-{version_release}-x86_64.flatpak',
        'arch': f'swipe-keyboard-{version_release}-x86_64.pkg.tar.zst',
        'rpms': {'x86_64': f'swipe-keyboard-{version_release}.fc44.x86_64.rpm',
                 'aarch64': f'swipe-keyboard-{version_release}.fc44.aarch64.rpm'},
        'version': version,
    }


def sign(path, key_id, armor=False):
    out = Path(str(path) + ('.asc' if armor else '.sig'))
    run('gpg', '--batch', '--yes', '--local-user', key_id, '--detach-sign', *(['--armor'] if armor else []),
        '--output', str(out), str(path))
    return out


def arch_index(release, tag, version_release, downloads):
    package = release / names(version_release)['arch']
    return {
        'schema': 1, 'package': 'swipe-keyboard', 'version': version_release,
        'files': {'x86_64': {'url': f'{downloads}/{tag}/{package.name}', 'sha256': sha256(package),
                             'size': package.stat().st_size}},
    }


def build_arch(release, site, tag, version_release, key_id, downloads):
    folder = site / 'arch'
    folder.mkdir(parents=True)
    index = folder / 'latest.json'
    index.write_text(json.dumps(arch_index(release, tag, version_release, downloads), indent=1) + '\n')
    sign(index, key_id)


def build_fedora(release, site, tag, version_release, key_id, downloads):
    for arch, name in names(version_release)['rpms'].items():
        folder = site / 'fedora' / arch
        folder.mkdir(parents=True)
        # WHY metadata only: the package stays on the release page (the exact published bytes); the index points there.
        shutil.copy2(release / name, folder / name)
        run('createrepo_c', '--quiet', '--baseurl', f'{downloads}/{tag}/', str(folder))
        (folder / name).unlink()
        sign(folder / 'repodata/repomd.xml', key_id, armor=True)


def flatpak_redirect(path=REDIRECT_FILE):
    """The address every Deck should move to, or '' (see packaging/updates/flatpak-redirect.txt)."""
    lines = [line.strip() for line in path.read_text().splitlines() if line.strip() and not line.lstrip().startswith('#')]
    if len(lines) > 1 or (lines and not lines[0].startswith('https://')):
        raise SystemExit(f'{path} must hold at most one https:// address')
    return lines[0] if lines else ''


def copy_without_redirect(repo, out, key_id=None):
    """A copy of a Flatpak repository whose summary carries no "moved" note (same commits, rebuilt summary).

    WHY the config line too: build-update-repo --redirect-url also saves the address in the repository's own config
    ([flatpak] redirect-url), so rebuilding the summary alone brings the note back (found by CI, 2026-10-25).
    """
    out = Path(out)
    shutil.copytree(repo, out, symlinks=True)
    subprocess.run(['ostree', 'config', f'--repo={out}', 'unset', 'flatpak.redirect-url'], capture_output=True)
    run('flatpak', 'build-update-repo', *([f'--gpg-sign={key_id}'] if key_id else []), '--title=Swipe Keyboard', str(out))
    return out


def build_flatpak(release, site, version_release, key_id):
    n = names(version_release)
    repo = site / 'repo'
    run('ostree', 'init', '--mode=archive-z2', f'--repo={repo}')
    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(release / n['steamos']) as archive:
            member = next(m for m in archive.getmembers() if m.name.endswith('/' + n['helper']))
            member.name = n['helper']
            archive.extract(member, tmp, filter='data')
        for bundle in (release / n['flatpak'], Path(tmp) / n['helper']):
            run('flatpak', 'build-import-bundle', f'--gpg-sign={key_id}', str(repo), str(bundle))
    # WHY no static deltas: a recent libostree refuses large delta parts, and a whole update is only about 30 MB.
    # WHY the redirect is part of the signed summary: a Deck only follows a "moved" note signed with our key.
    redirect = flatpak_redirect()
    run('flatpak', 'build-update-repo', f'--gpg-sign={key_id}', '--title=Swipe Keyboard',
        *([f'--redirect-url={redirect}'] if redirect else []), str(repo))


def write_key_files(site, key_id, base_url, test_key=False):
    binary = subprocess.run(['gpg', '--batch', '--export', key_id], check=True, capture_output=True).stdout
    if not binary:
        raise SystemExit('the signing key is not in gpg')
    (site / 'swipe-keyboard.gpg').write_bytes(binary)
    (site / 'swipe-keyboard.asc').write_bytes(
        subprocess.run(['gpg', '--batch', '--armor', '--export', key_id], check=True, capture_output=True).stdout)
    committed = (ROOT / 'packaging/updates/update-key.gpg').read_bytes()
    # WHY refuse: every installed keyboard trusts only the committed key; signing with another would strand them all.
    if binary != committed and not test_key:
        raise SystemExit('the signing key is not the committed packaging/updates/update-key.gpg')
    key = base64.b64encode(binary).decode()
    for app_id, title in ((MAIN_ID, 'Swipe Keyboard'), (HELPER_ID, 'Swipe Keyboard Gaming Mode helper')):
        (site / f'{app_id}.flatpakref').write_text(
            f'[Flatpak Ref]\nTitle={title}\nName={app_id}\nBranch=master\nIsRuntime=false\nUrl={base_url}/repo/\n'
            f'SuggestRemoteName=swipe-keyboard\nGPGKey={key}\n')
    (site / 'swipe-keyboard.flatpakrepo').write_text(
        f'[Flatpak Repo]\nTitle=Swipe Keyboard\nUrl={base_url}/repo/\nGPGKey={key}\n')
    (site / '.nojekyll').write_text('')


def build(release, tag, key_id, out, base_url=BASE_URL, downloads=DOWNLOADS, test_key=False):
    release, site = Path(release), Path(out)
    c = contract()
    version_release = f"{c['version']}-{c['release']}"
    if tag != f'v{version_release}':
        raise SystemExit(f'tag {tag} does not match the release contract {version_release}')
    if site.exists():
        raise SystemExit(f'{site} already exists')
    site.mkdir(parents=True)
    write_key_files(site, key_id, base_url, test_key)
    build_arch(release, site, tag, version_release, key_id, downloads)
    build_fedora(release, site, tag, version_release, key_id, downloads)
    build_flatpak(release, site, version_release, key_id)
    (site / 'index.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Swipe Keyboard updates</title>'
        f'<p>Update site for Swipe Keyboard {version_release}. Installed keyboards check here once a day. '
        '<a href="https://github.com/itsdestin/swipe-keyboard">Project page</a></p>\n')
    return site


def bundle_origin_accepts(site, bundle, keyring):
    """Install a bundle the way a Deck does, then point its origin at the site exactly as install.sh does.

    Returns True if Flatpak then accepts an update check against the site. WHY this and not a fresh remote-add: a
    bundle's origin remote starts with signature checks off, so only this path shows whether checks are really on.
    """
    keyring = Path(keyring).resolve()
    with tempfile.TemporaryDirectory() as home:
        env = {'HOME': home, 'PATH': '/usr/bin:/bin', 'LANG': 'C'}
        flatpak = lambda *a, **k: subprocess.run(['flatpak', *a], env=env, capture_output=True, text=True, **k)
        # WHY --no-deps: the test needs the app's origin remote, not its 1 GB runtime.
        flatpak('install', '--user', '--noninteractive', '-y', '--no-deps', '--bundle', str(bundle), check=True)
        origin = flatpak('info', '--user', '--show-origin', MAIN_ID, check=True).stdout.strip()
        # The same command as gaming/setup/install.sh point_at_update_site.
        flatpak('remote-modify', '--user', f'--url=file://{site}/repo', '--gpg-verify', f'--gpg-import={keyring}', origin,
                check=True)
        config = (Path(home) / '.local/share/flatpak/repo/config').read_text()
        section = config[config.index(f'[remote "{origin}"]'):]
        section = section[:section.find('\n[', 1)] if '\n[' in section[1:] else section
        if 'gpg-verify=true' not in section:
            raise SystemExit('the modified origin remote does not check signatures:\n' + section)
        # WHY not the update's exit code: Flatpak treats a refused remote as "non-fatal, nothing to update" and exits 0
        # (seen 2026-10-23). Listing the origin fails outright when the summary signature does not match.
        if flatpak('remote-ls', '--user', '--columns=application', origin).returncode != 0:
            return False
        before = flatpak('info', '--user', '--show-commit', MAIN_ID, check=True).stdout.strip()
        flatpak('update', '--user', '--noninteractive', '-y', '--no-deps', '--no-related', MAIN_ID)
        after = flatpak('info', '--user', '--show-commit', MAIN_ID, check=True).stdout.strip()
        wanted = subprocess.run(['ostree', f'--repo={site}/repo', 'rev-parse', f'app/{MAIN_ID}/x86_64/master'],
                                check=True, capture_output=True, text=True).stdout.strip()
        # When the site holds a newer build, the update must really have moved the app onto it.
        return after == wanted or (before == wanted and after == before)


def newer_copy(site, key_id):
    """A copy of the site's Flatpak repository whose app commit is re-stamped now and re-signed: a real update."""
    newer = Path(tempfile.mkdtemp(prefix='swipekb-newer-'))
    shutil.copytree(Path(site) / 'repo', newer / 'repo', symlinks=True)
    ref = f'app/{MAIN_ID}/x86_64/master'
    run('flatpak', 'build-commit-from', '--force', '--timestamp=NOW', f'--gpg-sign={key_id}', f'--src-ref={ref}',
        str(newer / 'repo'), ref)
    run('flatpak', 'build-update-repo', f'--gpg-sign={key_id}', '--title=Swipe Keyboard', str(newer / 'repo'))
    return newer


def verify(site, keyring, armored, bundle=None, key_id=None):
    """Each platform's own tool must accept the site with the signature checked (a wrong site strands every keyboard)."""
    site = Path(site).resolve()
    # WHY absolute: gpgv reads a bare keyring name from inside ~/.gnupg, not the current folder (seen in CI).
    keyring = Path(keyring).resolve()
    version_release = f"{contract()['version']}-{contract()['release']}"
    run('gpgv', '--keyring', str(keyring), str(site / 'arch/latest.json.sig'), str(site / 'arch/latest.json'))
    if json.loads((site / 'arch/latest.json').read_text())['version'] != version_release:
        raise SystemExit('arch/latest.json names the wrong version')
    for arch in ('x86_64', 'aarch64'):
        repodata = site / 'fedora' / arch / 'repodata'
        run('gpgv', '--keyring', str(keyring), str(repodata / 'repomd.xml.asc'), str(repodata / 'repomd.xml'))
    # The Deck's "moved" note (packaging/updates/flatpak-redirect.txt): exactly the one written there, or none at all.
    redirect = flatpak_redirect()
    summary = subprocess.run(['ostree', 'summary', '--view', f'--repo={site}/repo'], check=True, capture_output=True,
                             text=True).stdout
    if redirect and f"xa.redirect-url: '{redirect}'" not in summary:
        raise SystemExit(f'the Flatpak summary does not carry the moved note to {redirect}')
    if not redirect and 'xa.redirect-url' in summary:
        raise SystemExit('the Flatpak summary carries a moved note nobody turned on')
    flatpak_site = site
    if redirect:
        # WHY prove it with Flatpak itself: a Deck that adds this site must end up at the new address (the note is part
        # of the signed summary, so a Deck only takes it from a summary signed with our key).
        with tempfile.TemporaryDirectory() as home:
            env = {'HOME': home, 'PATH': '/usr/bin:/bin'}
            run('flatpak', 'remote-add', '--user', f'--gpg-import={keyring}', 'verify-repo', f'file://{site}/repo', env=env)
            url = subprocess.run(['flatpak', 'remotes', '--user', '--columns=url'], check=True, capture_output=True,
                                 text=True, env=env).stdout.strip()
        if url.rstrip('/') != redirect.rstrip('/'):
            raise SystemExit(f'a Deck adding the site ends up at {url}, not {redirect}')
        # WHY a copy without the note for the checks below: they install and update from this site, and the new address
        # only exists after the rename. The copy holds the same signed commits; only its summary is rebuilt.
        if not key_id:
            raise SystemExit('with the moved note on, verify needs --key-id to check the apps on a copy without it')
        flatpak_site = Path(tempfile.mkdtemp(prefix='swipekb-noredirect-'))
        copy_without_redirect(site / 'repo', flatpak_site / 'repo', key_id)
    with tempfile.TemporaryDirectory() as home:
        env = {'HOME': home, 'PATH': '/usr/bin:/bin'}
        run('flatpak', 'remote-add', '--user', f'--gpg-import={keyring}', 'verify-repo', f'file://{flatpak_site}/repo', env=env)
        listed = subprocess.run(['flatpak', 'remote-ls', '--user', '--columns=application', 'verify-repo'],
                                check=True, capture_output=True, text=True, env=env).stdout.split()
    if sorted(listed) != sorted([MAIN_ID, HELPER_ID]):
        raise SystemExit(f'the Flatpak repository lists {listed}')
    if bundle:
        if not bundle_origin_accepts(flatpak_site, bundle, keyring):
            raise SystemExit('a Deck installed from the bundle cannot update from the site with the right key')
        if key_id:
            # WHY a newer copy: the site holds the same build as the bundle, so "nothing to update" would pass the check
            # above. A re-stamped, re-signed commit makes a real update: it must install with our key (below, wrong key).
            newer = newer_copy(flatpak_site, key_id)
            if not bundle_origin_accepts(newer, bundle, keyring):
                raise SystemExit('a Deck did not install a newer, correctly signed update')
        with tempfile.TemporaryDirectory() as gnupg:
            run('gpg', '--batch', '--quiet', '--homedir', gnupg, '--passphrase', '', '--quick-generate-key',
                'Wrong key (verify only)', 'ed25519', 'sign', '1d')
            wrong = Path(gnupg) / 'wrong.gpg'
            wrong.write_bytes(subprocess.run(['gpg', '--batch', '--homedir', gnupg, '--export'], check=True,
                                             capture_output=True).stdout)
            for candidate in ([flatpak_site] + ([newer] if key_id else [])):
                if bundle_origin_accepts(candidate, bundle, wrong):
                    raise SystemExit('a Deck that trusts a different key still accepted the site: signatures are not checked')
        if key_id:
            shutil.rmtree(newer, ignore_errors=True)
    if flatpak_site != site:
        shutil.rmtree(flatpak_site, ignore_errors=True)
    # WHY a real Fedora: dnf itself must accept the signed index (repo_gpgcheck) and see this version.
    repo = (ROOT / 'packaging/updates/swipe-keyboard.repo').read_text()
    repo = '\n'.join('baseurl=file:///site/fedora/$basearch/' if l.startswith('baseurl=') else
                     'gpgkey=file:///key.asc' if l.startswith('gpgkey=') else l for l in repo.splitlines()) + '\n'
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / 'swipe-keyboard.repo').write_text(repo)
        script = ('cp /cfg/swipe-keyboard.repo /etc/yum.repos.d/ && dnf -y -q --repo=swipe-keyboard makecache && '
                  'dnf -q --repo=swipe-keyboard repoquery swipe-keyboard')
        out = subprocess.run(['docker', 'run', '--rm', '-v', f'{site}:/site:ro', '-v', f'{Path(armored).resolve()}:/key.asc:ro',
                              '-v', f'{tmp}:/cfg:ro', 'fedora:44', 'bash', '-euc', script],
                             check=True, capture_output=True, text=True).stdout
    if version_release not in out:
        raise SystemExit('dnf does not see ' + version_release + ' in the site: ' + out)
    print('Update site verified:', site)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build')
    b.add_argument('--release', required=True)
    b.add_argument('--tag', required=True)
    b.add_argument('--key-id', required=True)
    b.add_argument('--out', required=True)
    b.add_argument('--base-url', default=BASE_URL)
    b.add_argument('--downloads', default=DOWNLOADS)
    # WHY a flag: only CI rehearsals may sign with a throwaway key; their site is never published.
    b.add_argument('--test-key', action='store_true')
    v = sub.add_parser('verify')
    v.add_argument('--site', required=True)
    v.add_argument('--keyring', required=True)
    v.add_argument('--armored', required=True)
    v.add_argument('--bundle', help='the release .flatpak: also prove a bundle install updates only with the right key')
    v.add_argument('--key-id', help='signing key: with --bundle, also prove a real newer update installs')
    args = parser.parse_args()
    if args.command == 'verify':
        verify(args.site, args.keyring, args.armored, args.bundle, args.key_id)
    else:
        print(build(args.release, args.tag, args.key_id, args.out, args.base_url.rstrip('/'),
                    args.downloads.rstrip('/'), args.test_key))


if __name__ == '__main__':
    main()
