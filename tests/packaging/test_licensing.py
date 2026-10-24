# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Licensing contract (KDE policy + REUSE): every source file names its licence, the engine is LGPL, the rest GPL.

WHY a plain-Python check as well as `reuse lint`: it needs no extra tool, so it runs in every test job and catches a
new file without a header (or with the wrong licence family) the moment it is added. `reuse lint` itself runs when the
tool is installed.
"""
import pathlib
import re
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GPL = 'GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL'
LGPL = 'LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL'
CODE = re.compile(r'\.(cpp|h|hpp|qml|py|sh|js|bats)$')
# REUSE-IgnoreStart (a pattern, not a licence)
LICENCE = re.compile(r'SPDX-License-Identifier: (.+?)\s*(\*/)?$', re.M)
# REUSE-IgnoreEnd


def tracked():
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files'], check=True, capture_output=True, text=True).stdout
    return [p for p in out.splitlines() if (ROOT/p).is_file()]


def header_licence(path):
    head = '\n'.join((ROOT/path).read_text(errors='replace').splitlines()[:8])
    found = LICENCE.search(head)
    return found.group(1).strip() if found else None


class Licensing(unittest.TestCase):
    def setUp(self):
        if not (ROOT/'.git').exists():
            self.skipTest('needs a git checkout (file list)')

    def test_every_source_file_has_the_kde_licence_for_its_part(self):
        wrong = []
        for path in tracked():
            if not CODE.search(path) or path.startswith('tests/gaming/fixtures/'):
                continue
            want = LGPL if path.startswith('engine/') else GPL
            if header_licence(path) != want:
                wrong.append(f'{path}: {header_licence(path)!r}')
        self.assertEqual(wrong, [], 'SPDX header missing or wrong licence family')

    def test_cmake_files_are_bsd(self):
        for path in tracked():
            if path.endswith('CMakeLists.txt') or path.endswith('.cmake'):
                self.assertEqual(header_licence(path), 'BSD-3-Clause', path)

    def test_licences_folder_holds_the_kde_texts(self):
        names = {p.stem for p in (ROOT/'LICENSES').glob('*.txt')}
        for name in ('GPL-2.0-only', 'GPL-3.0-only', 'LicenseRef-KDE-Accepted-GPL', 'LGPL-2.1-only', 'LGPL-3.0-only',
                     'LicenseRef-KDE-Accepted-LGPL', 'BSD-3-Clause', 'CC-BY-SA-4.0', 'CC0-1.0'):
            self.assertIn(name, names)
        self.assertNotIn('GPL-3.0-or-later', names)

    def test_reuse_lint_passes_when_the_tool_is_installed(self):
        tool = shutil.which('reuse')
        if not tool:
            self.skipTest('reuse not installed')
        result = subprocess.run([tool, '--root', str(ROOT), 'lint'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])


if __name__ == '__main__':
    unittest.main()
