# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Reproducible data-only packs: mechanics, never language-quality acceptance."""
import importlib.util
import pathlib
import os
import json
import hashlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('language_pack', ROOT/'scripts/dict/language_pack.py')


class LanguagePacks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = importlib.util.module_from_spec(SPEC)
        SPEC.loader.exec_module(cls.module)

    def test_unicode_identity_and_filtering(self):
        m = self.module
        self.assertEqual(m.clean_word('e\u0301cole', 'fr'), 'école')
        self.assertEqual(m.clean_word('l’école', 'fr'), 'l’école')
        for word in ('sí', 'si', 'niño'):
            self.assertEqual(m.clean_word(word, 'es'), word)
        for word in ('Straße', 'schön'):
            self.assertEqual(m.clean_word(word, 'de'), word)
        for word in ('<html>', 'a,b', 'hello world', '123', 'русский', "'word", 'a--b'):
            self.assertIsNone(m.clean_word(word, 'fr'))

    def generator_inputs(self):
        # WHY explicit: the real files are a 125 MB pinned download (scripts/fetch-assets.py fetches them);
        # supplied-but-wrong fixtures fail, absent ones skip.
        folder = os.environ.get('SWIPEKB_TEST_ENGLISH_ASSETS')
        if not folder or not (pathlib.Path(folder)/'language-files').is_dir():
            self.skipTest('Set SWIPEKB_TEST_ENGLISH_ASSETS to an assets folder with language-files/ (fetch-assets.py)')
        return pathlib.Path(folder)

    def assets_root(self, tmp, assets, language_files=True):
        """A scratch assets folder with (optionally) the downloaded language files."""
        root = pathlib.Path(tmp)/'assets'
        root.mkdir()
        if language_files:
            (root/'language-files').symlink_to(assets/'language-files', target_is_directory=True)
        return root

    def lock(self):
        return json.loads((ROOT/'packaging/assets.lock.json').read_text())

    def test_neural_next_word_model_joins_a_pack_only_when_it_matches_the_lock(self):
        import copy
        m = self.module
        lock = copy.deepcopy(self.lock())
        sha = lambda data: hashlib.sha256(data).hexdigest()
        shared = lock['language_files']
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp)
            pin = lock['multilingual_context_models']['es']
            files = {'es.ngram': b'g', 'es.vocab.tsv': b'v', 'es-CONTRIBUTORS.txt': b'c',
                     shared['odcTerms']['path']: b'odc', shared['notice']['path']: b'n',
                     lock['language_dictionaries']['es']['path']: b'd'}
            for name, data in files.items():
                (source/name).write_bytes(data)
            pin.update(modelSha256=sha(b'g'), vocabSha256=sha(b'v'), contributorsSha256=sha(b'c'))
            shared['odcTerms']['sha256'] = sha(b'odc'); shared['notice']['sha256'] = sha(b'n')
            lock['language_dictionaries']['es']['sha256'] = sha(b'd')
            models = lock['trained_models']
            (source/models['notice']['path']).write_bytes(b'models notice')
            models['notice']['sha256'] = sha(b'models notice')
            models['nextWord']['es'].update(sha256=sha(b'nwp'), bytes=3)
            # Without the model file the pack is exactly what it was before.
            self.assertNotIn('es.nwp.bin', m.context_pack_files('es', lock, source))
            (source/'es.nwp.bin').write_bytes(b'nwp')
            result = m.context_pack_files('es', lock, source)
            self.assertEqual(result['es.nwp.bin'], b'nwp')
            self.assertEqual(result[models['notice']['path']], b'models notice')
            # A present file that differs from the pin (even by one byte, same size) aborts the build.
            (source/'es.nwp.bin').write_bytes(b'nwq')
            with self.assertRaisesRegex(ValueError, 'pin mismatch: es.nwp.bin'):
                m.context_pack_files('es', lock, source)

    def test_lock_has_no_share_alike_inputs(self):
        lock = self.lock()
        for gone in ('wordfreq_wheel', 'wordfreq_source', 'cc_license_source'):
            self.assertNotIn(gone, lock)
        self.assertEqual(lock['dictionary_sha256'], lock['language_dictionaries']['en']['sha256'])

    def test_pack_without_language_files_is_refused_not_half_built(self):
        assets = self.generator_inputs()
        with tempfile.TemporaryDirectory() as d:
            out = pathlib.Path(d)/'pack'
            with self.assertRaisesRegex(ValueError, 'not downloaded'):
                self.module.generate('es', self.assets_root(d, assets, language_files=False), out)
            self.assertFalse(out.exists())
            # English likewise installs nothing without the downloaded files.
            empty = pathlib.Path(d)/'empty'
            empty.mkdir()
            self.assertEqual(self.module.generate_english_context(empty, pathlib.Path(d)/'en'), [])

    def test_pack_carries_open_licences_and_pinned_files(self):
        assets = self.generator_inputs()
        lock = self.lock()
        with tempfile.TemporaryDirectory() as d:
            root = self.assets_root(d, assets)
            out = pathlib.Path(d)/'pack'
            self.module.generate('de', root, out)
            manifest = json.loads((out/'de.json').read_text())
            self.assertEqual(manifest['schema'], 3)
            self.assertEqual(manifest['compatibility'], 'swipekb-open-v1')
            self.assertEqual(set(manifest['assets']), {'dictionary', 'contextModel', 'contextVocab'})
            # WHY an exact key set: a pack carries only what the open format defines (no other engine's pins).
            self.assertEqual(set(manifest), {'schema', 'id', 'version', 'language', 'locales', 'layouts', 'modes', 'scripts',
                                             'compatibility', 'textRulesVersion', 'transformationVersion', 'license',
                                             'source', 'assets', 'contextFormat', 'contextLicense'})
            self.assertEqual(manifest['id'], 'fineweb-de')
            self.assertEqual(manifest['transformationVersion'], 'fineweb-counts-v1')
            self.assertEqual(manifest['source'], lock['word_list_source'])
            self.assertEqual(manifest['license']['id'], 'ODC-By-1.0 AND CC-BY-2.0-FR')
            self.assertEqual(manifest['license']['termsSha256'], lock['language_files']['odcTerms']['sha256'])
            self.assertEqual(manifest['license']['tatoebaTermsSha256'], lock['tatoeba_license_notice_sha256'])
            for name in ('de.combined', 'de.ngram', 'de.vocab.tsv', 'de-CONTRIBUTORS.txt', 'ODC-By-1.0.txt',
                         'LANGUAGE-FILES-NOTICE.md', 'TATOEBA-CC-BY-2.0-FR.txt'):
                self.assertTrue((out/name).is_file(), name)
            for name in ('CC-BY-SA-4.0.txt', 'de-LICENSE.txt', 'metadata.json'):
                self.assertFalse((out/name).exists(), name)
            self.assertEqual(hashlib.sha256((out/'de.combined').read_bytes()).hexdigest(), lock['language_dictionaries']['de']['sha256'])
            context = lock['multilingual_context_models']['de']
            for role, pin in (('contextModel', 'modelSha256'), ('contextVocab', 'vocabSha256')):
                self.assertEqual(manifest['assets'][role]['sha256'], context[pin])
            terms = manifest['contextLicense']
            self.assertEqual(terms['id'], 'ODC-By-1.0 AND CC-BY-2.0-FR')
            self.assertEqual(terms['sourceUrl'], lock['language_files']['baseUrl'] + 'de.ngram')
            for path_key, hash_key in (('attributionPath', 'attributionSha256'), ('termsPath', 'termsSha256'),
                                       ('webTermsPath', 'webTermsSha256'), ('noticePath', 'noticeSha256')):
                self.assertEqual(hashlib.sha256((out/terms[path_key]).read_bytes()).hexdigest(), terms[hash_key])
            # English next-word files are installed beside the packs, with the licence texts they need (incl. AOSP's Apache-2.0).
            written = self.module.generate_english_context(root, out)
            for name in ('en.ngram', 'en.vocab.tsv', 'en-CONTRIBUTORS.txt', 'ODC-By-1.0.txt', 'LANGUAGE-FILES-NOTICE.md',
                         'TATOEBA-CC-BY-2.0-FR.txt', 'Apache-2.0.txt'):
                self.assertIn(name, written)
                self.assertTrue((out/name).is_file(), name)
            self.assertNotIn('CC-BY-SA-4.0.txt', written)
            self.assertEqual(self.module.generate_english_context(root, out), written)  # rerun changes nothing
            # The shared swipe model sits beside the packs, exactly as pinned, with its notices.
            shared = self.module.generate_shared(root, out)
            if shared:
                swipe = lock['trained_models']['swipe']
                self.assertEqual(hashlib.sha256((out/'swipe-model.bin').read_bytes()).hexdigest(), swipe['sha256'])
                self.assertIn(lock['trained_models']['swipeDataTerms']['path'], shared)

    def test_wrong_or_partial_language_files_are_rejected_before_any_output(self):
        assets = self.generator_inputs()
        files = assets/'language-files'
        with tempfile.TemporaryDirectory() as d:
            bad = pathlib.Path(d)/'assets'
            (bad/'language-files').mkdir(parents=True)
            for source in files.iterdir():
                (bad/'language-files'/source.name).write_bytes(source.read_bytes())
            for tampered in ('es.ngram', 'es.combined'):
                original = (files/tampered).read_bytes()
                (bad/'language-files'/tampered).write_bytes(original + b'x')
                with self.assertRaisesRegex(ValueError, 'pin mismatch'):
                    self.module.generate('es', bad, pathlib.Path(d)/'a')
                self.assertFalse((pathlib.Path(d)/'a').exists())
                (bad/'language-files'/tampered).write_bytes(original)
            (bad/'language-files/ODC-By-1.0.txt').unlink()
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                self.module.generate('es', bad, pathlib.Path(d)/'b')
            self.assertFalse((pathlib.Path(d)/'b').exists())

    def test_generate_is_immutable_and_idempotent(self):
        assets = self.generator_inputs()
        with tempfile.TemporaryDirectory() as d:
            root = self.assets_root(d, assets)
            out = pathlib.Path(d)/'pack'
            self.module.generate('fr', root, out)
            snapshot = lambda: {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in out.iterdir()}
            before = snapshot()
            self.module.generate('fr', root, out)
            self.assertEqual(snapshot(), before)
            (out/'fr.combined').write_bytes(b'locally corrupted immutable output')
            corrupt = snapshot()
            with self.assertRaisesRegex(ValueError, 'immutable'):
                self.module.generate('fr', root, out)
            self.assertEqual(snapshot(), corrupt)

    def test_normal_preparation_builds_all_three_packs_from_pinned_cache_without_network(self):
        from unittest import mock
        assets = self.generator_inputs()
        spec = importlib.util.spec_from_file_location('fetch_assets', ROOT/'scripts/fetch-assets.py')
        fetcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fetcher)
        with tempfile.TemporaryDirectory() as tmp:
            root = self.assets_root(tmp, assets)
            with mock.patch.object(fetcher.urllib.request, 'urlopen', side_effect=AssertionError('No network permitted')):
                fetcher.prepare_language_packs(root)
                before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (root/'language-packs').iterdir()}
                fetcher.prepare_language_packs(root)
            after = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (root/'language-packs').iterdir()}
            self.assertEqual(before, after)
            for language in ('es', 'fr', 'de'):
                manifest = json.loads((root/f'language-packs/{language}.json').read_text())
                self.assertEqual(manifest['id'], f'fineweb-{language}')
                self.assertNotIn('experimental', manifest)


if __name__ == '__main__':
    unittest.main()
