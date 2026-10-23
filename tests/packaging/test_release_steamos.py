"""The single SteamOS download (plan Section 15 question 2): both Flatpaks, install.sh, uninstall.sh, units, checksums.

Covers scripts/steamos_bundle.py and how the release gate treats the new `steamos` target.
"""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


release = load('release', 'scripts/release.py')
bundle = load('steamos_bundle', 'scripts/steamos_bundle.py')
VERSION = '0.4.0-1'


class SteamosBundleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.main = self.dir / f'swipe-keyboard-{VERSION}-x86_64.flatpak'
        self.helper = self.dir / 'helper-built-elsewhere.flatpak'
        self.main.write_bytes(b'main flatpak bytes')
        self.helper.write_bytes(b'helper flatpak bytes')
        self.out = self.dir / 'out'
        self.out.mkdir()
        self.archive = bundle.assemble(self.main, self.helper, VERSION, self.out)

    def rewrite(self, change):
        """Rebuild the archive after `change(members: dict name -> (mode, bytes))`, to craft broken downloads."""
        with tarfile.open(self.archive) as a:
            members = {m.name: (m.mode, a.extractfile(m).read()) for m in a.getmembers() if m.isfile()}
        change(members)
        with tarfile.open(self.archive, 'w:gz') as a:
            for name, (mode, data) in sorted(members.items()):
                info = tarfile.TarInfo(name)
                info.size, info.mode = len(data), mode
                a.addfile(info, io.BytesIO(data))

    def top(self, name):
        return f'swipe-keyboard-steamos-{VERSION}/{name}'

    def test_name_follows_the_release_contract(self):
        contract = release.load_contract()
        self.assertEqual(self.archive.name, release.filename(contract['targets']['steamos']['files'][0], contract))

    def test_contains_exactly_the_expected_files_with_correct_modes(self):
        with tarfile.open(self.archive) as a:
            names = {m.name: m for m in a.getmembers()}
        top = f'swipe-keyboard-steamos-{VERSION}'
        self.assertEqual(set(names), {
            top, top + '/install.sh', top + '/uninstall.sh', top + '/VERSION', top + '/SHA256SUMS',
            top + '/switch-watch.sh', top + '/units/swipekb-engine.service', top + '/units/swipekb-helper.service',
            top + '/units/swipekb-switch-watch.service', top + '/units/swipekb-switch-watch.timer', top + '/units',
            top + f'/swipe-keyboard-{VERSION}-x86_64.flatpak',
            top + f'/swipe-keyboard-gaming-helper-{VERSION}-x86_64.flatpak'})
        self.assertTrue(names[top + '/install.sh'].mode & 0o111)
        self.assertTrue(names[top + '/uninstall.sh'].mode & 0o111)

    def test_embedded_main_bundle_is_the_given_file_byte_for_byte(self):
        result = bundle.verify(self.archive, VERSION)
        self.assertEqual(result['main_sha256'], hashlib.sha256(self.main.read_bytes()).hexdigest())
        self.assertEqual(result['helper_sha256'], hashlib.sha256(self.helper.read_bytes()).hexdigest())

    def test_same_inputs_give_identical_bytes(self):
        # WHY: the release hash must not change with the build clock.
        second = self.dir / 'out2'
        second.mkdir()
        again = bundle.assemble(self.main, self.helper, VERSION, second)
        self.assertEqual(again.read_bytes(), self.archive.read_bytes())

    def test_checksum_file_matches_what_install_sh_will_check(self):
        with tarfile.open(self.archive) as a:
            sums = a.extractfile(self.top('SHA256SUMS')).read().decode()
        lines = dict(reversed(l.split('  ')) for l in sums.splitlines())
        self.assertNotIn('SHA256SUMS', lines)
        self.assertIn('install.sh', lines)
        self.assertIn(f'swipe-keyboard-{VERSION}-x86_64.flatpak', lines)

    def test_broken_downloads_are_rejected(self):
        def tamper_flatpak(m):
            name = self.top(f'swipe-keyboard-{VERSION}-x86_64.flatpak')
            m[name] = (m[name][0], b'tampered')
        def extra(m): m[self.top('surprise.sh')] = (0o755, b'echo hi')
        def missing(m): del m[self.top('uninstall.sh')]
        def not_executable(m): m[self.top('install.sh')] = (0o644, m[self.top('install.sh')][1])
        def wrong_version(m): m[self.top('VERSION')] = (0o644, b'0.0.1-1\n')
        def stale_script(m):
            data = m[self.top('install.sh')][1] + b'\n# edited after checksums'
            m[self.top('install.sh')] = (0o755, data)
        def outside(m): m['../escape'] = (0o644, b'x')
        for change in (tamper_flatpak, extra, missing, not_executable, wrong_version, stale_script, outside):
            with self.subTest(change=change.__name__):
                good = self.archive.read_bytes()
                self.rewrite(change)
                with self.assertRaises(ValueError):
                    bundle.verify(self.archive, VERSION)
                self.archive.write_bytes(good)

    def test_scripts_in_the_download_must_equal_the_committed_scripts(self):
        # WHY: a stale or hand-edited install.sh with a self-consistent SHA256SUMS would otherwise pass.
        def swap(m):
            name = self.top('install.sh')
            data = b'#!/bin/sh\necho evil\n'
            m[name] = (0o755, data)
            sums = m[self.top('SHA256SUMS')][1].decode().splitlines()
            m[self.top('SHA256SUMS')] = (0o644, ('\n'.join(
                f'{hashlib.sha256(data).hexdigest()}  install.sh' if l.endswith('  install.sh') else l for l in sums) + '\n').encode())
        self.rewrite(swap)
        with self.assertRaisesRegex(ValueError, 'committed'):
            bundle.verify(self.archive, VERSION)


class SteamosReleaseTargetTests(unittest.TestCase):
    def setUp(self):
        self.contract = release.load_contract()
        self.sha = 'a' * 40

    def test_steamos_is_a_required_target_and_producer(self):
        self.assertIn('steamos', release.PRODUCERS)
        target = self.contract['targets']['steamos']
        self.assertEqual(target['arch'], 'x86_64')
        self.assertEqual(target['files'], ['swipe-keyboard-{version}-{release}-steamos-x86_64.tar.gz'])
        # WHY: existing targets and their file names must not change (assets are never replaced).
        self.assertEqual(self.contract['targets']['flatpak']['files'][0], 'swipe-keyboard-{version}-{release}-x86_64.flatpak')
        self.assertEqual(self.contract['targets']['arch']['files'], ['swipe-keyboard-{version}-{release}-x86_64.pkg.tar.zst'])

    def test_record_checks_the_archive_and_reports_the_embedded_main_bundle_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            main, helper = directory / 'm.flatpak', directory / 'h.flatpak'
            main.write_bytes(b'M'), helper.write_bytes(b'H')
            bundle.assemble(main, helper, release.version(self.contract), directory)
            entry = release.inspect('steamos', directory, None, self.sha, self.contract)
            self.assertEqual(entry['target'], 'steamos')
            self.assertEqual(entry['embedded_main_sha256'], hashlib.sha256(b'M').hexdigest())
            self.assertEqual(len(entry['files']), 1)
            # a download built for another version is not recordable
            (directory / release.filename(self.contract['targets']['steamos']['files'][0], self.contract)).unlink()
            bundle.assemble(main, helper, '9.9.9-1', directory)
            wrong = directory / 'swipe-keyboard-9.9.9-1-steamos-x86_64.tar.gz'
            wrong.rename(directory / release.filename(self.contract['targets']['steamos']['files'][0], self.contract))
            with self.assertRaises(ValueError):
                release.inspect('steamos', directory, None, self.sha, self.contract)

    def evidence(self, directory):
        evidence = []
        for target, definition in self.contract['targets'].items():
            records = []
            for template in definition['files']:
                name = release.filename(template, self.contract)
                (directory / name).write_bytes(b'fixture bytes for ' + target.encode())
                records.append({'name': name, 'size': (directory / name).stat().st_size,
                                'sha256': release.digest(directory / name)})
            evidence.append({'target': target, 'version': release.version(self.contract), 'source_sha': self.sha,
                             'arch': definition['arch'], 'files': records})
        flatpak = next(e for e in evidence if e['target'] == 'flatpak')
        next(e for e in evidence if e['target'] == 'steamos')['embedded_main_sha256'] = flatpak['files'][0]['sha256']
        return evidence

    def test_gate_requires_the_download_to_embed_the_released_flatpak(self):
        # WHY: the SteamOS download and the standalone Flatpak must be the same bytes, or a Deck user and a
        # Desktop-only user would run different builds under one version number.
        statuses = {job: {'result': 'success'} for job in release.PRODUCERS}
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            evidence = self.evidence(directory)
            release.validate(directory, evidence, statuses, self.sha, self.contract)
            for value in ('0' * 64, None):
                broken = copy.deepcopy(evidence)
                steamos = next(e for e in broken if e['target'] == 'steamos')
                if value is None:
                    del steamos['embedded_main_sha256']
                else:
                    steamos['embedded_main_sha256'] = value
                with self.assertRaisesRegex(ValueError, 'SteamOS'):
                    release.validate(directory, broken, statuses, self.sha, self.contract)

    def test_a_missing_steamos_producer_fails_the_gate(self):
        statuses = {job: {'result': 'success'} for job in release.PRODUCERS}
        statuses['steamos']['result'] = 'failure'
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            with self.assertRaises(ValueError):
                release.validate(directory, self.evidence(directory), statuses, self.sha, self.contract)
        del statuses['steamos']
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            with self.assertRaises(ValueError):
                release.validate(directory, self.evidence(directory), statuses, self.sha, self.contract)


if __name__ == '__main__':
    unittest.main()
