"""Behavioral release boundary tests: no remote operations or native compilation."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import tarfile
import io
import subprocess
import sys
import hashlib
import shutil

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('release', ROOT/'scripts/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.sha = 'a' * 40
        self.contract = release.load_contract()
        self.status = {job: {'result': 'success'} for job in release.PRODUCERS}
        self.evidence = []
        for target, definition in self.contract['targets'].items():
            records = []
            for name in definition['files']:
                name = release.filename(name, self.contract)
                path = self.directory/name
                path.write_bytes(b'fixture package/source bytes')
                records.append({'name': name, 'size': path.stat().st_size,
                                'sha256': release.digest(path)})
            self.evidence.append({'target': target, 'version': release.version(self.contract),
                                  'source_sha': self.sha, 'arch': definition['arch'],
                                  'files': records})
        # WHY: the SteamOS download must embed the released Flatpak; fixtures state that it does.
        by_target = {e['target']: e for e in self.evidence}
        by_target['steamos']['embedded_main_sha256'] = by_target['flatpak']['files'][0]['sha256']

    def validate(self):
        return release.validate(self.directory, self.evidence, self.status, self.sha, self.contract)

    def test_complete_required_set(self):
        result = self.validate()
        self.assertEqual(result['source_sha'], self.sha)
        self.assertEqual(len(result['files']), sum(len(t['files']) for t in self.contract['targets'].values()))

    def test_missing_arch(self):
        self.evidence = [e for e in self.evidence if e['target'] != 'arch']
        with self.assertRaises(ValueError):
            self.validate()

    def test_wrong_version_sha_arch_and_missing_metadata(self):
        for key, value in [('version', '0.2.0-1'), ('source_sha', 'b'*40), ('arch', 'aarch64'), ('version', None)]:
            with self.subTest(key=key, value=value):
                original = copy.deepcopy(self.evidence)
                if value is None:
                    del self.evidence[0][key]
                else:
                    self.evidence[0][key] = value
                with self.assertRaises(ValueError):
                    self.validate()
                self.evidence = original

    def test_all_required_producer_outcomes(self):
        for job in release.PRODUCERS:
            for outcome in ('failure', 'cancelled', 'skipped', None):
                with self.subTest(job=job, outcome=outcome):
                    self.status[job]['result'] = outcome
                    with self.assertRaises(ValueError):
                        self.validate()
            self.status[job]['result'] = 'success'
        del self.status['prepare']
        with self.assertRaises(ValueError):
            self.validate()

    def test_duplicate_and_missing_file_metadata(self):
        record = self.evidence[0]['files'].pop()
        with self.assertRaises(ValueError):
            self.validate()
        self.evidence[0]['files'].extend([record, record])
        with self.assertRaises(ValueError):
            self.validate()

    def test_unknown_and_duplicate_target(self):
        self.evidence.append(copy.deepcopy(self.evidence[0]))
        with self.assertRaises(ValueError):
            self.validate()
        self.evidence[-1]['target'] = 'optional-arm'
        with self.assertRaises(ValueError):
            self.validate()

    def test_nonempty_misnamed_missing_unexpected_symlink_and_tampered(self):
        path = self.directory/self.evidence[0]['files'][0]['name']
        original = path.read_bytes()
        for content in (b'', b'tampered after validation'):
            path.write_bytes(content)
            with self.assertRaises(ValueError):
                self.validate()
        path.write_bytes(original)
        renamed = path.with_name('wrong-version.pkg')
        path.rename(renamed)
        with self.assertRaises(ValueError):
            self.validate()
        renamed.rename(path)
        unexpected = self.directory/'unexpected.pkg'
        unexpected.write_bytes(b'extra')
        with self.assertRaises(ValueError):
            self.validate()
        unexpected.unlink()
        path.unlink()
        path.symlink_to('/etc/hostname')
        with self.assertRaises(ValueError):
            self.validate()

    def test_publication_rechecks_manifest_bytes(self):
        result = self.validate()
        (self.directory/'release-manifest.json').write_text(json.dumps(result))
        with self.assertRaises(ValueError):
            release.verify(self.directory, self.sha, self.contract)
        (self.directory/'SHA256SUMS').write_text(release.checksums(result))
        release.verify(self.directory, self.sha, self.contract)
        (self.directory/result['files'][0]['name']).write_bytes(b'changed')
        with self.assertRaises(ValueError):
            release.verify(self.directory, self.sha, self.contract)

    def test_real_cli_gate_and_reverification(self):
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp)
            for entry in self.evidence:
                (evidence/(entry['target'] + '.json')).write_text(json.dumps(entry))
            command = [sys.executable, str(ROOT/'scripts/release.py'), 'gate', '--directory', str(self.directory),
                       '--evidence', str(evidence), '--sha', self.sha, '--statuses', json.dumps(self.status)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('Verified complete release', result.stdout)
            release.verify(self.directory, self.sha, self.contract)
            (evidence/'arch.json').unlink()
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.status['arch']['result'] = 'skipped'
            command[-1] = json.dumps(self.status)
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)

    def test_flatpak_recipe_is_external_portable_and_hash_bound(self):
        spec = importlib.util.spec_from_file_location('ci_source', ROOT/'scripts/ci-source.py')
        ci_source = importlib.util.module_from_spec(spec)
        with mock.patch.dict(sys.modules, {'release': release}):
            spec.loader.exec_module(ci_source)
        fetcher = ci_source.module('fixture_fetch', ROOT/'scripts/fetch-assets.py')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root/'complete-source.tar.gz'
            source.write_bytes(b'finalized prepared source fixture with assets and licenses')
            original = release.digest(source)
            wheel = root/'tool.whl'
            wheel.write_bytes(b'pinned wheel fixture')
            inputs = root/'inputs'
            inputs.mkdir()
            recipe = {'modules': [{'sources': [
                {'type': 'archive', 'path': 'swipe-source.tar.xz', 'sha256': 'old-lab-hash'},
                {'type': 'file', 'url': wheel.as_uri(), 'sha256': release.digest(wheel), 'dest': 'wheelcache'},
                {'type': 'patch', 'path': '../../scripts/patches/plasma-prediction-policy.patch'}]}]}
            result = ci_source.external_manifest(recipe, source, inputs, fetcher)
            sources = result['modules'][0]['sources']
            self.assertEqual(sources[0]['sha256'], original)
            self.assertEqual(release.digest(inputs/sources[0]['path']), original)
            self.assertEqual(release.digest(source), original, 'no self-hash writeback into source')
            self.assertEqual(sources[1]['dest-filename'], 'tool.whl')
            self.assertNotIn('url', sources[1])
            self.assertEqual(release.digest(inputs/sources[1]['path']), sources[1]['sha256'])
            self.assertTrue((inputs/sources[2]['path']).is_file())
            self.assertEqual(result['sdk-commit'], json.loads((ROOT/'packaging/flatpak/runtime-lock.json').read_text())['org.kde.Sdk'])
            bad = {'modules': [{'sources': [{'type': 'file', 'url': wheel.as_uri(), 'sha256': '0'*64}]}]}
            with self.assertRaises(ValueError):
                ci_source.external_manifest(bad, source, inputs, fetcher)

    def test_source_preparation_fixture_builds_english_and_preserves_raw_inputs(self):
        spec = importlib.util.spec_from_file_location('ci_source', ROOT/'scripts/ci-source.py')
        ci_source = importlib.util.module_from_spec(spec)
        with mock.patch.dict(sys.modules, {'release': release}):
            spec.loader.exec_module(ci_source)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture = root/('swipe-keyboard-' + self.contract['version'])
            (fixture/'scripts').mkdir(parents=True)
            (fixture/'packaging/flatpak').mkdir(parents=True)
            wrapper = fixture/'scripts/fetch-assets.sh'
            wrapper.write_text('''set -eu
mkdir -p "$SWIPEKB_ASSETS/dict" "$SWIPEKB_ASSETS/dict-src/.venv"
printf fixture-english > "$SWIPEKB_ASSETS/dict/en_swipe.combined"
printf raw-aosp-source > "$SWIPEKB_ASSETS/dict-src/input.b64"
printf generated-tool > "$SWIPEKB_ASSETS/dict-src/.venv/tool"
''')
            (fixture/'packaging/assets.lock.json').write_text(json.dumps({'dictionary_sha256': hashlib.sha256(b'fixture-english').hexdigest()}))
            (fixture/'packaging/flatpak/org.itsdestin.SwipeKeyboard.json').write_text(json.dumps({'modules': [{'sources': [{'type': 'archive', 'path': 'swipe-source.tar.xz'}]}]}))
            shutil.copy2(ROOT/'packaging/flatpak/runtime-lock.json', fixture/'packaging/flatpak/runtime-lock.json')
            for profile in ('', 'native-aarch64', 'kde-6.10'):
                directory = fixture/'requirements'/profile
                directory.mkdir(parents=True, exist_ok=True)
                (directory/'wheels.lock.json').write_text('{"wheels": {}, "torchgen": {"filename": "fixture.whl"}}')
            output = root/'prepared'
            source_name = release.filename(self.contract['targets']['source']['files'][0], self.contract)
            actual_run = subprocess.run
            def run(command, **kwargs):
                if Path(command[1]).name == 'package-source.sh':
                    with tarfile.open(output/source_name, 'w:gz') as archive:
                        archive.add(fixture, arcname=fixture.name)
                    (output/(source_name + '.sha256')).write_text('initial snapshot checksum')
                    return mock.Mock(returncode=0)
                return actual_run(command, **kwargs)
            def fetch(item, cache):
                cache.mkdir(parents=True, exist_ok=True)
                (cache/item['filename']).write_bytes(b'fixture wheel')
            wheels = mock.Mock(fetch=fetch)
            with mock.patch.object(ci_source, 'ROOT', fixture), mock.patch.object(ci_source.subprocess, 'check_output', side_effect=['', self.sha]), mock.patch.object(ci_source.subprocess, 'run', side_effect=run), mock.patch.object(ci_source, 'module', side_effect=[wheels, mock.Mock()]):
                ci_source.prepare(output)
            release.verify_inputs(output, self.sha, self.contract)
            release.inspect('source', output, None, self.sha, self.contract)
            with tarfile.open(output/source_name) as archive:
                self.assertEqual(archive.extractfile(fixture.name + '/assets/dict/en_swipe.combined').read(), b'fixture-english')
                self.assertIn(fixture.name + '/assets/dict-src/input.b64', archive.getnames())
                self.assertNotIn(fixture.name + '/assets/dict-src/.venv', archive.getnames())

    def test_recipe_versions_agree(self):
        release.check_versions(self.contract)
        contract = copy.deepcopy(self.contract)
        contract['version'] = '9.9.9'
        with self.assertRaises(ValueError):
            release.check_versions(contract)

    def test_source_archive_embedded_identity_is_checked(self):
        name = release.filename(self.contract['targets']['source']['files'][0], self.contract)
        def archive(sha):
            content = json.dumps({'source_sha': sha, 'version': release.version(self.contract)}).encode()
            with tarfile.open(self.directory/name, 'w:gz') as out:
                member = tarfile.TarInfo(f"swipe-keyboard-{self.contract['version']}/packaging/source-identity.json")
                member.size = len(content)
                out.addfile(member, io.BytesIO(content))
        archive(self.sha)
        result = release.inspect('source', self.directory, None, self.sha, self.contract)
        self.assertEqual(result['source_sha'], self.sha)
        archive('b'*40)
        with self.assertRaises(ValueError):
            release.inspect('source', self.directory, None, self.sha, self.contract)

    def test_native_metadata_and_embedded_version_checked_before_evidence(self):
        stage = self.directory/'stage'
        base = stage/'usr/share/swipe-keyboard'
        base.mkdir(parents=True)
        (base/'source-identity.json').write_text(json.dumps({'source_sha': self.sha, 'version': release.version(self.contract)}))
        runtime = base/'runtime.json'
        runtime.write_text(json.dumps({'package': release.version(self.contract)}))
        good = f"pkgname = swipe-keyboard\npkgver = {release.version(self.contract)}\narch = x86_64"
        with mock.patch.object(release, 'output', return_value=good):
            release.inspect('arch', self.directory, stage, self.sha, self.contract)
        for metadata in (good.replace('x86_64', 'aarch64'), good.replace(release.version(self.contract), '0.2.0-1')):
            with mock.patch.object(release, 'output', return_value=metadata), self.assertRaises(ValueError):
                release.inspect('arch', self.directory, stage, self.sha, self.contract)
        runtime.write_text('{"package": "0.2.0-1"}')
        with self.assertRaises(ValueError):
            release.inspect('arch', self.directory, stage, self.sha, self.contract)

    def test_fedora_binary_arch_and_source_rpm_marker(self):
        stage = self.directory/'stage'
        base = stage/'usr/share/swipe-keyboard'
        base.mkdir(parents=True)
        (base/'source-identity.json').write_text(json.dumps({'source_sha': self.sha, 'version': release.version(self.contract)}))
        (base/'runtime.json').write_text(json.dumps({'package': release.version(self.contract) + '.fc44'}))
        prefix = 'swipe-keyboard ' + release.version(self.contract) + '.fc44 '
        with mock.patch.object(release, 'output', side_effect=[prefix + 'x86_64', prefix + '1']) as query:
            release.inspect('fedora', self.directory, stage, self.sha, self.contract)
            self.assertIn('%{SOURCEPACKAGE}', query.call_args.args[0][3])
        for values in ([prefix + 'aarch64', prefix + '1'], [prefix + 'x86_64', prefix + '(none)']):
            with mock.patch.object(release, 'output', side_effect=values), self.assertRaises(ValueError):
                release.inspect('fedora', self.directory, stage, self.sha, self.contract)

    def test_arm_is_mandatory_and_has_no_colliding_source_rpm(self):
        self.assertIn('fedora-arm64', release.PRODUCERS)
        target = self.contract['targets']['fedora-arm64']
        self.assertEqual(target['arch'], 'aarch64')
        self.assertEqual(target['files'], ['swipe-keyboard-{version}-{release}.fc44.aarch64.rpm'])
        names = [name for target in self.contract['targets'].values() for name in target['files']]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(sum(name.endswith('.src.rpm') for name in names), 1)
        self.evidence = [e for e in self.evidence if e['target'] != 'fedora-arm64']
        with self.assertRaises(ValueError):
            self.validate()

    def test_arm_rpm_inspection_requires_arch_version_and_embedded_identity(self):
        stage = self.directory/'stage'
        base = stage/'usr/share/swipe-keyboard'
        base.mkdir(parents=True)
        identity = base/'source-identity.json'
        runtime = base/'runtime.json'
        identity.write_text(json.dumps({'source_sha': self.sha, 'version': release.version(self.contract)}))
        runtime.write_text(json.dumps({'package': release.version(self.contract) + '.fc44'}))
        good = 'swipe-keyboard ' + release.version(self.contract) + '.fc44 aarch64'
        with mock.patch.object(release, 'output', return_value=good) as query:
            release.inspect('fedora-arm64', self.directory, stage, self.sha, self.contract)
            self.assertEqual(query.call_count, 1)
            self.assertIn('%{ARCH}', query.call_args.args[0][3])
        for bad in (good.replace('aarch64', 'x86_64'), good.replace(release.version(self.contract), '0.2.0-1')):
            with mock.patch.object(release, 'output', return_value=bad), self.assertRaises(ValueError):
                release.inspect('fedora-arm64', self.directory, stage, self.sha, self.contract)
        for path, content in [(identity, {'source_sha': 'b'*40, 'version': release.version(self.contract)}), (runtime, {'package': '0.2.0-1.fc44'})]:
            original = path.read_text()
            path.write_text(json.dumps(content))
            with mock.patch.object(release, 'output', return_value=good), self.assertRaises(ValueError):
                release.inspect('fedora-arm64', self.directory, stage, self.sha, self.contract)
            path.write_text(original)
        with self.assertRaises(ValueError):
            release.inspect('fedora-arm64', self.directory, None, self.sha, self.contract)

    def test_cross_job_input_hashes_and_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            records = []
            for name in release.input_names(self.contract):
                path = directory/name
                path.write_bytes(b'prepared source fixture')
                records.append({'name': name, 'size': path.stat().st_size, 'sha256': release.digest(path)})
            manifest = {'source_sha': self.sha, 'version': release.version(self.contract), 'files': records}
            (directory/'input-manifest.json').write_text(json.dumps(manifest))
            release.verify_inputs(directory, self.sha, self.contract)
            with self.assertRaises(ValueError):
                release.verify_inputs(directory, 'b'*40, self.contract)
            (directory/records[0]['name']).write_bytes(b'changed prepared archive')
            with self.assertRaises(ValueError):
                release.verify_inputs(directory, self.sha, self.contract)

    def test_checksum_file_is_bound(self):
        result = self.validate()
        (self.directory/'release-manifest.json').write_text(json.dumps(result))
        (self.directory/'SHA256SUMS').write_text('forged checksums')
        with self.assertRaises(ValueError):
            release.verify(self.directory, self.sha, self.contract)

    def test_duplicate_json_keys_rejected(self):
        with self.assertRaises(ValueError):
            release.parse_json('{"arch": 1, "arch": 2}')


if __name__ == '__main__':
    unittest.main()
