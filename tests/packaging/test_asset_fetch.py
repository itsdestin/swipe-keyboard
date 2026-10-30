# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Pinned asset caches must rebuild offline and retain strict byte verification."""
from pathlib import Path
import base64
import hashlib
import importlib.util
import io
import shutil
import tempfile
import unittest
from unittest import mock
from urllib.error import HTTPError

ROOT = Path(__file__).resolve().parents[2]


def helper():
    spec = importlib.util.spec_from_file_location('asset_fetch_fixture', ROOT/'scripts/fetch-assets.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def asset(data, path='models/fixture.bin'):
    return {'path': path, 'url': 'https://example.invalid/pinned-input',
            'sha256': hashlib.sha256(data).hexdigest()}


class AssetFetch(unittest.TestCase):
    def test_prepared_decoded_notice_is_authenticated_without_network(self):
        module = helper()
        notice = b'Original public license notice\n'
        encoded = base64.b64encode(notice)
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)/'licenses/AOSP-NOTICE.b64'
            target.parent.mkdir()
            decoded = target.with_suffix('')
            decoded.write_bytes(notice)
            with mock.patch.object(module.urllib.request, 'urlopen', side_effect=AssertionError('offline archive must not fetch')) as network:
                module.fetch(asset(encoded, 'licenses/AOSP-NOTICE.b64'), target)
            network.assert_not_called()
            self.assertEqual(decoded.read_bytes(), notice)
            self.assertFalse(target.exists())

    def test_notice_can_be_prepared_archived_and_rebuilt_offline(self):
        import json
        import tarfile
        module = helper()
        notice = b'Pinned public notice\n'
        encoded = base64.b64encode(notice)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)/'project'
            (root/'packaging/licenses').mkdir(parents=True)
            (root/'packaging/NOTICE').write_text('Project attribution\n')
            (root/'packaging/assets.lock.json').write_text(json.dumps({
                'assets': [asset(encoded, 'licenses/AOSP-NOTICE.b64')]}))
            # WHY a fixed free-space answer: this test is about the notice, and /tmp is a small memory-backed folder on some
            # systems (Kubuntu 26.10: 3.9 GB), where the real 21 GiB check failed the test (card #502, 2026-10-30).
            free = shutil.disk_usage(tmp)._replace(free=64 * 1024**3)
            with mock.patch.object(module, 'ROOT', root), \
                 mock.patch.object(module, 'prepare_language_packs'), mock.patch.object(module, 'fetch_language_files'), \
                 mock.patch('sys.argv', ['fetch-assets.py', '--assets', str(root/'assets')]), \
                 mock.patch.object(module.shutil, 'disk_usage', return_value=free), \
                 mock.patch.object(module.urllib.request, 'urlopen', return_value=io.BytesIO(encoded)):
                module.main()
            self.assertFalse((root/'assets/licenses/AOSP-NOTICE.b64').exists())
            archive_path = Path(tmp)/'source.tar'
            with tarfile.open(archive_path, 'w') as archive:
                archive.add(root, arcname='project')
            rebuilt = Path(tmp)/'rebuilt'
            rebuilt.mkdir()
            with tarfile.open(archive_path) as archive:
                archive.extractall(rebuilt, filter='data')
            copied = rebuilt/'project'
            with mock.patch.object(module, 'ROOT', copied), \
                 mock.patch.object(module, 'prepare_language_packs'), mock.patch.object(module, 'fetch_language_files'), \
                 mock.patch('sys.argv', ['fetch-assets.py', '--assets', str(copied/'assets')]), \
                 mock.patch.object(module.urllib.request, 'urlopen', side_effect=AssertionError('network forbidden')) as network:
                module.main()
            network.assert_not_called()
            self.assertEqual((copied/'assets/licenses/AOSP-NOTICE').read_bytes(), notice)

    def test_language_files_are_fetched_by_pinned_hash_from_the_release(self):
        import json
        module = helper()
        lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
        assets = module.language_file_assets(lock)
        names = {a['path']: a for a in assets}
        # Four languages x (n-gram, word list, credits, dictionary) + ODC-By, Apache-2.0 terms + notice.
        self.assertEqual(len(assets), 19)
        for language in ('en', 'es', 'fr', 'de'):
            for suffix in ('.ngram', '.vocab.tsv', '-CONTRIBUTORS.txt'):
                entry = names[language + suffix]
                self.assertEqual(entry['url'], lock['language_files']['baseUrl'] + language + suffix)
                self.assertRegex(entry['sha256'], r'^[a-f0-9]{64}$')
        for name in ('es.combined', 'fr.combined', 'de.combined', 'en_swipe.combined', 'Apache-2.0.txt', 'ODC-By-1.0.txt'):
            self.assertEqual(names[name]['url'], lock['language_files']['baseUrl'] + name)
        # The notice is stored under a clearer installed name than the release's generic NOTICE.md.
        self.assertTrue(names['LANGUAGE-FILES-NOTICE.md']['url'].endswith('/NOTICE.md'))
        # A tampered download is rejected and leaves nothing behind.
        data = b'not the pinned file'
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)/'language-files/es.ngram'
            with mock.patch.object(module.urllib.request, 'urlopen', return_value=io.BytesIO(data)):
                with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                    module.fetch(names['es.ngram'], target)
            self.assertFalse(target.exists())
            self.assertFalse(target.with_name('es.ngram.part').exists())

    def test_trained_models_are_pinned_and_a_test_mirror_cannot_bypass_the_pins(self):
        import json
        import os
        module = helper()
        lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
        assets = {a['path']: a for a in module.trained_model_assets(lock)}
        self.assertEqual(set(assets), {'language-files/en.nwp.bin', 'language-files/es.nwp.bin', 'language-files/fr.nwp.bin',
                                       'language-files/de.nwp.bin', 'language-files/swipe-model.bin', 'language-files/MODELS-NOTICE.md',
                                       'language-files/FUTO-SWIPE-DATA-LICENSE.txt'})
        base = 'https://github.com/itsdestin/swipe-keyboard/releases/download/models-2026-10-23/'
        for name, asset in assets.items():
            self.assertRegex(asset['sha256'], r'^[a-f0-9]{64}$')
            self.assertTrue(asset['url'].startswith(base), name)
        self.assertEqual(assets['language-files/swipe-model.bin']['url'], base + 'swipe-model.bin')
        # Each download is accepted only against the digest the lock pins for it.
        for language, pin in lock['trained_models']['nextWord'].items():
            self.assertEqual(assets[f'language-files/{language}.nwp.bin']['sha256'], pin['sha256'])
        self.assertEqual(assets['language-files/swipe-model.bin']['sha256'], lock['trained_models']['swipe']['sha256'])
        self.assertEqual(assets['language-files/MODELS-NOTICE.md']['url'], base + 'NOTICE.md')
        with tempfile.TemporaryDirectory() as tmp:
            mirror = Path(tmp)/'mirror'
            mirror.mkdir()
            with mock.patch.dict(os.environ, {'SWIPEKB_TEST_MODELS_MIRROR': str(mirror)}):
                mirrored = {a['path']: a for a in module.trained_model_assets(lock)}
            self.assertEqual(mirrored['language-files/es.nwp.bin']['url'], (mirror/'es.nwp.bin').as_uri())
            self.assertEqual(mirrored['language-files/es.nwp.bin']['sha256'], assets['language-files/es.nwp.bin']['sha256'])
            # A mirror serving other bytes is rejected by the pin, exactly like the real release would be.
            (mirror/'es.nwp.bin').write_bytes(b'not the pinned model')
            target = Path(tmp)/'out/language-files/es.nwp.bin'
            with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                module.fetch(mirrored['language-files/es.nwp.bin'], target)
            self.assertFalse(target.exists())

    def test_valid_decoded_notice_cannot_mask_a_corrupt_encoded_file(self):
        module = helper()
        notice = b'Pinned notice\n'
        encoded = base64.b64encode(notice)
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)/'AOSP-NOTICE.b64'
            target.write_bytes(b'corrupt encoded input')
            target.with_suffix('').write_bytes(notice)
            with mock.patch.object(module.urllib.request, 'urlopen', return_value=io.BytesIO(encoded)) as network:
                module.fetch(asset(encoded, 'licenses/AOSP-NOTICE.b64'), target)
            network.assert_called_once()
            self.assertEqual(target.read_bytes(), encoded)

    def test_corrupt_decoded_notice_does_not_authorize_cache(self):
        module = helper()
        encoded = base64.b64encode(b'Pinned notice\n')
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)/'AOSP-NOTICE.b64'
            target.with_suffix('').write_bytes(b'Changed notice\n')
            with mock.patch.object(module.urllib.request, 'urlopen', return_value=io.BytesIO(encoded)) as network:
                module.fetch(asset(encoded, 'licenses/AOSP-NOTICE.b64'), target)
            network.assert_called_once()
            self.assertEqual(target.read_bytes(), encoded)

    def test_other_encoded_inputs_do_not_use_notice_exception(self):
        module = helper()
        encoded = base64.b64encode(b'Other input')
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)/'dictionary.b64'
            target.with_suffix('').write_bytes(b'Other input')
            with mock.patch.object(module.urllib.request, 'urlopen', return_value=io.BytesIO(encoded)) as network:
                module.fetch(asset(encoded, 'dict-src/dictionary.b64'), target)
            network.assert_called_once()
            self.assertEqual(target.read_bytes(), encoded)

    def test_http_503_retries_then_checks_the_pinned_payload(self):
        module = helper()
        data = b'verified download'
        failure = HTTPError('https://example.invalid/pinned-input', 503, 'Unavailable', {}, None)
        self.addCleanup(failure.close)
        with tempfile.TemporaryDirectory() as tmp, mock.patch('time.sleep') as sleep:
            target = Path(tmp)/'asset.bin'
            with mock.patch.object(module.urllib.request, 'urlopen', side_effect=[failure, io.BytesIO(data)]) as network:
                module.fetch(asset(data), target)
            self.assertEqual(network.call_count, 2)
            sleep.assert_called_once()
            self.assertEqual(target.read_bytes(), data)
            self.assertFalse(target.with_name(target.name+'.part').exists())

    def test_repeated_http_503_is_bounded_and_leaves_no_partial(self):
        module = helper()
        failure = HTTPError('https://example.invalid/pinned-input', 503, 'Unavailable', {}, None)
        self.addCleanup(failure.close)
        with tempfile.TemporaryDirectory() as tmp, mock.patch('time.sleep') as sleep:
            target = Path(tmp)/'asset.bin'
            with mock.patch.object(module.urllib.request, 'urlopen', side_effect=failure) as network:
                with self.assertRaises(HTTPError):
                    module.fetch(asset(b'pinned'), target)
            self.assertEqual(network.call_count, 3)
            self.assertEqual(sleep.call_count, 2)
            self.assertFalse(target.exists())
            self.assertFalse(target.with_name(target.name+'.part').exists())

    def test_checksum_failure_is_not_retried_or_published(self):
        module = helper()
        with tempfile.TemporaryDirectory() as tmp, mock.patch('time.sleep') as sleep:
            target = Path(tmp)/'asset.bin'
            with mock.patch.object(module.urllib.request, 'urlopen', return_value=io.BytesIO(b'wrong bytes')) as network:
                with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                    module.fetch(asset(b'pinned'), target)
            network.assert_called_once()
            sleep.assert_not_called()
            self.assertFalse(target.exists())
            self.assertFalse(target.with_name(target.name+'.part').exists())

    def test_http_404_is_not_retried(self):
        module = helper()
        failure = HTTPError('https://example.invalid/pinned-input', 404, 'Missing', {}, None)
        self.addCleanup(failure.close)
        with tempfile.TemporaryDirectory() as tmp, mock.patch('time.sleep') as sleep:
            with mock.patch.object(module.urllib.request, 'urlopen', side_effect=failure) as network:
                with self.assertRaises(HTTPError):
                    module.fetch(asset(b'pinned'), Path(tmp)/'asset.bin')
            network.assert_called_once()
            sleep.assert_not_called()


if __name__ == '__main__':
    unittest.main()
