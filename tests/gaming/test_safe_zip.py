# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Valve's zips are untrusted input: extraction must refuse anything that could escape or exhaust."""
import stat
import tempfile
import unittest
import zipfile
from pathlib import Path

from support import make_zip
import safe_zip


class SafeZip(unittest.TestCase):
    def extract(self, data, **kw):
        # WHY two sibling temp dirs: lets tests prove nothing landed OUTSIDE the destination.
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp, ignore_errors=True))
        zpath = self.tmp / 'in.zip'
        zpath.write_bytes(data)
        dest = self.tmp / 'out'
        dest.mkdir()
        return safe_zip.safe_extract(zpath, dest, **kw), dest

    def test_ordinary_files_extract(self):
        _, dest = self.extract(make_zip([('steamui/sp.js', b'abc'), ('steamui/a/b.txt', b'x')]))
        self.assertEqual((dest / 'steamui/sp.js').read_bytes(), b'abc')
        self.assertEqual((dest / 'steamui/a/b.txt').read_bytes(), b'x')

    def test_backslash_names_become_real_directories(self):
        # Valve's real zips contain 'steamui\\css/' style entries and 'steamui\\x.js' files.
        _, dest = self.extract(make_zip([('steamui\\css/', b''), ('steamui\\css\\a.css', b'body{}')]))
        self.assertEqual((dest / 'steamui/css/a.css').read_bytes(), b'body{}')
        self.assertFalse(any('\\' in p.name for p in dest.rglob('*')))

    def test_parent_traversal_is_refused_and_nothing_written(self):
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(make_zip([('ok.txt', b'1'), ('../evil.txt', b'pwn')]))
        self.assertFalse((self.tmp / 'evil.txt').exists())
        self.assertEqual(list((self.tmp / 'out').iterdir()), [], 'validation must happen before any write')

    def test_backslash_traversal_is_refused(self):
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(make_zip([('..\\..\\evil.txt', b'pwn')]))

    def test_mixed_traversal_is_refused(self):
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(make_zip([('steamui/..\\..\\evil.txt', b'pwn')]))

    def test_absolute_and_drive_paths_are_refused(self):
        for name in ('/etc/passwd', '\\windows\\x', 'C:\\x.txt', 'C:/x.txt'):
            with self.subTest(name=name), self.assertRaises(safe_zip.UnsafeArchive):
                self.extract(make_zip([(name, b'x')]))

    def test_nul_and_empty_names_are_refused(self):
        # WHY no NUL case: Python's zipfile itself cuts names at a NUL, so it cannot reach us; the
        # check in clean_name() stays as a second line of defence.
        for name in ('', '.', 'a//b/..'):
            with self.subTest(name=repr(name)), self.assertRaises(safe_zip.UnsafeArchive):
                self.extract(make_zip([(name, b'x')]))

    def test_symlink_entries_are_refused(self):
        info = zipfile.ZipInfo('link')
        info.create_system = 3
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(make_zip([(info, b'/etc/passwd')]))

    def test_names_that_collide_after_cleaning_are_refused(self):
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(make_zip([('a/b.js', b'1'), ('a\\b.js', b'2')]))

    def test_file_under_a_file_is_refused(self):
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(make_zip([('a', b'1'), ('a/b', b'2')]))

    def test_size_limits(self):
        data = make_zip([('big.bin', b'0' * 5000)])
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(data, max_entry_bytes=1000)
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(data, max_total_bytes=1000)
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(make_zip([(f'f{i}', b'x') for i in range(5)]), max_entries=3)

    def test_zip_bomb_ratio_is_refused(self):
        data = make_zip([('bomb.bin', b'\0' * (3 * 1024 * 1024))])
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(data, max_ratio=100)

    def test_lying_header_cannot_exceed_streamed_limit(self):
        # A header that claims 10 bytes but really inflates to more must still be stopped.
        data = bytearray(make_zip([('liar.bin', b'A' * 4000)]))
        # patch uncompressed size in local header (offset 22) and central directory to 10
        for sig in (b'PK\x03\x04', b'PK\x01\x02'):
            i = data.find(sig)
            off = 22 if sig == b'PK\x03\x04' else 24
            data[i + off:i + off + 4] = (10).to_bytes(4, 'little')
        with self.assertRaises((safe_zip.UnsafeArchive, zipfile.BadZipFile)):
            self.extract(bytes(data))

    def test_only_filter_writes_just_wanted_files_but_still_validates_all(self):
        _, dest = self.extract(make_zip([('a/keep.so', b'1'), ('b/skip.so', b'2')]), only={'a/keep.so'})
        self.assertTrue((dest / 'a/keep.so').exists())
        self.assertFalse((dest / 'b/skip.so').exists())
        with self.assertRaises(safe_zip.UnsafeArchive):
            self.extract(make_zip([('a/keep.so', b'1'), ('../x', b'2')]), only={'a/keep.so'})

    def test_extracted_files_are_not_executable(self):
        info = zipfile.ZipInfo('run.sh')
        info.create_system = 3
        info.external_attr = (stat.S_IFREG | 0o755) << 16
        _, dest = self.extract(make_zip([(info, b'#!/bin/sh')]))
        self.assertEqual((dest / 'run.sh').stat().st_mode & 0o111, 0)


if __name__ == '__main__':
    unittest.main()
