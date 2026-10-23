#!/bin/sh
set -eu
# WHY: isolated archive sources contain no host objects or venvs. Build every
# native object against this SDK, using a separate CPython 3.13 hash lock.
export CFLAGS="${CFLAGS:-} -fPIC"
export CXXFLAGS="${CXXFLAGS:-} -fPIC"
# WHY 4: matches the 4-CPU hosted runner; one bound shared with the manifest and build.py.
export CMAKE_BUILD_PARALLEL_LEVEL=4
python3 scripts/apply-build-patches.py third_party/swipe-library/third_party/executorch --swipe-library third_party/swipe-library
python3 scripts/provision-build-deps.py futo third_party/swipe-library/.venv --profile kde-6.10 --cache wheelcache
cmake -S third_party/swipe-library -B third_party/swipe-library/build -DCMAKE_BUILD_TYPE=Release -DUSE_MARCH_NATIVE=OFF
if [ -n "${CCACHE_DIR:-}" ]; then
    # WHY: CCACHE_DIR is set only when flatpak-builder --ccache supplies a persistent store, i.e.
    # trusted non-release CI runs; release tags never get it and compile everything cold.
    # ccache keys each result on the compiler's contents, flags, defines and the preprocessed source
    # (headers included), so a stale or foreign entry can only miss, never yield different bytes.
    # It covers ExecuTorch, the FUTO engine and the keyboard's own objects; the language-asset QRC
    # target opts out in CMake, and QRC generation, install output and packages are never cached.
    export CCACHE_COMPILERCHECK=content CCACHE_MAXSIZE=1500M
    export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
fi
make -C third_party/swipe-library host JOBS=4
cmake -S . -B build-flatpak -G Ninja -DCMAKE_INSTALL_PREFIX=/app -DCMAKE_INSTALL_LIBDIR=lib -DSWIPEKB_PACKAGE_VERSION="$(python3 scripts/release.py version)" -DUSE_MARCH_NATIVE=OFF -DSWIPEKB_STOCK_LAYOUTS=/app/share/plasma/keyboard/layouts -DSWIPEKB_BACKEND_QML_DIR=/app/lib/qml -DSWIPEKB_BUNDLE_LIBDIR=/app/lib -DSWIPEKB_INSTALL_GAMINGD=ON
cmake --build build-flatpak -j4
if [ -n "${CCACHE_DIR:-}" ]; then ccache -s || true; fi
QT_QPA_PLATFORM=offscreen ctest --test-dir build-flatpak --output-on-failure
cmake --install build-flatpak
# WHY swipekb-gamingd is in the ELF list: it is the Gaming Mode engine daemon (plan Section 4) and ships only
# in this Flatpak, so this is the one place its installed bytes get the same Qt-ABI check as the plugin.
# WHY installed bytes, not only build-tree ctest: catch broken RPATH/QML imports in the SDK.
python3 scripts/check-stock-layout.py build-flatpak/plugin/tst_stocklayout --runtime-root /app/share/swipe-keyboard --stock-layouts /app/share/plasma/keyboard/layouts --backend-qml-dir /app/lib/qml
python3 scripts/package-check.py --elf /app/share/swipe-keyboard/qml/SwipeKeyboard/*.so /app/share/swipe-keyboard/integration/swipekb-preflight /app/share/swipe-keyboard/integration/swipekb-config /app/share/swipe-keyboard/integration/swipekb-profilecheck /app/bin/swipekb-gamingd
for library in /app/share/swipe-keyboard/qml/SwipeKeyboard/*.so; do
    ldd "$library" > /tmp/swipe-ldd
    if grep -q 'not found' /tmp/swipe-ldd; then cat /tmp/swipe-ldd; exit 1; fi
done
for locale in en-US en-GB es-ES es-MX fr-FR de-DE; do
    layout=qwerty
    case "$locale" in fr-FR) layout=azerty;; de-DE) layout=qwertz;; es-ES|es-MX) layout=qwerty;; esac
    /app/share/swipe-keyboard/integration/swipekb-profilecheck --model-root /app/share/swipe-keyboard --locale "$locale" --layout "$layout" --mode Latin | python3 -c 'import json,sys; p=json.load(sys.stdin); assert p["swipeCapable"]; assert p["nextWordCapable"] and p["contextRankingCapable"] if p["language"] in ("es","fr","de") else p["nextWordCapable"]; assert p["packId"] == "fineweb-es" if p["locale"] in ("es-ES","es-MX") else True'
done
# WHY: native activation is unsupported on SteamOS 6.4; don't export its CLI or
# desktop entry alongside the manual-selection Flatpak entry.
rm /app/bin/swipe-keyboard /app/share/applications/swipe-keyboard.desktop \
    /app/share/swipe-keyboard/integration/setup.py \
    /app/share/swipe-keyboard/integration/swipe-keyboard \
    /app/share/swipe-keyboard/integration/swipe-keyboard.desktop
install -Dm755 packaging/flatpak/setup-guide.py /app/bin/swipe-keyboard-setup
install -Dm755 packaging/flatpak/launcher.sh /app/bin/swipe-keyboard-flatpak
install -Dm644 packaging/flatpak/launch.py /app/share/swipe-keyboard/flatpak/launch.py
install -Dm644 packaging/flatpak/org.itsdestin.SwipeKeyboard.desktop /app/share/applications/org.itsdestin.SwipeKeyboard.desktop
