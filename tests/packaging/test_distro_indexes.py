# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""The update site's Kubuntu/Debian/openSUSE indexes are exactly what each distro's updater accepts (card #502)."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(shutil.which('gpg'), 'needs gpg')
class DistroIndexes(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.gnupg = self.tmp / 'gnupg'
        self.gnupg.mkdir(mode=0o700)
        self.env = {'GNUPGHOME': str(self.gnupg)}
        subprocess.run(['gpg', '--batch', '--quiet', '--passphrase', '', '--quick-generate-key', 'Test site', 'ed25519',
                        'sign', '1d'], check=True, capture_output=True, env={**os.environ, **self.env})
        self.site_module = load('update_site', 'scripts/update_site.py')

    def build(self, present):
        release, site = self.tmp / 'release', self.tmp / 'site'
        release.mkdir()
        site.mkdir()
        for folder, version, name in self.site_module.distro_packages('0.5.1-1'):
            if folder in present:
                (release / name).write_bytes(b'package ' + folder.encode())
        with mock.patch.dict(os.environ, self.env):
            self.site_module.build_distro_indexes(release, site, 'v0.5.1-1', '0.5.1-1', 'Test site',
                                                  'https://github.com/itsdestin/swipe-keyboard/releases/download')
        return site

    def test_each_present_package_gets_a_signed_index_its_updater_accepts(self):
        site = self.build({'deb/kubuntu-26.10', 'deb/debian-testing', 'opensuse/tumbleweed'})
        for folder, owner in (('deb/kubuntu-26.10', 'apt'), ('deb/debian-testing', 'apt'), ('opensuse/tumbleweed', 'zypper')):
            with self.subTest(folder=folder):
                index = json.loads((site / folder / 'latest.json').read_text())
                self.assertTrue((site / folder / 'latest.json.sig').is_file())
                target = folder.split('/')[1]
                (self.tmp / 'update-target').write_text(target + '\n')
                with mock.patch.dict(os.environ, {'SWIPEKB_UPDATE_TESTING': '1',
                                                  'SWIPEKB_UPDATE_TARGET_FILE': str(self.tmp / 'update-target')}):
                    updater = load('swipekb_update', 'integration/update.py')
                    how = updater.index_updater(owner)
                self.assertEqual(how['folder'], folder)
                url = index['files']['x86_64']['url']
                self.assertTrue(url.startswith(updater.RELEASE_DOWNLOADS), url)
                self.assertTrue(how['name_ok'](url.rsplit('/', 1)[1], index['version']), url)

    def test_a_release_without_these_packages_still_builds_its_site(self):
        # WHY: until the OpenSwipe rename they are not release files, and today's releases must be unaffected.
        site = self.build(set())
        self.assertFalse((site / 'deb').exists())
        self.assertFalse((site / 'opensuse').exists())


if __name__ == '__main__':
    unittest.main()
