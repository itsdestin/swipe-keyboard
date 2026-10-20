"""Exercise the metadata gate from pinned upstream bytes, not a dirty submodule."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = ROOT / 'third_party/swipe-library'
# WHY: native package recipes run from VCS-free corresponding-source archives.
# Normalize a copied (pristine OR already patched) source back to the pinned
# upstream fixture and verify its full bytes, without requiring Git history.
# Digests are from swipe-library 1b13f2c85d6b347f6ea3fbc4b3aaf01fce42429a.
PRISTINE_SHA256 = {
    'tests/test_metadata.cpp': '21472324e05d5917ddf065c7321cfa8ab1949ff9212373763bd73ac39abc1147',
    'CMakeLists.txt': '389db11573914de7cca18a179a1f48a0805aaa07612be8ce02493f5e5a53799e',
}


class MetadataContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        cls.source = cls.work / 'swipe'
        for name in PRISTINE_SHA256:
            target = cls.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(UPSTREAM / name, target)
        reverse = ['git', 'apply', '--unsafe-paths', '--directory=' + str(cls.source), '--reverse']
        patch = str(ROOT / 'scripts/patches/swipe-metadata.patch')
        if subprocess.run(reverse + ['--check', patch], capture_output=True).returncode == 0:
            subprocess.run(reverse + [patch], check=True, capture_output=True)
        for name, digest in PRISTINE_SHA256.items():
            actual = hashlib.sha256((cls.source / name).read_bytes()).hexdigest()
            if actual != digest:
                raise AssertionError(f'Unknown upstream fixture {name}: {actual} != {digest}')
        cls.executor = cls.work / 'executor'
        target = cls.executor / 'third-party/CMakeLists.txt'
        target.parent.mkdir(parents=True)
        target.write_text('-DFLATBUFFERS_BUILD_TESTS=OFF\n             -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>')
        # The production patch entry point also applies the pinned XNNPACK fix.
        for name in ('patches/executorch-xnnpack-fexceptions.patch',
                     'third_party/executorch/backends/xnnpack/CMakeLists.txt'):
            target = cls.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(UPSTREAM / name, target)
        cls.patch_command = ['python3', str(ROOT / 'scripts/apply-build-patches.py'), str(cls.executor), '--swipe-library', str(cls.source)]
        subprocess.run(cls.patch_command, check=True, capture_output=True)
        cls.binary = cls.work / 'test_metadata'
        subprocess.run(['c++', '-std=c++17', '-O0', '-I' + str(UPSTREAM / 'include'),
                        '-I' + str(UPSTREAM / 'third_party/executorch/third-party/json/include'),
                        str(cls.source / 'tests/test_metadata.cpp'), str(UPSTREAM / 'src/model_metadata.cpp'),
                        '-o', str(cls.binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def validate(self, content):
        with tempfile.TemporaryDirectory(dir=self.work) as tmp:
            (Path(tmp) / 'metadata.json').write_text(content)
            return subprocess.run([str(self.binary), tmp], capture_output=True, text=True)

    def test_real_contextlm_schema(self):
        result = self.validate((ROOT / 'assets/models/hungry_jellyfish/metadata.json').read_text())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('1 contextlm', result.stdout)

    def test_contextlm_rejects_malformed_identity(self):
        # WHY: dimensions/config live in the PTE and vocabulary in vocab.txt;
        # inventing those as required JSON fields rejects the genuine export.
        for value in ({'kind': 'contextlm'}, {'kind': 'contextlm', 'codename': ''},
                      {'kind': 'contextlm', 'codename': 4}, {'kind': 'contextlm', 'codename': None},
                      {'kind': 'contextlm', 'codename': True},
                      {'kind': 'contextlm', 'codename': []}, {'kind': 'contextlm', 'codename': {}},
                      {'kind': 4, 'codename': 'hungry_jellyfish'}, {'codename': 'hungry_jellyfish'},
                      {'kind': 'unknown', 'codename': 'hungry_jellyfish'},
                      {'nested': {'kind': 'contextlm', 'codename': 'hungry_jellyfish'}}):
            with self.subTest(value=value):
                result = self.validate(json.dumps(value))
                self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_contextlm_rejects_invalid_json(self):
        for content in ('{"kind":"contextlm","codename":"x"',
                        '{"kind":"contextlm","codename":"x",}',
                        '{"kind":"contextlm","codename":"x"} trailing',
                        '[{"kind":"contextlm","codename":"x"}]',
                        '{"kind":"contextlm","codename":"x","codename":4}',
                        '{"kind":"contextlm","codename":"x","codename":"y"}',
                        '{"kind":"contextlm","kind":"contextlm","codename":"x"}'):
            with self.subTest(content=content):
                self.assertNotEqual(self.validate(content).returncode, 0)

    def test_top_level_identity_cannot_be_shadowed(self):
        attack = {'nested': {'kind': 'encoder'}, 'kind': 'contextlm', 'codename': 4,
                  'layout': 'en_qwerty', 'max_keys': 64, 'input_shape': [1, 64, 64],
                  'model_config': {'freq_x': 8, 'freq_y': 8}}
        self.assertNotEqual(self.validate(json.dumps(attack)).returncode, 0)
        for model in ('honorable_sturgeon', 'magic_macaw'):
            value = json.loads((ROOT / f'assets/models/{model}/metadata.json').read_text())
            for nested_kind in ('encoder', 'decoder', 'contextlm'):
                for top_kind in (value['kind'], 'contextlm', 'unknown', None):
                    mixed = {'nested': {'kind': nested_kind}, **value, 'kind': top_kind}
                    if top_kind == 'contextlm':
                        mixed['codename'] = 4
                    with self.subTest(model=model, nested=nested_kind, top=top_kind):
                        result = self.validate(json.dumps(mixed))
                        self.assertEqual(result.returncode == 0, top_kind == value['kind'], result.stdout)
            for first, last in (('encoder', 'contextlm'), ('contextlm', 'encoder'),
                                ('decoder', 'encoder'), (value['kind'], value['kind'])):
                duplicate = '{"kind":' + json.dumps(first) + ',' + json.dumps({**value, 'kind': last})[1:]
                with self.subTest(first=first, last=last):
                    self.assertNotEqual(self.validate(duplicate).returncode, 0)
        for nested_kind in ('encoder', 'decoder'):
            missing = {**attack, 'nested': {'kind': nested_kind}}
            del missing['kind']
            self.assertNotEqual(self.validate(json.dumps(missing)).returncode, 0)

    def test_encoder_decoder_contract_preserved(self):
        for model in ('honorable_sturgeon', 'magic_macaw'):
            value = json.loads((ROOT / f'assets/models/{model}/metadata.json').read_text())
            with self.subTest(model=model):
                self.assertEqual(self.validate(json.dumps(value)).returncode, 0)
                value['layout'] = ''
                self.assertNotEqual(self.validate(json.dumps(value)).returncode, 0)
                value['layout'] = 'en_qwerty'
                value.pop('input_shape', None)
                value.pop('input_shapes', None)
                self.assertNotEqual(self.validate(json.dumps(value)).returncode, 0)
                self.assertNotEqual(self.validate(json.dumps({'kind': value['kind']})).returncode, 0)

    def test_patch_idempotent_and_guarded(self):
        before = (self.source / 'tests/test_metadata.cpp').read_bytes()
        subprocess.run(self.patch_command, check=True, capture_output=True)
        self.assertEqual((self.source / 'tests/test_metadata.cpp').read_bytes(), before)
        with tempfile.TemporaryDirectory(dir=self.work) as tmp:
            copy = Path(tmp) / 'swipe'
            shutil.copytree(self.source, copy)
            target = copy / 'tests/test_metadata.cpp'
            target.write_text('unknown upstream validator')
            result = subprocess.run(self.patch_command[:-1] + [str(copy)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(target.read_text(), 'unknown upstream validator')


if __name__ == '__main__':
    unittest.main()
