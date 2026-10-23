"""The native updater against a local copy of the update site, a throwaway key and stand-ins for pacman and dnf."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
UPDATE = ROOT / 'integration/update.py'

# Stand-in for pacman: answers -Q with the installed version and records every -U.
PACMAN = '''#!/usr/bin/env python3
import os, sys
log = os.environ['FAKE_LOG']
if sys.argv[1:3] == ['-Q', 'swipe-keyboard']:
    print('swipe-keyboard ' + os.environ['FAKE_INSTALLED']); sys.exit(0)
with open(log, 'a') as f:
    f.write(' '.join(sys.argv[1:]) + '\\n')
sys.exit(int(os.environ.get('FAKE_PACMAN_RC', '0')))
'''
# Stand-in for vercmp (pacman's version order is enough here: dotted numbers and the -pkgrel).
VERCMP = '''#!/usr/bin/env python3
import re, sys
key = lambda v: [int(x) for x in re.split(r'[.-]', v)]
a, b = key(sys.argv[1]), key(sys.argv[2])
print((a > b) - (a < b))
'''


@unittest.skipUnless(shutil.which('gpg') and shutil.which('gpgv'), 'needs gpg and gpgv')
class ArchUpdater(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.gnupg = self.tmp / 'gnupg'
        self.gnupg.mkdir(mode=0o700)
        self.gpg('--quick-generate-key', 'Test updates', 'ed25519', 'sign', '1d')
        self.keyring = self.tmp / 'update-key.gpg'
        self.keyring.write_bytes(self.gpg('--export', capture=True))
        self.site = self.tmp / 'site'
        (self.site / 'arch').mkdir(parents=True)
        self.downloads = self.tmp / 'downloads'
        self.downloads.mkdir()
        bin_dir = self.tmp / 'bin'
        bin_dir.mkdir()
        for name, text in (('pacman', PACMAN), ('vercmp', VERCMP)):
            (bin_dir / name).write_text(text)
            (bin_dir / name).chmod(0o755)
        self.log = self.tmp / 'pacman.log'
        self.env = {
            'PATH': f'{bin_dir}:{os.environ["PATH"]}', 'FAKE_LOG': str(self.log), 'FAKE_INSTALLED': '0.4.1-1',
            'SWIPEKB_UPDATE_TESTING': '1',
            'SWIPEKB_UPDATE_SITE': self.site.as_uri(), 'SWIPEKB_UPDATE_KEYRING': str(self.keyring),
            'SWIPEKB_UPDATE_OFF_FLAG': str(self.tmp / 'etc/auto-update-off'),
            'SWIPEKB_UPDATE_DOWNLOADS': self.downloads.as_uri() + '/',
        }

    def gpg(self, *args, capture=False):
        result = subprocess.run(['gpg', '--batch', '--quiet', '--homedir', str(self.gnupg), '--passphrase', '', *args],
                                check=True, capture_output=True)
        return result.stdout if capture else None

    def publish(self, version, payload=b'package bytes', sha256=None, url=None, sign=True):
        name = f'swipe-keyboard-{version}-x86_64.pkg.tar.zst'
        (self.downloads / name).write_bytes(payload)
        index = {'schema': 1, 'package': 'swipe-keyboard', 'version': version,
                 'files': {'x86_64': {'url': url or (self.downloads / name).as_uri(),
                                      'sha256': sha256 or hashlib.sha256(payload).hexdigest(), 'size': len(payload)}}}
        path = self.site / 'arch/latest.json'
        path.write_text(json.dumps(index))
        if sign:
            self.gpg('--yes', '--detach-sign', '--output', str(path) + '.sig', str(path))
        return name

    def run_updater(self, *args):
        with mock.patch.dict(os.environ, self.env):
            spec = importlib.util.spec_from_file_location('swipekb_update', UPDATE)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            with mock.patch.object(module.platform, 'machine', return_value='x86_64'), \
                 mock.patch.object(module.os, 'geteuid', return_value=0), \
                 mock.patch.object(module.shutil, 'which', side_effect=lambda n: n == 'pacman' or shutil.which(n)):
                return module.main(list(args))

    def installs(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_a_signed_newer_package_is_checked_then_installed_with_pacman(self):
        name = self.publish('0.4.2-1')
        self.assertEqual(self.run_updater('check'), 0)
        [call] = self.installs()
        self.assertTrue(call.startswith('-U --noconfirm --needed '), call)
        self.assertTrue(call.endswith('/' + name), call)

    def test_the_same_or_an_older_version_installs_nothing(self):
        for version in ('0.4.1-1', '0.4.0-1'):
            self.publish(version)
            self.assertEqual(self.run_updater('check'), 0)
        self.assertEqual(self.installs(), [])

    def test_an_index_changed_after_signing_is_refused(self):
        self.publish('0.4.2-1')
        index = self.site / 'arch/latest.json'
        index.write_text(index.read_text().replace('0.4.2-1', '0.4.3-1'))
        self.assertEqual(self.run_updater('check'), 1)
        self.assertEqual(self.installs(), [])

    def test_an_index_signed_by_another_key_is_refused(self):
        self.publish('0.4.2-1')
        other = self.tmp / 'other'
        other.mkdir(mode=0o700)
        subprocess.run(['gpg', '--batch', '--quiet', '--homedir', str(other), '--passphrase', '',
                        '--quick-generate-key', 'Someone else', 'ed25519', 'sign', '1d'], check=True, capture_output=True)
        index = self.site / 'arch/latest.json'
        subprocess.run(['gpg', '--batch', '--quiet', '--homedir', str(other), '--yes', '--detach-sign',
                        '--output', str(index) + '.sig', str(index)], check=True, capture_output=True)
        self.assertEqual(self.run_updater('check'), 1)
        self.assertEqual(self.installs(), [])

    def test_a_download_that_differs_from_the_signed_hash_is_refused(self):
        self.publish('0.4.2-1', sha256='0' * 64)
        self.assertEqual(self.run_updater('check'), 1)
        self.assertEqual(self.installs(), [])

    def test_a_signed_index_cannot_send_the_updater_to_another_site(self):
        elsewhere = self.tmp / 'elsewhere'
        elsewhere.mkdir()
        payload = b'package bytes'
        (elsewhere / 'swipe-keyboard-0.4.2-1-x86_64.pkg.tar.zst').write_bytes(payload)
        self.publish('0.4.2-1', payload, url=(elsewhere / 'swipe-keyboard-0.4.2-1-x86_64.pkg.tar.zst').as_uri())
        self.assertEqual(self.run_updater('check'), 1)
        self.assertEqual(self.installs(), [])

    def test_turned_off_means_nothing_is_fetched_or_installed(self):
        self.publish('0.4.2-1')
        self.assertEqual(self.run_updater('off'), 0)
        self.assertTrue(Path(self.env['SWIPEKB_UPDATE_OFF_FLAG']).is_file())
        self.assertEqual(self.run_updater('check'), 0)
        self.assertEqual(self.installs(), [])
        self.assertEqual(self.run_updater('on'), 0)
        self.assertEqual(self.run_updater('check'), 0)
        self.assertEqual(len(self.installs()), 1)

    def test_an_unreachable_site_is_not_an_error_and_installs_nothing(self):
        self.env['SWIPEKB_UPDATE_SITE'] = 'http://127.0.0.1:9'   # nothing listens on the discard port
        self.assertEqual(self.run_updater('check'), 0)
        self.assertEqual(self.installs(), [])

    def test_a_malformed_but_signed_index_is_refused_without_a_crash(self):
        self.publish('0.4.2-1')
        index = self.site / 'arch/latest.json'
        data = json.loads(index.read_text())
        data['files']['x86_64']['size'] = 'big'
        index.write_text(json.dumps(data))
        self.gpg('--yes', '--detach-sign', '--output', str(index) + '.sig', str(index))
        self.assertEqual(self.run_updater('check'), 1)
        self.assertEqual(self.installs(), [])

    def test_outside_test_mode_the_environment_cannot_redirect_the_root_updater(self):
        with mock.patch.dict(os.environ, {**self.env, 'SWIPEKB_UPDATE_TESTING': ''}):
            spec = importlib.util.spec_from_file_location('swipekb_update_plain', UPDATE)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        self.assertEqual(module.SITE, 'https://itsdestin.github.io/swipe-keyboard')
        self.assertEqual(module.KEYRING, UPDATE.parent / 'update-key.gpg')
        self.assertEqual(module.RELEASE_DOWNLOADS, 'https://github.com/itsdestin/swipe-keyboard/releases/download/')

    def test_a_failed_pacman_run_is_reported(self):
        self.publish('0.4.2-1')
        self.env['FAKE_PACMAN_RC'] = '1'
        self.assertEqual(self.run_updater('check'), 1)


class Units(unittest.TestCase):
    def test_the_daily_timer_runs_the_installed_updater_unless_turned_off(self):
        service = (ROOT / 'packaging/updates/swipe-keyboard-update.service').read_text()
        self.assertIn('ExecStart=/usr/bin/python3 -I /usr/share/swipe-keyboard/integration/update.py check', service)
        self.assertIn('TimeoutStartSec=', service)
        self.assertIn('ConditionPathExists=!/etc/swipe-keyboard/auto-update-off', service)
        timer = (ROOT / 'packaging/updates/swipe-keyboard-update.timer').read_text()
        self.assertIn('Persistent=true', timer)
        self.assertIn('OnCalendar=daily', timer)
        self.assertNotIn('OnUnitActiveSec', timer)   # would stop for good after an "off" day
        self.assertIn('RandomizedDelaySec=', timer)

    def test_the_fedora_repository_requires_a_signed_index_and_never_blocks_other_updates(self):
        repo = (ROOT / 'packaging/updates/swipe-keyboard.repo').read_text()
        for line in ('repo_gpgcheck=1', 'skip_if_unavailable=1',
                     'baseurl=https://itsdestin.github.io/swipe-keyboard/fedora/$basearch/',
                     'gpgkey=file:///usr/share/swipe-keyboard/integration/update-key.asc'):
            self.assertIn(line, repo)

    def test_the_shipped_key_files_are_the_same_key(self):
        if not shutil.which('gpg'):
            self.skipTest('needs gpg')
        fingerprints = []
        for name in ('update-key.asc', 'update-key.gpg'):
            out = subprocess.run(['gpg', '--batch', '--with-colons', '--show-keys', str(ROOT / 'packaging/updates' / name)],
                                 check=True, text=True, capture_output=True).stdout
            fingerprints.append([line.split(':')[9] for line in out.splitlines() if line.startswith('fpr')][0])
        self.assertEqual(fingerprints[0], fingerprints[1])

    def test_the_wrapper_sends_updates_to_the_updater_and_everything_else_to_setup(self):
        wrapper = (ROOT / 'integration/swipe-keyboard').read_text()
        self.assertIn('exec python3 -I /usr/share/swipe-keyboard/integration/update.py "$@"', wrapper)
        self.assertIn('exec python3 /usr/share/swipe-keyboard/integration/setup.py "$@"', wrapper)


if __name__ == '__main__':
    unittest.main()
