"""D1 bundle contracts: a build candidate, never native host activation."""
import json
import importlib.util
import tempfile
import subprocess
from unittest import mock
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT / 'packaging/flatpak'


class FlatpakContractTests(unittest.TestCase):
    def manifest(self):
        return json.loads((BUNDLE / 'org.itsdestin.SwipeKeyboard.json').read_text())

    def builder_command(self, **options):
        spec = importlib.util.spec_from_file_location('flatpak_build', BUNDLE / 'build.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.builder_command('build', 'manifest.json', 'repo', **options)

    def test_builder_flags_are_cold_by_default_and_cache_only_on_request(self):
        plain = self.builder_command()
        self.assertNotIn('--ccache', plain)
        self.assertNotIn('--stop-at=swipe-keyboard', plain)
        self.assertIn('--repo=repo', plain)
        for flag in ('--force-clean', '--disable-rofiles-fuse'):
            self.assertIn(flag, plain)
        self.assertIn('--ccache', self.builder_command(ccache=True))
        deps = self.builder_command(dependencies_only=True)
        self.assertIn('--stop-at=swipe-keyboard', deps)
        # WHY: a dependency-only run exports nothing, so nothing of the product can be saved.
        self.assertFalse([a for a in deps if a.startswith('--repo')])

    def test_ccache_is_enabled_only_when_a_persistent_store_is_supplied(self):
        # WHY: the cache covers the keyboard's own objects on non-release runs. Release builds get no CCACHE_DIR, so none of this activates.
        script = (BUNDLE / 'build-swipe.sh').read_text()
        guard = script.index('if [ -n "${CCACHE_DIR:-}" ]; then')
        self.assertLess(guard, script.index('cmake -S . -B build-flatpak'))
        self.assertIn('CMAKE_CXX_COMPILER_LAUNCHER=ccache', script)
        self.assertNotIn('CCACHE_DISABLE', script)
        # Language-asset QRC is always compiled fresh.
        plugin = (ROOT / 'plugin/CMakeLists.txt').read_text()
        self.assertIn('set_target_properties(swipekb_language PROPERTIES C_COMPILER_LAUNCHER "" CXX_COMPILER_LAUNCHER "")', plugin)

    def test_identity_sdk_prefix_and_parallel_bound(self):
        manifest = self.manifest()
        self.assertEqual(manifest['id'], 'org.itsdestin.SwipeKeyboard')
        self.assertEqual(manifest['runtime'], 'org.kde.Platform')
        self.assertEqual(manifest['sdk'], 'org.kde.Sdk')
        self.assertEqual(manifest['runtime-version'], '6.10')
        # WHY 4 (was 2): hosted runners have 4 CPUs/16 GB; the audit approved a bounded 2->4 benchmark.
        # Every layer must agree so nested make/cmake never exceeds this one bound.
        self.assertEqual(manifest['build-options']['env']['CMAKE_BUILD_PARALLEL_LEVEL'], '4')
        self.assertEqual(manifest['build-options']['env']['MAKEFLAGS'], '-j4')
        script = (BUNDLE / 'build-swipe.sh').read_text()
        self.assertIn('CMAKE_BUILD_PARALLEL_LEVEL=4', script)
        self.assertNotIn('-j2', script)
        self.assertNotIn('JOBS=2', script)
        self.assertIn('--jobs=4', ' '.join(self.builder_command()))
        self.assertEqual(manifest['command'], 'swipe-keyboard-flatpak')
        self.assertEqual(manifest['build-options']['prefix'], '/app')

    def test_runtime_permissions_are_wayland_pair_plus_gaming_socket_dir(self):
        # WHY: Flatpak 1.16.6 run-sockets.c gates inherited FD handling on
        # SOCKET_WAYLAND, too. Devices, IPC, host shell and home are not needed.
        # KDE runtime metadata grants styling config/bus access by default;
        # the package must explicitly subtract those unneeded privileges.
        # Gaming Mode adds exactly one thing: the shared socket directory the engine daemon
        # (swipekb-gamingd) and the separate helper app talk through. Still no network
        # (test_flatpak_gaming.py enforces that and the helper's side).
        self.assertEqual(self.manifest()['finish-args'], [
            '--socket=wayland', '--socket=inherit-wayland-socket',
            '--filesystem=xdg-run/swipekb:create',
            '--nofilesystem=xdg-config/kdeglobals',
            '--no-talk-name=com.canonical.AppMenu.Registrar',
            '--no-talk-name=org.kde.kconfig.notify',
            '--no-talk-name=org.kde.KGlobalSettings'])

    def test_backend_policy_bridge_is_packaged(self):
        # WHY: stock backend hints cannot authorize contextual prediction; the final bundled
        # backend must compile the reviewed raw content-purpose policy bridge as well.
        backend = next(m for m in self.manifest()['modules'] if m['name'] == 'plasma-keyboard')
        self.assertIn({'type': 'patch', 'path': '../../scripts/patches/plasma-prediction-policy.patch'}, backend['sources'])
        self.assertIn({'type': 'file', 'path': '../../scripts/patches/backendpredictionpolicy.h', 'dest': 'src'}, backend['sources'])

    def test_upstream_sources_are_hash_pinned(self):
        modules = self.manifest()['modules']
        self.assertEqual([module['name'] for module in modules[:3]],
                         ['qt6-virtualkeyboard', 'libplasma', 'plasma-keyboard'])
        for module in modules[:3]:
            for source in module['sources']:
                if source['type'] != 'archive':
                    # WHY local reviewed bridge inputs supplement, never replace/unpin the backend archive.
                    self.assertEqual(module['name'], 'plasma-keyboard')
                    self.assertIn((source['type'], source['path']), {
                        ('patch', '../../scripts/patches/plasma-prediction-policy.patch'),
                        ('file', '../../scripts/patches/backendpredictionpolicy.h')})
                    self.assertTrue((BUNDLE / source['path']).is_file())
                    self.assertNotIn('url', source)
                    continue
                self.assertRegex(source['sha256'], r'^[a-f0-9]{64}$')
                self.assertNotIn('master', source['url'])
                self.assertNotIn('Plasma/6.7', source['url'])
        self.assertIn('e54f5a9d95de54c0147911de009dc12b2d711757', modules[2]['sources'][0]['url'])
        self.assertIn('3992079c1036f18c78f8fdda78be3697b689071c', modules[1]['sources'][0]['url'])

    def test_runtime_commits_are_recorded(self):
        pins = json.loads((BUNDLE / 'runtime-lock.json').read_text())
        for name in ('org.kde.Sdk', 'org.kde.Platform'):
            self.assertRegex(pins[name], r'^[a-f0-9]{64}$')

    def test_bundled_stock_layout_check_uses_app_prefix(self):
        cmake = (ROOT / 'plugin/CMakeLists.txt').read_text()
        self.assertIn('--stock-layouts ${SWIPEKB_STOCK_LAYOUTS}', cmake)
        self.assertIn('"/usr/share/plasma/keyboard/layouts" CACHE PATH', cmake)
        build = (BUNDLE / 'build-swipe.sh').read_text()
        self.assertIn('-DSWIPEKB_STOCK_LAYOUTS=/app/share/plasma/keyboard/layouts', build)

    def check_loader_imports(self, backend=None):
        spec = importlib.util.spec_from_file_location('stockdriver', ROOT / 'scripts/check-stock-layout.py')
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'integration').mkdir()
            (root / 'integration/layout.py').write_text('def build_overlay(stock, destination):\n    destination.mkdir()\n')
            argv = ['check-stock-layout.py', '/bin/true', '--runtime-root', str(root)]
            if backend:
                argv += ['--backend-qml-dir', backend]
            with mock.patch.object(driver.sys, 'argv', argv), mock.patch.dict(driver.os.environ, {'QML_IMPORT_PATH': '/untrusted-host-qml'}), mock.patch.object(driver.subprocess, 'run') as run:
                run.return_value.returncode = 0
                self.assertEqual(driver.main(), 0)
                env = run.call_args.kwargs['env']
                expected = str(root / 'qml') + (':' + backend if backend else '')
                self.assertEqual(env['QML_IMPORT_PATH'], expected)
                self.assertEqual(env['QML2_IMPORT_PATH'], expected)
                self.assertNotIn('/untrusted-host-qml', env['QML_IMPORT_PATH'])

    def test_sdk_loader_imports_explicit_bundled_backend(self):
        self.check_loader_imports('/app/lib/qml')

    def test_native_loader_keeps_isolated_import_default(self):
        self.check_loader_imports()

    def test_bundle_loader_root_is_explicit_and_native_origin_preserved(self):
        cmake = (ROOT / 'plugin/CMakeLists.txt').read_text()
        self.assertIn('INSTALL_RPATH "\\$ORIGIN"', cmake)
        self.assertIn('SWIPEKB_BUNDLE_LIBDIR "" CACHE PATH', cmake)
        self.assertIn('CMAKE_INSTALL_PREFIX STREQUAL "/app"', cmake)
        self.assertIn('SWIPEKB_BUNDLE_LIBDIR STREQUAL "/app/lib"', cmake)
        self.assertIn('APPEND PROPERTY INSTALL_RPATH ${SWIPEKB_BUNDLE_LIBDIR}', cmake)
        self.assertIn('${SWIPEKB_BUNDLE_LIBDIR}/x86_64-linux-gnu', cmake)
        self.assertIn('CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"', cmake)
        self.assertIn('-DSWIPEKB_BUNDLE_LIBDIR=/app/lib', (BUNDLE / 'build-swipe.sh').read_text())

    def test_all_backend_licenses_are_explicitly_installed(self):
        module = self.manifest()['modules'][-1]
        commands = '\n'.join(module['build-commands'])
        for name in ('qt6-virtualkeyboard', 'libplasma', 'plasma-keyboard'):
            self.assertIn('/app/share/licenses/' + name, commands)
            source = next(s for s in module['sources'] if s.get('dest') == 'source-licenses/' + name)
            upstream = next(m for m in self.manifest()['modules'] if m['name'] == name)['sources'][0]
            self.assertEqual(source['sha256'], upstream['sha256'])
            self.assertIn('source-licenses/' + name + '/LICENSES', commands)
        self.assertIn('tcime-COPYING', commands)

    def test_flatpak_bundle_checks_context_capabilities_for_every_supported_locale(self):
        build=(BUNDLE/'build-swipe.sh').read_text()
        for locale in ('es-ES','es-MX','fr-FR','de-DE'):
            self.assertIn(locale,build)
        self.assertIn('nextWordCapable',build)
        self.assertIn('contextRankingCapable',build)
        self.assertIn('fineweb-es',build)

    def test_runtime_updates_fail_closed(self):
        spec = importlib.util.spec_from_file_location('flatpakbuild', BUNDLE / 'build.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        pins = json.loads((BUNDLE / 'runtime-lock.json').read_text())
        with self.assertRaisesRegex(RuntimeError, 'commit mismatch'):
            module.verify_runtime(pins, run=lambda *args, **kwargs: '0' * 64)
        module.verify_runtime(pins, run=lambda command, **kwargs: pins[command[-1].split('//')[0]])

    def test_backend_receives_touch_samples_without_global_environment_writes(self):
        import os
        import socket
        import sys
        import types
        spec = importlib.util.spec_from_file_location('flatpak_runtime_launch', BUNDLE/'launch.py')
        launcher = importlib.util.module_from_spec(spec)
        layout = types.SimpleNamespace(build_overlay=mock.Mock())
        with mock.patch.dict(sys.modules, {'layout': layout}), mock.patch.object(sys, 'path', sys.path.copy()):
            spec.loader.exec_module(launcher)
        left, right = socket.socketpair()
        self.addCleanup(left.close)
        self.addCleanup(right.close)
        with tempfile.TemporaryDirectory() as directory:
            environment = {'FLATPAK_ID': 'org.itsdestin.SwipeKeyboard',
                           'WAYLAND_SOCKET': str(left.fileno()), 'XDG_DATA_HOME': directory,
                           'QT_WAYLAND_RECONNECT': '1'}
            with mock.patch.dict(launcher.os.environ, environment, clear=True), \
                 mock.patch.object(launcher.Path, 'is_file', return_value=True), \
                 mock.patch.object(launcher.sys, 'argv', ['swipe-keyboard-flatpak']), \
                 mock.patch.object(launcher.os, 'execve') as execute:
                before = dict(launcher.os.environ)
                launcher.main()
                execute.assert_called_once()
                child = execute.call_args.args[2]
                self.assertEqual(child.get('QML_NO_TOUCH_COMPRESSION'), '1')
                self.assertEqual(child.get('QT_WAYLAND_FRAME_CALLBACK_TIMEOUT'), '5000')
                self.assertNotIn('QT_WAYLAND_RECONNECT', child)
                self.assertEqual(child['WAYLAND_SOCKET'], str(left.fileno()))
                self.assertEqual(dict(launcher.os.environ), before)
                os.fstat(left.fileno())  # Validation must not consume KWin's socket.

    def test_launcher_is_sandbox_only_and_preserves_fd(self):
        launcher = (BUNDLE / 'launcher.sh').read_text()
        self.assertIn('/.flatpak-info', launcher)
        self.assertIn('WAYLAND_SOCKET', launcher)
        self.assertNotRegex(launcher, r'unset\s+WAYLAND_SOCKET')
        self.assertIn('/app/', launcher)
        self.assertNotIn('flatpak-spawn', launcher)
        self.assertNotIn('setup.py', launcher)
        self.assertNotIn('kwriteconfig', launcher)
        self.assertIn('"$@"', launcher)

    def test_launcher_refuses_outside_sandbox(self):
        result = subprocess.run(['sh', str(BUNDLE / 'launcher.sh')],
                                capture_output=True, text=True, env={})
        self.assertEqual(result.returncode, 1)
        self.assertIn('requires its Flatpak sandbox', result.stderr)

    def test_export_is_manual_virtual_keyboard_selection(self):
        desktop = (BUNDLE / 'org.itsdestin.SwipeKeyboard.desktop').read_text()
        self.assertIn('X-KDE-Wayland-VirtualKeyboard=true', desktop)
        self.assertIn('NoDisplay=true', desktop)
        self.assertIn('Exec=swipe-keyboard-flatpak', desktop)
        self.assertIn('\nName=Swipe Keyboard\n', desktop)
        self.assertIn('Comment=English, Spanish, French and German swipe typing, completions and next-word suggestions;', desktop)
        self.assertNotIn('English-only', desktop)
        self.assertNotIn('Desktop build candidate', desktop)
        self.assertNotIn('--enable', desktop)


if __name__ == '__main__':
    unittest.main()
