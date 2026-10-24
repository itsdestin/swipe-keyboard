# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Manifest parsing and verified downloads from Valve's public update server (no network in tests)."""
import hashlib
import io
import tempfile
import unittest
from pathlib import Path

from support import BASE, FIXTURES, FakeServer, vdf
import fetch

class Manifest(unittest.TestCase):
    def test_real_manifest_parses(self):
        m = fetch.parse_manifest((FIXTURES / 'manifest-synthetic.vdf').read_text())
        self.assertEqual(m.version, '1792618285')
        info = m.package('steamui_websrc_all')
        self.assertEqual(info.file, 'steamui_websrc_all.zip.24dd80531ce5c8cf19b5bfdd2e580ab55ebf8365')
        self.assertEqual(info.size, 35647879)
        self.assertEqual(info.sha1, '24dd80531ce5c8cf19b5bfdd2e580ab55ebf8365')
        self.assertRegex(info.sha256, r'^[0-9a-f]{64}$')
        self.assertEqual(m.package('bins_ubuntu12').size, 76110471)

    def test_missing_package_and_garbage_are_errors(self):
        m = fetch.parse_manifest((FIXTURES / 'manifest-synthetic.vdf').read_text())
        with self.assertRaises(fetch.FetchError):
            m.package('no_such_package')
        for bad in ('', 'not a manifest', '"a" { "version"', '{}'):
            with self.subTest(bad=bad), self.assertRaises(fetch.FetchError):
                fetch.parse_manifest(bad)

    def test_manifest_without_version_is_an_error(self):
        with self.assertRaises(fetch.FetchError):
            fetch.parse_manifest(vdf('', {}).replace('"version"\t\t""', ''))

    def test_hostile_file_names_are_refused_before_any_url_is_built(self):
        for bad in ('../../etc/passwd', 'steamui_websrc_all.zip.zz', 'other.zip.' + 'a' * 40,
                    'steamui_websrc_all.zip.' + 'a' * 40 + '/../x', 'steamui_websrc_all.zip.' + 'A' * 40):
            text = vdf('1', {'steamui_websrc_all': (bad, 5, 'b' * 64)})
            with self.subTest(bad=bad), self.assertRaises(fetch.FetchError):
                fetch.parse_manifest(text).package('steamui_websrc_all')


class Download(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(self.dir, ignore_errors=True))
        self.data = b'zipbytes' * 100
        self.sha1 = hashlib.sha1(self.data).hexdigest()
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        self.name = f'steamui_websrc_all.zip.{self.sha1}'
        self.info = fetch.PackageInfo('steamui_websrc_all', self.name, len(self.data), self.sha1, self.sha256)

    def fetcher(self, files, **kw):
        server = FakeServer(files)
        return fetch.Fetcher(BASE, opener=server, sleep=lambda s: None, **kw), server

    def test_good_download_verifies_and_saves(self):
        f, server = self.fetcher({self.name: self.data})
        out = f.download_package(self.info, self.dir / 'p.zip', max_bytes=10_000)
        self.assertEqual(out.read_bytes(), self.data)
        self.assertEqual(server.calls, [BASE + self.name])

    def test_sha1_mismatch_is_refused_and_file_removed(self):
        f, _ = self.fetcher({self.name: b'z' * len(self.data)})
        with self.assertRaises(fetch.FetchError):
            f.download_package(self.info, self.dir / 'p.zip', max_bytes=10_000)
        self.assertFalse((self.dir / 'p.zip').exists())

    def test_sha256_mismatch_is_refused(self):
        bad = fetch.PackageInfo('steamui_websrc_all', self.name, len(self.data), self.sha1, '0' * 64)
        f, _ = self.fetcher({self.name: self.data})
        with self.assertRaises(fetch.FetchError):
            f.download_package(bad, self.dir / 'p.zip', max_bytes=10_000)

    def test_size_over_cap_is_refused_while_streaming(self):
        f, _ = self.fetcher({self.name: self.data})
        with self.assertRaises(fetch.FetchError):
            f.download_package(self.info, self.dir / 'p.zip', max_bytes=100)
        self.assertFalse((self.dir / 'p.zip').exists())

    def test_size_differing_from_manifest_is_refused(self):
        wrong = fetch.PackageInfo('steamui_websrc_all', self.name, 5, self.sha1, self.sha256)
        f, _ = self.fetcher({self.name: self.data})
        with self.assertRaises(fetch.FetchError):
            f.download_package(wrong, self.dir / 'p.zip', max_bytes=10_000)

    def test_transient_errors_are_retried_then_give_up(self):
        calls = {'n': 0}

        class Flaky(FakeServer):
            def __call__(s, req, timeout=None):
                calls['n'] += 1
                if calls['n'] < 3:
                    raise OSError('connection reset')
                return super().__call__(req, timeout)

        server = Flaky({self.name: self.data})
        f = fetch.Fetcher(BASE, opener=server, sleep=lambda s: None, retries=3)
        f.download_package(self.info, self.dir / 'p.zip', max_bytes=10_000)
        self.assertEqual(calls['n'], 3)
        always = fetch.Fetcher(BASE, opener=FakeServer({self.name: OSError('down')}), sleep=lambda s: None, retries=2)
        with self.assertRaises(fetch.FetchError):
            always.download_package(self.info, self.dir / 'q.zip', max_bytes=10_000)

    def test_http_404_is_not_retried(self):
        server = FakeServer({})
        f = fetch.Fetcher(BASE, opener=server, sleep=lambda s: None, retries=3)
        with self.assertRaises(fetch.FetchError):
            f.download_package(self.info, self.dir / 'p.zip', max_bytes=10_000)
        self.assertEqual(len(server.calls), 1)

    def test_manifest_fetch_is_size_capped_and_parsed(self):
        text = (FIXTURES / 'manifest-synthetic.vdf').read_bytes()
        f, _ = self.fetcher({'steam_client_x': text})
        self.assertEqual(f.fetch_manifest('steam_client_x', max_bytes=100_000).version, '1792618285')
        with self.assertRaises(fetch.FetchError):
            f.fetch_manifest('steam_client_x', max_bytes=100)

    def test_only_https_base_and_clean_manifest_names(self):
        with self.assertRaises(ValueError):
            fetch.Fetcher('http://client-update.steamstatic.com/')
        f, _ = self.fetcher({})
        with self.assertRaises(fetch.FetchError):
            f.fetch_manifest('../secret', max_bytes=10)


if __name__ == '__main__':
    unittest.main()
