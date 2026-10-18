# Compatibility investigation

status: resolved for package revision 0.1.0-3; verified scope is in revision3-verification.md

## Why the release-2 package refused an older installed system

The Arch recipe derived a package/version minimum from every library in recursive `ldd` output, not merely the plugin's direct ABI requirements. It repeated providers such as qt6-base, included build-time fakeroot injection, and treated build-machine package revisions as mandatory runtime versions. Consequently `pacman -T` rejected KF 6.30/Qt 6.12 package revision 2 and older curl/freetype/systemd/zlib despite no demonstrated plugin incompatibility.

An isolated probe of the **unchanged release-2 Arch payload** against those actual installed libraries passed the stock InputPanel tests: recognition, normal typing, whole-word/candidate undo, selection/privacy/generation guards and limits. The probe used temporary HOME/XDG folders, software/offscreen rendering and removed desktop D-Bus/Wayland/display connections. It did not modify or test the running keyboard. This demonstrates that those specific package floors were artificial, not that arbitrary older libraries work.

## Qt minor version: real problem in generated code

Qt documents public plugin compatibility from an older minor build to a newer minor runtime, within the same major, but not the opposite direction: <https://doc.qt.io/qt-6/deployment-plugins.html>.

The exact Fedora Qt 6.11-built payload was then probed on the Qt 6.12 runtime. It failed to load with `Qt_6.11_PRIVATE_API not found`, required by the module's backing library. `nm -D --undefined-only --with-symbol-versions` identified two generated `QQmlPrivate::AOTCompiledContext::mark` imports. Although the handwritten module uses public Qt targets, QML ahead-of-time compilation introduces a private, minor-specific ABI dependency.

An experiment disabling that generated QML code (`QT_QML_NO_CACHEGEN=ON`) rebuilt the module with Qt 6.11 in the Fedora guest. The same resulting module then **passed** the complete isolated stock InputPanel swipe/safety test on the newer Qt 6.12 host. `nm` confirmed no `PRIVATE_API` imports in any of its three shipped libraries. This is concrete cross-minor evidence; the corresponding original AOT build failed. Production packaging will use `NO_CACHEGEN` for the module and a final-ELF guard against unexpected private-Qt imports. Missing symbols must still fail before changing user settings, and this experiment alone does not establish every older Qt baseline or a final native package's full desktop behavior.

## Real minimums remain real

The release-2 engine requires versioned symbols up to `GLIBC_2.43` and `GLIBCXX_3.4.32`. Removing package metadata cannot remove those compiled requirements. Reducing them requires building with an older compatible toolchain/sysroot and testing the exact result. The native build locks also currently include CPython 3.14-specific wheels, so an older build image cannot be assumed to work without validating its Python/build inputs.

Broadening compatibility does not make an extension work where Plasma Keyboard itself is absent. Upstream Plasma Keyboard 6.7.0 declares source-build minimums Qt 6.10, Frameworks 6.26 and PlasmaQuick 6.7; it also uses QtGuiPrivate/QtWaylandClientPrivate for its compositor integration. Source: <https://invent.kde.org/plasma/plasma-keyboard/-/raw/v6.7.0/CMakeLists.txt>. Removing this extension's private-Qt imports does not make the upstream keyboard executable independent of its distro-matched Qt build. Native Plasma Keyboard is not Maliit, and having only Qt Virtual Keyboard libraries is insufficient. Existing supported Plasma Keyboard series, distro baselines and architectures remain the only verified claims until additional installation/compositor tests establish more.

## Verified older native build baseline

The official Arch archive database at `https://archive.archlinux.org/repos/2026/07/01/` contains Plasma Keyboard `6.7.2-1`, Qt `6.11.1-1`, KWindowSystem `6.27.0-1`, glibc `2.43+r37+gfdf10644d6ee-1`, GCC `16.1.1+r346+g4e03491b401d-1` and Python `3.14.6-1`. These were read from the actual core/extra repository database archives, not inferred from dates. This is a plausible common public build baseline with newer runtime tests on Qt 6.12/KF 6.30 and 6.31.

For comparison, the June 15 archive has Plasma Keyboard 6.6.5, which is outside the controller/compositor range already verified; it must not be silently added just because its Qt version looks suitable. The September 1 archive has Qt 6.11.2/KF 6.29, but provides less baseline coverage than July.

A complete July-snapshot build container is being prepared inside the owned Fedora VM. Downgrading that disposable container is not an update/downgrade of the user's device or of the Fedora desktop guest.

## Evidence locations

Private cache logs: `host-older-libraries-probe.log` (pass), `qt611-on-qt612-probe.log` (private-ABI load failure), and the subsequent no-AOT/candidate logs. No private typing, credentials or unreviewed desktop logs are included here.
