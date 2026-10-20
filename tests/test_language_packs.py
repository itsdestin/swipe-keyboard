"""Reproducible data-only packs: mechanics, never language-quality acceptance."""
import importlib.util
import pathlib
import os
import json
import hashlib
import zipfile
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

    def test_deterministic_fixture_dictionary(self):
        m = self.module
        bins = [[], ['si', 'sí', 'niño', 'bad!'], ['sí', 'casa']]
        a = m.dictionary_bytes(bins, 'es', 10)
        self.assertEqual(a, m.dictionary_bytes(bins, 'es', 10))
        self.assertIn('word=sí,'.encode(), a)
        self.assertIn('word=si,'.encode(), a)
        self.assertNotIn(b'bad!', a)
        self.assertNotIn(b'EXPERIMENTAL', a)
        self.assertEqual(a.count('word=sí,'.encode()), 1)

    def test_common_orthography_keeps_elision_sharp_s_and_noun_case(self):
        # WHY wordfreq's tokenized/case-folded forms are not always printable dictionary forms.
        fr = self.module.dictionary_bytes([[], ['école', 'homme', 'amour', 'accord']], 'fr', 20).decode()
        for form in ('l’école', 'l’homme', 'l’amour', 'd’accord'):
            self.assertIn(f'word={form},', fr)
        de = self.module.dictionary_bytes([[], ['strasse', 'strassen', 'häuser', 'krankenhaus', 'schule', 'schön']], 'de', 20).decode()
        for form in ('Straße', 'Straßen', 'Häuser', 'Krankenhaus', 'Schule'):
            self.assertIn(f'word={form},', de)
        self.assertIn('word=schön,', de)  # no blanket German title-casing or accent erasure
        self.assertNotIn('word=Strasse,', de)

    def generator_inputs(self):
        names = ('SWIPEKB_TEST_WORDFREQ_WHEEL', 'SWIPEKB_TEST_ENGLISH_ASSETS', 'SWIPEKB_TEST_CC_LICENSE')
        if not all(os.environ.get(n) for n in names):
            self.skipTest('Explicit cached wheel, pinned model assets and CC license fixtures required')
        paths = tuple(pathlib.Path(os.environ[n]) for n in names)
        for path in paths:
            self.assertTrue(path.exists(), str(path))
        return paths

    def test_whole_generate_retains_pinned_terms_and_is_immutable(self):
        wheel, assets, cc = self.generator_inputs()
        with tempfile.TemporaryDirectory() as d:
            out = pathlib.Path(d)/'pack'
            self.module.generate(wheel, 'fr', assets, cc, out, 30)
            manifest = json.loads((out/'fr.json').read_text())
            terms = out/manifest['license']['termsPath']
            self.assertEqual(terms.read_bytes(), cc.read_bytes())
            lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
            self.assertEqual(hashlib.sha256(terms.read_bytes()).hexdigest(), lock['cc_license_source']['sha256'])
            self.assertEqual(manifest['license']['termsSha256'], lock['cc_license_source']['sha256'])
            notice = (out/manifest['license']['path']).read_bytes()
            with zipfile.ZipFile(wheel) as z:
                self.assertIn(z.read('wordfreq-3.1.1.dist-info/METADATA'), notice)
                self.assertIn(z.read('wordfreq-3.1.1.dist-info/LICENSE.txt'), notice)
            self.assertIn(cc.read_bytes(), notice)
            self.assertEqual((out/'FUTO-MODEL-LICENSE.md').read_bytes(), (assets/'models/FUTO-MODEL-LICENSE.md').read_bytes())
            snapshot = lambda: {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in out.iterdir()}
            before = snapshot()
            self.module.generate(wheel, 'fr', assets, cc, out, 30)
            self.assertEqual(snapshot(), before)
            with self.assertRaisesRegex(ValueError, 'immutable'):
                self.module.generate(wheel, 'fr', assets, cc, out, 31)
            self.assertEqual(snapshot(), before)
            (out/'encoder.pte').write_bytes(b'locally corrupted immutable output')
            corrupt = snapshot()
            with self.assertRaisesRegex(ValueError, 'immutable'):
                self.module.generate(wheel, 'fr', assets, cc, out, 30)
            self.assertEqual(snapshot(), corrupt)

    def test_generate_rejects_unpinned_license_before_output(self):
        wheel, assets, _ = self.generator_inputs()
        with tempfile.TemporaryDirectory() as d:
            cc = pathlib.Path(d)/'fake-license'
            cc.write_text('self-attested license text')
            out = pathlib.Path(d)/'pack'
            with self.assertRaisesRegex(ValueError, 'license checksum'):
                self.module.generate(wheel, 'de', assets, cc, out, 30)
            self.assertFalse(out.exists())

    def test_normal_preparation_builds_all_three_packs_from_pinned_cache_without_network(self):
        from unittest import mock
        wheel, assets, cc = self.generator_inputs()
        spec = importlib.util.spec_from_file_location('fetch_assets', ROOT/'scripts/fetch-assets.py')
        fetcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fetcher)
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            # Only the generator reads this explicit fixture link; no runtime containment bypass.
            (root/'models').symlink_to(assets/'models', target_is_directory=True)
            with mock.patch.object(fetcher.urllib.request, 'urlopen', side_effect=AssertionError('No network permitted')):
                fetcher.prepare_language_packs(root, wheel, cc)
                before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (root/'language-packs').iterdir()}
                fetcher.prepare_language_packs(root, wheel, cc)
            after = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (root/'language-packs').iterdir()}
            self.assertEqual(before, after)
            for language in ('es', 'fr', 'de'):
                manifest = json.loads((root/f'language-packs/{language}.json').read_text())
                self.assertEqual(manifest['id'], f'wordfreq-{language}')
                self.assertNotIn('experimental', manifest)
            # Missing system parser uses the existing pinned build-only venv, not an undeclared dependency.
            with mock.patch('importlib.util.find_spec', return_value=None), mock.patch('subprocess.run') as run:
                fetcher.prepare_language_packs(root, wheel, cc)
            self.assertEqual(run.call_count, 4)
            self.assertIn('provision-build-deps.py', run.call_args_list[0].args[0][1])
            self.assertEqual([call.args[0][call.args[0].index('--language')+1]
                              for call in run.call_args_list[1:]], ['es','fr','de'])

    def test_reject_unpinned_wheel(self):
        with tempfile.TemporaryDirectory() as d:
            wheel = pathlib.Path(d)/'bad.whl'
            wheel.write_bytes(b'not pinned')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                self.module.read_bins(wheel, 'fr')


if __name__ == '__main__':
    unittest.main()
