"""Executable build inputs must be hash-locked, including cached downloads."""
import hashlib
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
