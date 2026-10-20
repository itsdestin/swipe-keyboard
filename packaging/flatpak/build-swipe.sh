#!/bin/sh
set -eu
# WHY: isolated archive sources contain no host objects or venvs. Build every
# native object against this SDK, using a separate CPython 3.13 hash lock.
export CFLAGS="${CFLAGS:-} -fPIC"
export CXXFLAGS="${CXXFLAGS:-} -fPIC"
export CMAKE_BUILD_PARALLEL_LEVEL=2
python3 scripts/apply-build-patches.py third_party/swipe-library/third_party/executorch --swipe-library third_party/swipe-library
python3 scripts/provision-build-deps.py futo third_party/swipe-library/.venv --profile kde-6.10 --cache wheelcache
cmake -S third_party/swipe-library -B third_party/swipe-library/build -DCMAKE_BUILD_TYPE=Release -DUSE_MARCH_NATIVE=OFF
make -C third_party/swipe-library host JOBS=2
cmake -S . -B build-flatpak -G Ninja -DCMAKE_INSTALL_PREFIX=/app -DCMAKE_INSTALL_LIBDIR=lib -DSWIPEKB_PACKAGE_VERSION=0.2.0-1 -DUSE_MARCH_NATIVE=OFF -DSWIPEKB_STOCK_LAYOUTS=/app/share/plasma/keyboard/layouts -DSWIPEKB_BACKEND_QML_DIR=/app/lib/qml -DSWIPEKB_BUNDLE_LIBDIR=/app/lib
cmake --build build-flatpak -j2
QT_QPA_PLATFORM=offscreen ctest --test-dir build-flatpak --output-on-failure
cmake --install build-flatpak
# WHY: native activation is unsupported on SteamOS 6.4; don't export its CLI or
# desktop entry alongside the manual-selection Flatpak entry.
rm /app/bin/swipe-keyboard /app/share/applications/swipe-keyboard.desktop \
    /app/share/swipe-keyboard/integration/setup.py \
    /app/share/swipe-keyboard/integration/swipe-keyboard \
    /app/share/swipe-keyboard/integration/swipe-keyboard.desktop
install -Dm755 packaging/flatpak/launcher.sh /app/bin/swipe-keyboard-flatpak
install -Dm644 packaging/flatpak/launch.py /app/share/swipe-keyboard/flatpak/launch.py
install -Dm644 packaging/flatpak/org.itsdestin.SwipeKeyboard.desktop /app/share/applications/org.itsdestin.SwipeKeyboard.desktop
