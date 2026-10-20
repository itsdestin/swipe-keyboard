"""Executable build inputs must be hash-locked, including cached downloads."""
import hashlib
import json
import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]

class BuildDependencies(unittest.TestCase):
    def helper(self):
        spec = importlib.util.spec_from_file_location('builddeps', ROOT/'scripts/provision-build-deps.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_arm_manifest_requirements_and_wheels_match(self):
        directory = ROOT/'requirements/native-aarch64'
        manifest = json.loads((directory/'wheels.lock.json').read_text())
        native = json.loads((ROOT/'requirements/wheels.lock.json').read_text())
        for group, names in manifest['groups'].items():
            expected = ''.join(f"{name}=={manifest['wheels'][name]['version']} --hash=sha256:{manifest['wheels'][name]['sha256']}\n" for name in names)
            self.assertEqual((directory/(group + '.txt')).read_text(), expected)
        for name, wheel in manifest['wheels'].items():
            self.assertEqual(wheel['version'], native['wheels'][name]['version'])
            self.assertNotIn('x86_64', wheel['filename'])
            self.assertRegex(wheel['filename'], r'(aarch64|none-any)\.whl$')
        # Extract-only code generator, never an installed foreign torch extension.
        self.assertEqual(manifest['torchgen'], native['torchgen'])

    def test_native_profile_automatically_selects_machine_and_keeps_interpreter_guard(self):
        helper = self.helper()
        for machine, profile, version, expected in [
            ('aarch64', 'native', (3, 14), 'native-aarch64'),
            ('x86_64', 'native', (3, 14), ''),
            ('x86_64', 'kde-6.10', (3, 13), 'kde-6.10'),
        ]:
            manifest = json.loads((ROOT/'requirements'/expected/'wheels.lock.json').read_text())
            with mock.patch.object(helper.platform, 'machine', return_value=machine), mock.patch.object(helper.sys, 'version_info', version), mock.patch.object(helper.sys, 'platform', 'linux'), mock.patch.object(helper, 'fetch', side_effect=[pathlib.Path('/fixture-pip.whl'), ValueError('fixture stop before installation')]) as fetch:
                with self.assertRaisesRegex(ValueError, 'fixture stop'):
                    helper.provision('futo', pathlib.Path('/unused'), pathlib.Path('/cache'), profile)
                self.assertEqual(fetch.call_args.args[0], manifest['wheels']['cmake'])
        for machine, profile, version in [('aarch64', 'native', (3, 13)), ('aarch64', 'kde-6.10', (3, 13)), ('riscv64', 'native', (3, 14))]:
            with mock.patch.object(helper.platform, 'machine', return_value=machine), mock.patch.object(helper.sys, 'version_info', version), mock.patch.object(helper, 'fetch') as fetch:
                with self.assertRaises(RuntimeError):
                    helper.provision('futo', pathlib.Path('/unused'), pathlib.Path('/cache'), profile)
                fetch.assert_not_called()

    def test_missing_hash_refused_before_fetch(self):
        helper = self.helper()
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'sha256'):
                helper.fetch({'url': 'https://example.invalid/a.whl', 'filename': 'a.whl'}, pathlib.Path(tmp))

    def test_corrupt_cached_and_remote_bytes_refused(self):
        helper = self.helper()
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp)
            source = p/'source'
            source.write_bytes(b'corrupt')
            cache = p/'cache'
            cache.mkdir()
            (cache/'a.whl').write_bytes(b'corrupt cached')
            item = {'url': source.as_uri(), 'filename': 'a.whl', 'sha256': hashlib.sha256(b'good').hexdigest()}
            with self.assertRaisesRegex(ValueError, 'checksum'):
                helper.fetch(item, cache)
            self.assertFalse((cache/'a.whl.part').exists())

    def test_pip_command_is_offline_hash_locked_no_deps(self):
        helper = self.helper()
        with mock.patch.object(helper.subprocess, 'run') as run:
            helper.install(pathlib.Path('/venv/bin/python'), pathlib.Path('/bounded/requirements.txt'), pathlib.Path('/cache'))
        command = run.call_args.args[0]
        for flag in ('--require-hashes', '--no-deps', '--no-index', '--only-binary=:all:'):
            self.assertIn(flag, command)
        self.assertEqual(command[command.index('-r') + 1], '/bounded/requirements.txt')
        self.assertNotIn('--upgrade', command)

    def test_scripts_use_provisioner_not_mutable_pip(self):
        for name in ('scripts/build-futo.sh', 'scripts/dict/build-en-swipe.sh'):
            text = (ROOT/name).read_text()
            self.assertIn('provision-build-deps.py', text)
            self.assertNotIn('pip" install', text)
            self.assertNotIn('--upgrade', text)

if __name__ == '__main__':
    unittest.main()
