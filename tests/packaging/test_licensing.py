# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Licensing contract (REUSE + owner decision 2026-10-23, "MIT where possible").

MIT everywhere except the keyboard add-on layers that plug into GPL code (plugin/: Qt Virtual Keyboard;
frontend-plasma69/: inside plasma-keyboard) and KDE's own code in scripts/patches/, which use KDE's standard
GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL. CMake files are MIT everywhere.

WHY a plain-Python check as well as `reuse lint`: it needs no extra tool, so it runs in every test job and catches a
new file without a header (or with the wrong licence) the moment it is added. `reuse lint` itself runs when installed.
"""
import pathlib
import re
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
KDE_GPL = 'GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL'
CODE = re.compile(r'(\.(cpp|h|hpp|qml|py|sh|js|bats)|CMakeLists\.txt|\.cmake)$')
GPL_FOLDERS = ('plugin/', 'frontend-plasma69/', 'scripts/patches/')
# REUSE-IgnoreStart (a pattern, not a licence)
LICENCE = re.compile(r'SPDX-License-Identifier: (.+?)\s*(\*/)?$', re.M)
# REUSE-IgnoreEnd


def tracked():
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files'], check=True, capture_output=True, text=True).stdout
    return [p for p in out.splitlines() if (ROOT/p).is_file()]


def header_licence(path):
    head = '\n'.join((ROOT/path).read_text(errors='replace').splitlines()[:10])
    found = LICENCE.search(head)
    return found.group(1).strip() if found else None


def shared_plugin_files():
    """The plugin/ sources that Gaming Mode, the tools and the 6.9 front end also build: MIT (owner decision 2026-10-23).

    WHY read from plugin/CMakeLists.txt, not a hand list: a file added to these libraries later must be MIT too, or the
    daemon and tools would quietly become GPL as built again."""
    cmake = (ROOT/'plugin/CMakeLists.txt').read_text()
    names = set()
    # WHY swipekb_swapflow too: the swap bench (tools/, MIT) builds it, like the two libraries before it.
    for lib in ('swipekb_language', 'swipekb_session', 'swipekb_swapflow'):
        sources = re.search(r'add_library\(' + lib + r' STATIC ([^)]*)\)', cmake).group(1).split()
        names |= {pathlib.Path(s).stem for s in sources if s.endswith('.cpp') and not s.startswith('../')}
    names |= {'nextwordworker', 'learningmanager'}   # built into frontend-plasma69's core and the feature tests
    return {f'plugin/{n}{ext}' for n in names for ext in ('.cpp', '.h') if (ROOT/f'plugin/{n}{ext}').exists()}


SHARED = shared_plugin_files()


def wanted(path):
    if path.endswith('CMakeLists.txt') or path.endswith('.cmake') or path in SHARED:
        return 'MIT'
    return KDE_GPL if path.startswith(GPL_FOLDERS) else 'MIT'


class Licensing(unittest.TestCase):
    def setUp(self):
        if not (ROOT/'.git').exists():
            self.skipTest('needs a git checkout (file list)')

    def test_every_source_file_has_the_licence_of_its_part(self):
        wrong = []
        for path in tracked():
            if not CODE.search(path) or path.startswith('tests/gaming/fixtures/'):
                continue
            if header_licence(path) != wanted(path):
                wrong.append(f'{path}: {header_licence(path)!r} (want {wanted(path)!r})')
        self.assertEqual(wrong, [], 'SPDX header missing or wrong licence')

    def test_the_split_is_mit_except_the_keyboard_add_on_layers(self):
        # Spot checks of the decision itself, so a bulk edit cannot quietly flip a whole part.
        for path in ('engine/src/language_engine.cpp', 'engine/src/learning.cpp', 'tools/swipekb-bench.cpp',
                     'scripts/fetch-assets.py', 'integration/setup.py', 'integration/plasma69.py', 'gaming/daemon/server.cpp',
                     'plugin/languageprofile.cpp', 'plugin/swipesession.h', 'plugin/learningmanager.cpp',
                     'CMakeLists.txt', 'plugin/CMakeLists.txt', 'frontend-plasma69/CMakeLists.txt'):
            self.assertEqual(header_licence(path), 'MIT', path)
        for path in ('plugin/swipeinputmethod.cpp', 'plugin/SwipeCapture.qml', 'frontend-plasma69/src/swipecontroller.cpp',
                     'frontend-plasma69/qml/SwipeLayer.qml', 'frontend-plasma69/tests/harness/tst_e2e.cpp',
                     'scripts/patches/backendpredictionpolicy.h'):
            self.assertEqual(header_licence(path), KDE_GPL, path)
        toml = (ROOT/'REUSE.toml').read_text()
        patch = toml[toml.index('scripts/patches/plasma-prediction-policy.patch'):]
        self.assertIn(KDE_GPL, patch.split('[[annotations]]')[0])

    def test_gaming_mode_and_tools_build_only_mit_plugin_code(self):
        # WHY: these programs link swipekb_language / swipekb_session; if any of their sources were GPL, the programs
        # would be GPL as built (LICENSE says they are MIT).
        self.assertGreaterEqual(len(SHARED), 20)
        for path in SHARED:
            self.assertEqual(header_licence(path), 'MIT', path)

    def test_licences_folder_holds_exactly_the_licences_in_use(self):
        names = {p.stem for p in (ROOT/'LICENSES').glob('*.txt')}
        self.assertEqual(names, {'MIT', 'GPL-2.0-only', 'GPL-3.0-only', 'LicenseRef-KDE-Accepted-GPL', 'Apache-2.0',
                                 'CC-BY-4.0', 'CC0-1.0', 'ODC-By-1.0'})

    def test_reuse_lint_passes_when_the_tool_is_installed(self):
        tool = shutil.which('reuse')
        if not tool:
            self.skipTest('reuse not installed')
        result = subprocess.run([tool, '--root', str(ROOT), 'lint'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])


if __name__ == '__main__':
    unittest.main()
