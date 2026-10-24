#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Small same-run release boundary. All paths and targets come from the committed contract."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PRODUCERS = ('prepare', 'arch', 'fedora', 'fedora-arm64', 'flatpak', 'steamos')
# WHY: drafts and plain branch dispatch run one native system (Arch: dual old/new-library
# verification and the whole native test suite). Anything that can merge or ship runs all.
FAST_PRODUCERS = ('prepare', 'arch')
FAST_TARGETS = ('source', 'arch')


def is_doc(path):
    """Words only: Markdown anywhere, or anything under docs/ (no build, package or test reads docs/ code).

    WHY not LICENSES/: those texts are installed into every package (cmake/Install.cmake), so changing one
    changes the shipped bytes and must build.
    """
    return path.endswith('.md') or path.startswith('docs/')


def docs_only(paths):
    paths = [p for p in paths if p]
    return bool(paths) and all(is_doc(p) for p in paths)


def run_mode(env, changed=None):
    """'docs' for a pull request that changes only documentation (card #200), 'fast' only for a draft pull
    request or a branch dispatch that did not ask for full.

    Doubt always means 'full': tags, main, ready pull requests and unknown events. An unknown list of changed
    files (None) is never 'docs'.
    """
    ref = env.get('GITHUB_REF', '')
    event = env.get('GITHUB_EVENT_NAME', '')
    if ref.startswith('refs/tags/') or ref == 'refs/heads/main':
        return 'full'
    if event == 'pull_request':
        # WHY pull requests only: main's run is what a release tag reuses, so main always builds.
        if changed is not None and docs_only(changed):
            return 'docs'
        return 'fast' if env.get('PR_DRAFT') == 'true' else 'full'
    if event == 'workflow_dispatch':
        return 'full' if env.get('FULL_INPUT') == 'true' else 'fast'
    return 'full'


def parse_json(text):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate JSON key: ' + key)
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=unique)


def load_contract():
    return parse_json((ROOT/'packaging/release-targets.json').read_text())


def version(contract):
    return f"{contract['version']}-{contract['release']}"


def filename(template, contract):
    return template.format(version=contract['version'], release=contract['release'])


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def expected_files(contract):
    return {filename(name, contract) for target in contract['targets'].values() for name in target['files']}


def check_files(directory, records, expected):
    names = [r.get('name') for r in records]
    if len(names) != len(set(names)) or set(names) != expected:
        raise ValueError('Missing, duplicate or unexpected artifact metadata')
    for record in records:
        path = directory/record['name']
        if path.name != record['name'] or path.is_symlink() or not path.is_file():
            raise ValueError('Missing/unsafe artifact: ' + record['name'])
        if not path.stat().st_size or path.stat().st_size != record.get('size') or digest(path) != record.get('sha256'):
            raise ValueError('Empty or tampered artifact: ' + record['name'])


def check_records(records, expected):
    """Evidence shape only (no bytes): exact names, non-empty size, well-formed digest."""
    names = [r.get('name') for r in records]
    if len(names) != len(set(names)) or set(names) != expected:
        raise ValueError('Missing, duplicate or unexpected artifact metadata')
    for record in records:
        size, sha = record.get('size'), record.get('sha256')
        if not isinstance(size, int) or size <= 0 or not isinstance(sha, str) or not re.fullmatch('[0-9a-f]{64}', sha):
            raise ValueError('Malformed artifact metadata: ' + record['name'])


def validate(directory, evidence, statuses, sha, contract, mode='full', check_bytes=True):
    if not re.fullmatch('[0-9a-f]{40}', sha):
        raise ValueError('Invalid source SHA')
    required = PRODUCERS if mode == 'full' else FAST_PRODUCERS
    # WHY exact: every producer must be present; required ones succeeded, optional ones were skipped
    # (never failed, cancelled or quietly run in a different mode).
    if mode not in ('full', 'fast') or set(statuses) != set(PRODUCERS) or any(
            statuses[job].get('result') != ('success' if job in required else 'skipped') for job in PRODUCERS):
        raise ValueError('Every required producer must succeed (including prepare)')
    wanted = set(contract['targets']) if mode == 'full' else set(FAST_TARGETS)
    targets = [e.get('target') for e in evidence]
    if len(targets) != len(set(targets)) or set(targets) != wanted:
        raise ValueError('Missing, duplicate or unexpected target evidence')
    records = []
    for entry in evidence:
        target = contract['targets'][entry['target']]
        if entry.get('version') != version(contract) or entry.get('source_sha') != sha or entry.get('arch') != target['arch']:
            raise ValueError('Target version/source SHA/architecture mismatch')
        files = entry.get('files', [])
        names = {filename(n, contract) for n in target['files']}
        (check_files(directory, files, names) if check_bytes else check_records(files, names))
        records.extend(files)
    expected = {filename(n, contract) for t in wanted for n in contract['targets'][t]['files']}
    (check_files(directory, records, expected) if check_bytes else check_records(records, expected))
    if mode == 'full':
        # WHY: the SteamOS download carries its own copy of the main Flatpak. It must be the very file released
        # on its own, or Deck users and Desktop-only users would run different builds under one version number.
        # (Fast runs build neither, so there is nothing to compare.)
        flatpak_main = next(r for e in evidence if e['target'] == 'flatpak' for r in e['files']
                            if r['name'] == filename(contract['targets']['flatpak']['files'][0], contract))
        steamos = next(e for e in evidence if e['target'] == 'steamos')
        if steamos.get('embedded_main_sha256') != flatpak_main['sha256']:
            raise ValueError('SteamOS download does not embed the released Flatpak bundle')
    # WHY exact directory: an uploader glob must never smuggle additional unchecked bytes.
    if check_bytes and mode == 'full':
        allowed = expected_files(contract) | {'release-manifest.json', 'SHA256SUMS'}
        if {p.name for p in directory.iterdir()} - allowed:
            raise ValueError('Unexpected publication input')
    return {'version': version(contract), 'source_sha': sha, 'evidence': evidence,
            'producers': statuses, 'files': sorted(records, key=lambda r: r['name'])}


def input_names(contract):
    return {filename(contract['targets'][target]['files'][index], contract)
            for target, index in [('source', 0), ('flatpak', 1)]}


def verify_inputs(directory, sha, contract):
    manifest = parse_json((directory/'input-manifest.json').read_text())
    if manifest.get('source_sha') != sha or manifest.get('version') != version(contract):
        raise ValueError('Prepared input source/version mismatch')
    names = input_names(contract)
    if {p.name for p in directory.iterdir()} != names | {'input-manifest.json'}:
        raise ValueError('Prepared input file set mismatch')
    check_files(directory, manifest.get('files', []), names)


def checksums(manifest):
    return ''.join(f"{r['sha256']}  {r['name']}\n" for r in manifest['files'])


def verify(directory, sha, contract):
    manifest_path = directory/'release-manifest.json'
    checksum_file = directory/'SHA256SUMS'
    if any(p.is_symlink() or not p.is_file() for p in (manifest_path, checksum_file)):
        raise ValueError('Required manifest/checksums missing or unsafe')
    manifest = parse_json(manifest_path.read_text())
    checked = validate(directory, manifest['evidence'], manifest['producers'], sha, contract)
    if checked != manifest:
        raise ValueError('Manifest differs from checked target evidence')
    if checksum_file.read_text() != checksums(checked):
        raise ValueError('Checksums differ from manifest')
    return checked


def check_versions(contract):
    # WHY one authoritative list, with checks rather than hidden recipe rewriting.
    for path, pattern, expected in [
        ('packaging/arch/PKGBUILD', r'^pkgver=(.+)$', contract['version']),
        ('packaging/arch/PKGBUILD', r'^pkgrel=(.+)$', str(contract['release'])),
        ('packaging/fedora/swipe-keyboard.spec', r'^Version: (.+)$', contract['version']),
        ('packaging/fedora/swipe-keyboard.spec', r'^Release: (\d+)', str(contract['release'])),
    ]:
        match = re.search(pattern, (ROOT/path).read_text(), re.M)
        if not match or match[1] != expected:
            raise ValueError('Version metadata drift: ' + path)


README = ROOT/'README.md'
DOWNLOAD = re.compile(r'https://github\.com/itsdestin/swipe-keyboard/releases/download/v([0-9][0-9.]*-[0-9][0-9.]*)/')


def readme_versions(text):
    """Every release version the README's download links point at."""
    return set(DOWNLOAD.findall(text))


def update_readme(text, contract):
    """Point every download link, file name and command at this release (card #200).

    WHY a plain replacement of the one old version string: the README names it in links, file names, tar/cd/pacman/dnf
    commands, the release header and the release-notes link, and all of them must move together. Refuses when the
    links name more than one version, because then which one is "old" is a guess.
    """
    old = readme_versions(text)
    if len(old) != 1:
        raise ValueError('README download links name ' + (', '.join(sorted(old)) or 'no version') + '; fix by hand')
    (previous,) = old
    return text.replace(previous, version(contract)), previous


def steamos_bundle():
    # WHY loaded by path: this file is itself loaded by path in tests, so a plain import of a sibling would fail.
    import importlib.util
    spec = importlib.util.spec_from_file_location('steamos_bundle', ROOT/'scripts/steamos_bundle.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def output(command):
    return subprocess.check_output(command, text=True).strip()


def inspect(target, directory, stage, sha, contract):
    """Producer-side package-manager metadata AND extracted embedded identity, before hashing."""
    names = [filename(n, contract) for n in contract['targets'][target]['files']]
    architecture = contract['targets'][target]['arch']
    rpm_target = target in ('fedora', 'fedora-arm64')
    if target in ('arch', 'flatpak') or rpm_target:
        if stage is None:
            raise ValueError('Extracted/installed package root required')
        prefix = 'files' if target == 'flatpak' else 'usr'
        runtime = parse_json((stage/prefix/'share/swipe-keyboard/runtime.json').read_text())
        identity = parse_json((stage/prefix/'share/swipe-keyboard/source-identity.json').read_text())
        embedded = version(contract) + ('.fc44' if rpm_target else '')
        if runtime.get('package') != embedded or identity != {'source_sha': sha, 'version': version(contract)}:
            raise ValueError('Embedded package version/source mismatch')
    if target == 'arch':
        metadata = output(['bsdtar', '-xOf', str(directory/names[0]), '.PKGINFO'])
        for key, value in [('pkgname', 'swipe-keyboard'), ('pkgver', version(contract)), ('arch', 'x86_64')]:
            if re.findall(r'^' + key + r' = (.+)$', metadata, re.M) != [value]:
                raise ValueError('Arch package metadata mismatch: ' + key)
    elif rpm_target:
        # WHY one common SRPM: ARM publishes only its binary; publishing both
        # same-named build-host-dependent SRPMs would overwrite merged artifacts.
        # SOURCEPACKAGE checks source headers whose ARCH may be the build host.
        for name in names:
            field, expected = ('SOURCEPACKAGE', '1') if name.endswith('.src.rpm') else ('ARCH', architecture)
            actual = output(['rpm', '-qp', '--qf', '%{NAME} %{VERSION}-%{RELEASE} %{' + field + '}', str(directory/name)])
            if actual != f"swipe-keyboard {version(contract)}.fc44 {expected}":
                raise ValueError('RPM package metadata mismatch')
    elif target == 'flatpak':
        # Bundle is installed before this call, with an exact ref/arch check in CI.
        metadata = (stage/'metadata').read_text()
        if 'runtime=org.kde.Platform/x86_64/6.10' not in metadata or 'name=org.itsdestin.SwipeKeyboard' not in metadata:
            raise ValueError('Flatpak metadata mismatch')
    elif target == 'steamos':
        # WHY check the real archive, not just its hash: it is what Deck users run install.sh from.
        embedded = steamos_bundle().verify(directory/names[0], version(contract))
    elif target == 'source':
        with tarfile.open(directory/names[0]) as archive:
            member = archive.extractfile(f"swipe-keyboard-{contract['version']}/packaging/source-identity.json")
            if member is None or parse_json(member.read()) != {'source_sha': sha, 'version': version(contract)}:
                raise ValueError('Source archive identity mismatch')
    records = [{'name': n, 'size': (directory/n).stat().st_size, 'sha256': digest(directory/n)} for n in names]
    check_files(directory, records, set(names))
    entry = {'target': target, 'version': version(contract), 'source_sha': sha,
             'arch': contract['targets'][target]['arch'], 'files': records}
    if target == 'steamos':
        entry['embedded_main_sha256'] = embedded['main_sha256']
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('version', 'check-versions', 'record', 'gate', 'verify', 'verify-inputs', 'mode', 'readme'))
    parser.add_argument('--directory', type=Path, default=Path('release'))
    parser.add_argument('--evidence', type=Path, default=Path('evidence'))
    parser.add_argument('--target')
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--sha')
    parser.add_argument('--evidence-only', action='store_true',
                        help='Non-release runs: trust each producer\'s recorded hashes; never allowed on tags')
    parser.add_argument('--statuses', help='GitHub needs JSON, supplied by the workflow, not an artifact')
    parser.add_argument('--changed', type=Path, help='mode: file listing the paths a pull request changes, one per line')
    args = parser.parse_args()
    contract = load_contract()
    if args.command == 'version':
        print(version(contract))
    elif args.command == 'mode':
        import os
        changed = args.changed.read_text().splitlines() if args.changed and args.changed.is_file() else None
        print(run_mode(os.environ, changed))
    elif args.command == 'readme':
        text, previous = update_readme(README.read_text(), contract)
        README.write_text(text)
        print(f'README now points at {version(contract)} (was {previous}); review the diff before committing')
    elif args.command == 'check-versions':
        check_versions(contract)
        print('Release recipe versions agree: ' + version(contract))
    elif args.command == 'verify-inputs':
        verify_inputs(args.directory, args.sha, contract)
        print('Prepared input hashes and identity verified: ' + args.sha)
    elif args.command == 'record':
        entry = inspect(args.target, args.directory, args.stage, args.sha, contract)
        args.evidence.mkdir(parents=True, exist_ok=True)
        (args.evidence/(args.target + '.json')).write_text(json.dumps(entry, indent=2) + '\n')
    elif args.command == 'gate':
        import os
        mode = run_mode(os.environ)
        # WHY trusted only for pull requests: prepare decided 'docs' from the changed files; the gate cannot see
        # them, and a tag or main run is always full whatever prepare said.
        ref = os.environ.get('GITHUB_REF', '')
        if (os.environ.get('PREPARED_MODE') == 'docs' and os.environ.get('GITHUB_EVENT_NAME') == 'pull_request'
                and not ref.startswith('refs/tags/') and ref != 'refs/heads/main'):
            mode = 'docs'
        if args.evidence_only and os.environ.get('GITHUB_REF', 'refs/tags/unknown').startswith('refs/tags/'):
            # WHY: a tag (or an unknown ref) publishes bytes, so the gate must hash the real bytes.
            raise ValueError('Evidence-only gate is refused for tag or unknown refs; tags verify actual bytes')
        if mode == 'fast' and os.environ.get('GITHUB_REF', '').startswith('refs/tags/'):
            raise ValueError('A tag run cannot be fast')
        if mode == 'docs':
            # WHY no evidence: a documentation-only pull request builds no package. prepare (which ran the
            # documentation checks) must have succeeded and every package job must have been skipped.
            statuses = parse_json(args.statuses)
            if set(statuses) != set(PRODUCERS) or statuses['prepare'].get('result') != 'success' or any(
                    statuses[job].get('result') != 'skipped' for job in PRODUCERS if job != 'prepare'):
                raise ValueError('Documentation-only run: prepare must succeed and every package job be skipped')
            print('Verified documentation-only pull request (no package built): ' + args.sha)
            return
        targets = list(contract['targets']) if mode == 'full' else list(FAST_TARGETS)
        # Explicit target evidence names; do not infer support from what happened to download.
        if {p.name for p in args.evidence.iterdir()} != {t + '.json' for t in targets}:
            raise ValueError('Exact required evidence set missing/duplicated/unexpected')
        evidence = [parse_json((args.evidence/(t + '.json')).read_text()) for t in targets]
        manifest = validate(args.directory, evidence, parse_json(args.statuses), args.sha, contract,
                            mode=mode, check_bytes=not args.evidence_only)
        if mode == 'fast':
            print('Verified fast-path run (one system only, NOT releasable or mergeable): ' + manifest['version'] + ' ' + args.sha)
        elif args.evidence_only:
            print('Verified complete non-release evidence: ' + manifest['version'] + ' ' + args.sha)
        else:
            (args.directory/'release-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            (args.directory/'SHA256SUMS').write_text(checksums(manifest))
            verify(args.directory, args.sha, contract)
            print('Verified complete release: ' + manifest['version'] + ' ' + args.sha)
    else:
        verify(args.directory, args.sha, contract)
        print('Publication bytes verified: ' + args.sha)


if __name__ == '__main__':
    main()
