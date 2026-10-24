# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Publication is exercised with a fake gh transport; no remote effects or credentials."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('release', ROOT/'scripts/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
spec = importlib.util.spec_from_file_location('publish', ROOT/'scripts/ci-publish.py')
publish = importlib.util.module_from_spec(spec)
with mock.patch.dict(sys.modules, {'release': release}):
    spec.loader.exec_module(publish)


class PublishTests(unittest.TestCase):
    def attempt(self, *, tampered=False, existing=False, untrusted=False):
        contract = release.load_contract()
        sha = 'a'*40
        env = {'GITHUB_SHA': sha, 'GITHUB_REF': 'refs/tags/v' + release.version(contract),
               'GITHUB_EVENT_NAME': 'pull_request' if untrusted else 'push',
               'GITHUB_REPOSITORY': 'itsdestin/swipe-keyboard'}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = root/'release'
            directory.mkdir()
            evidence = []
            for target, definition in contract['targets'].items():
                records = []
                for template in definition['files']:
                    name = release.filename(template, contract)
                    path = directory/name
                    path.write_bytes(b'fixture installable/source bytes')
                    records.append({'name': name, 'size': path.stat().st_size, 'sha256': release.digest(path)})
                evidence.append({'target': target, 'version': release.version(contract),
                                 'source_sha': sha, 'arch': definition['arch'], 'files': records})
            by_target = {e['target']: e for e in evidence}
            by_target['steamos']['embedded_main_sha256'] = by_target['flatpak']['files'][0]['sha256']
            manifest = release.validate(directory, evidence, {j: {'result': 'success'} for j in release.PRODUCERS}, sha, contract)
            (directory/'release-manifest.json').write_text(json.dumps(manifest))
            (directory/'SHA256SUMS').write_text(release.checksums(manifest))
            calls = []
            def run(command, **kwargs):
                calls.append(command)
                if command[:3] == ['gh', 'release', 'view']:
                    return mock.Mock(returncode=0 if existing else 1)
                if command[:3] == ['gh', 'release', 'download']:
                    destination = Path(command[command.index('--dir') + 1])
                    for path in directory.iterdir():
                        shutil.copy2(path, destination/path.name)
                    if tampered:
                        (destination/manifest['files'][0]['name']).write_bytes(b'changed after upload')
                return mock.Mock(returncode=0)
            previous = Path.cwd()
            try:
                os.chdir(root)
                with mock.patch.dict(os.environ, env), mock.patch.object(publish.subprocess, 'run', side_effect=run), mock.patch.object(publish.subprocess, 'check_output', return_value=sha + '\n'):
                    if tampered or existing or untrusted:
                        with self.assertRaises(ValueError):
                            publish.publish()
                    else:
                        publish.publish()
            finally:
                os.chdir(previous)
            edited = any('--draft=false' in command for command in calls)
            self.assertEqual(edited, not (tampered or existing or untrusted))
            if untrusted:
                self.assertEqual(calls, [])
            if existing:
                self.assertFalse(any(command[:3] == ['gh', 'release', 'upload'] for command in calls))
            if not (existing or untrusted):
                upload = next(c for c in calls if c[:3] == ['gh', 'release', 'upload'])
                self.assertEqual(set(upload[4:]), {'release/' + p.name for p in directory.iterdir()})
                self.assertNotIn('--clobber', upload)
                create = next(c for c in calls if c[:3] == ['gh', 'release', 'create'])
                self.assertIn('--draft', create)
                self.assertIn('--prerelease', create)

    def test_verified_exact_upload_publishes_only_after_remote_verification(self):
        self.attempt()

    def test_remote_tampering_leaves_draft(self):
        self.attempt(tampered=True)

    def test_existing_release_never_replaced(self):
        self.attempt(existing=True)

    def test_pr_never_gets_publication(self):
        self.attempt(untrusted=True)


if __name__ == '__main__':
    unittest.main()
