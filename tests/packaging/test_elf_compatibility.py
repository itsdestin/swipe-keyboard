"""Exercise compatibility checks on real ELF inputs, never recursive ldd fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ElfCompatibility(unittest.TestCase):
    def test_direct_dependencies_ignore_recursive_and_injected_libraries(self):
        generator = load('arch-dependencies')
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            module = stage / 'usr/share/swipe-keyboard'
            (module / 'qml/SwipeKeyboard').mkdir(parents=True)
            (module / 'runtime.json').write_text(json.dumps({'qt': '6.11'}))
            for name in ('one.so', 'two.so'):
                (module / 'qml/SwipeKeyboard' / name).write_bytes(b'\x7fELF')
            def command(args):
                if args[0] == 'readelf':
                    if '--dynamic' in args:
                        return ('  (NEEDED) Shared library: [libQt6Core.so.6]\n'
                                '  (NEEDED) Shared library: [libc.so.6]\n'
                                '  (NEEDED) Shared library: [libstdc++.so.6]\n')
                    return ('Version needs section\nName: GLIBC_2.43\nName: GLIBCXX_3.4.32\n'
                            'Name: CXXABI_1.3.11\n')
                if args[0] == 'ldconfig':
                    return '\n'.join(f'{lib} (libc6,x86-64) => /usr/lib/{lib}' for lib in
                                     ('libQt6Core.so.6', 'libc.so.6', 'libstdc++.so.6', 'libfakeroot.so', 'libicuuc.so.78'))
                if args[:2] == ['pacman', '-Qoq']:
                    return {'libQt6Core.so.6': 'qt6-base', 'libc.so.6': 'glibc', 'libstdc++.so.6': 'gcc-libs'}[Path(args[2]).name]
                self.fail(f'Unexpected command (no recursive ldd or package release floors): {args}')
            dependencies = generator.dependencies(stage, command)
            self.assertEqual(dependencies, ['gcc-libs>=13.2', 'glibc>=2.43', 'qt6-base>=6.11'])
            def split_owner(args):
                if args[:2] == ['pacman', '-Qoq'] and Path(args[2]).name == 'libstdc++.so.6':
                    return 'libstdc++'
                return command(args)
            self.assertEqual(generator.dependencies(stage, split_owner),
                             ['glibc>=2.43', 'libstdc++>=13.2', 'qt6-base>=6.11'])
            def missing_provider(args):
                return '' if args[0] == 'ldconfig' else command(args)
            with self.assertRaisesRegex(ValueError, 'Unresolved direct ELF dependency'):
                generator.dependencies(stage, missing_provider)

    def test_config_helper_contributes_direct_needs_and_version_floors(self):
        generator = load('arch-dependencies')
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            base = stage / 'usr/share/swipe-keyboard'
            (base / 'qml/SwipeKeyboard').mkdir(parents=True)
            (base / 'integration').mkdir()
            (base / 'runtime.json').write_text('{"qt": "6.11"}')
            for name in ('qml/SwipeKeyboard/one.so', 'integration/swipekb-preflight', 'integration/swipekb-config'):
                (base / name).write_bytes(b'ELF')
            calls = []
            def command(args):
                calls.append(args)
                if args[0] == 'readelf':
                    helper = Path(args[-1]).name == 'swipekb-config'
                    if '--dynamic' in args:
                        names = ('libKF6ConfigCore.so.6', 'libc.so.6', 'libstdc++.so.6') if helper else ('libc.so.6',)
                        return '\n'.join(f'(NEEDED) Shared library: [{name}]' for name in names)
                    return 'Version needs section\nName: GLIBC_2.44\nName: GLIBCXX_3.4.34\n' if helper else 'Version needs section\nName: GLIBC_2.40\n'
                if args[0] == 'ldconfig':
                    return '\n'.join(f'{name} (libc6,x86-64) => /usr/lib/{name}' for name in
                                     ('libKF6ConfigCore.so.6', 'libc.so.6', 'libstdc++.so.6'))
                if args[:2] == ['pacman', '-Qoq']:
                    return {'libKF6ConfigCore.so.6': 'kconfig', 'libc.so.6': 'glibc', 'libstdc++.so.6': 'gcc-libs'}[Path(args[-1]).name]
                self.fail(args)
            self.assertEqual(generator.dependencies(stage, command),
                             ['gcc-libs>=15.1', 'glibc>=2.44', 'kconfig'])
            for mode in ('--dynamic', '--version-info'):
                self.assertTrue(any(mode in call and call[-1].endswith('/swipekb-config') for call in calls))

    def test_cxx_symbol_floors_and_unknown_future_requirements(self):
        generator = load('arch-dependencies')
        self.assertEqual(generator.gcc_floor({'GLIBCXX_3.4.31', 'CXXABI_1.3.17'}), '16.1')
        self.assertEqual(generator.gcc_floor({'GLIBCXX_3.4.34'}), '15.1')
        with self.assertRaisesRegex(ValueError, 'Unknown C\\+\\+ ABI requirement'):
            generator.gcc_floor({'GLIBCXX_3.4.999'})

    def test_riscv_only_cxxabi_tag_refuses_on_supported_x86_64(self):
        generator = load('arch-dependencies')
        with self.assertRaisesRegex(ValueError, 'RISC-V-only'):
            generator.gcc_floor({'CXXABI_1.3.16'})

    def test_private_qt_imports_rejected_in_final_elf(self):
        checker = load('package-check')
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'lib.c').write_text('extern void private_call(void); void entry(void) { private_call(); }')
            (p / 'provider.c').write_text('void private_call(void) {}')
            (p / 'versions').write_text('Qt_6.11_PRIVATE_API { global: private_call; };')
            subprocess.run(['cc', '-shared', '-fPIC', str(p/'provider.c'), '-Wl,--version-script='+str(p/'versions'), '-o', str(p/'provider.so')], check=True)
            subprocess.run(['cc', '-shared', '-fPIC', str(p/'lib.c'), str(p/'provider.so'), '-o', str(p/'consumer.so')], check=True)
            with self.assertRaisesRegex(ValueError, 'private Qt ABI'):
                checker.check_public_qt_abi(p/'consumer.so')
            checker.check_public_qt_abi(p/'provider.so')  # Definitions are not imports.

    def test_real_preflight_rejects_missing_library_and_unresolved_symbol(self):
        helper = Path(os.environ.get('SWIPEKB_TEST_PREFLIGHT', ROOT/'build/plugin/swipekb-preflight'))
        if not helper.is_file():
            self.skipTest('Build the native preflight helper first')
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(loader_probe LANGUAGES CXX)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
set(CMAKE_AUTOMOC ON)
add_library(provider SHARED provider.cpp)
add_library(probe MODULE probe.cpp)
target_link_libraries(probe PRIVATE Qt6::Core provider)
set_target_properties(probe PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
''')
            (p/'provider.cpp').write_text('extern "C" void required_symbol() {}')
            (p/'probe.cpp').write_text('''#include <QObject>
extern "C" void required_symbol();
extern "C" void use_required_symbol() { required_symbol(); }
class Probe : public QObject { Q_OBJECT Q_PLUGIN_METADATA(IID "test.probe") };
#include "probe.moc"
''')
            subprocess.run(['cmake', '-S', str(p), '-B', str(p/'build')], check=True, capture_output=True)
            subprocess.run(['cmake', '--build', str(p/'build'), '-j2'], check=True, capture_output=True)
            args = [str(helper), str(p/'build/libprobe.so')]
            result = subprocess.run(args, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            provider = p/'build/libprovider.so'
            provider.rename(p/'hidden.so')
            result = subprocess.run(args, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('libprovider.so', result.stderr)
            (p/'provider.cpp').write_text('extern "C" void unrelated_symbol() {}')
            subprocess.run(['c++', '-shared', '-fPIC', str(p/'provider.cpp'), '-o', str(provider)], check=True)
            result = subprocess.run(args, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('required_symbol', result.stderr)

    def test_no_cachegen_is_required_for_public_qt_module(self):
        self.assertIn('NO_CACHEGEN', (ROOT/'plugin/CMakeLists.txt').read_text())


if __name__ == '__main__':
    unittest.main()
