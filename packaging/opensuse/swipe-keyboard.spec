# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

Name: swipe-keyboard
Version: 0.5.1
# WHY the target in the release: the updater fetches only builds for this distro release (opensuse/<target>/ on the
# update site, integration/update.py), and the file name must differ from other targets' on a release page.
%global update_target tumbleweed
Release: 1.%{update_target}
Summary: English, Spanish, French and German swipe typing, completions and next-word suggestions for Plasma Keyboard
# WHY these licences (owner decision 2026-10-23: MIT where possible; each data file keeps its own, see REUSE.toml):
# engine, tools, launcher MIT; the keyboard add-on layers that plug into GPL code (Qt Virtual Keyboard plugin, Plasma 6.9
# front end) GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL; AOSP word list Apache-2.0; FineWeb text
# ODC-By-1.0; the swipe model MIT (trained on MIT-licensed swipe data; models: Destin Moss via youcoded.ai, 2026);
# swear-word list CC-BY-4.0; texting words CC0-1.0.
# SPDX lists CC-BY-2.0 (Generic), not the Tatoeba France port: its terms are a custom LicenseRef with its notice.
License: MIT AND (GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL) AND Apache-2.0 AND ODC-By-1.0 AND CC-BY-4.0 AND CC0-1.0 AND LicenseRef-Tatoeba-CC-BY-2.0-FR
URL: https://github.com/itsdestin/swipe-keyboard
Source0: %{name}-%{version}.tar.gz
ExclusiveArch: x86_64
# openSUSE Tumbleweed package (card #502), adapted from packaging/fedora/swipe-keyboard.spec with openSUSE's names.
BuildRequires: gcc-c++, binutils, cmake >= 3.29, ninja, python3, curl
BuildRequires: qt6-base-devel >= 6.8, qt6-declarative-devel, qt6-virtualkeyboard-devel
# Kirigami's QML module is required by the native accent-colour test.
BuildRequires: kf6-kwindowsystem-devel, kf6-kconfig-devel, kf6-kirigami-imports, gtest
# Exercise the generated overlay against actual distro layouts, not a hand-written fixture.
BuildRequires: plasma6-keyboard
# Real KConfigWatcher regression uses only dbus-run-session's private bus and busctl.
BuildRequires: dbus-1, systemd
# WHY each: python3 runs the launcher and updater; kwriteconfig6 (kf6-kconfig), qdbus6 (qt6-tools-qdbus) and busctl
# (systemd) are what the launcher calls to switch the keyboard; gpgv (gpg2) and zypper check and install updates.
Requires: python3, systemd, zypper, gpg2, kf6-kconfig, qt6-tools-qdbus, kf6-kirigami-imports
# WHY no upper bound: a Plasma 6.9 upgrade must never be blocked by this package. On 6.9+ the launcher
# (integration/setup.py) sees the newer keyboard and starts the plain KDE keyboard instead.
Requires: plasma6-keyboard >= 6.7
# WHY conditional and filtered, as on Fedora: only the Plasma 6.7/6.8 front end links Qt Virtual Keyboard; Plasma 6.9's
# rebuilt keyboard has none.
Requires: (qt6-virtualkeyboard if plasma6-keyboard < 6.8.80)
# WHY the QML imports too: openSUSE's QML dependency generator turns our own private modules (SwipeKeyboard,
# SwipeKeyboard69, inside /usr/share/swipe-keyboard) into requirements nothing in the distro provides, and the package
# could not be installed (GitHub's Tumbleweed image, 2026-10-30).
%global __requires_exclude ^(libQt6VirtualKeyboard\\.so.*|qt6qmlimport\\(SwipeKeyboard.*)$
# RPM generates the other Qt/KF/libstdc++ ABI dependencies automatically from installed ELF.

%description
English (US/UK), Spanish, French and German swipe typing, current-word completions and next-word suggestions for
KDE's on-screen keyboard, with its own open swipe engine. Everything runs offline on this computer.
Installing it changes nothing on its own: each user turns it on with "swipe-keyboard enable" (or picks it in System
Settings), and "swipe-keyboard disable" puts their previous keyboard back.

%prep
%autosetup

%build
scripts/fetch-assets.sh
# WHY plain cmake rather than openSUSE's %%cmake: the same flags as the Arch recipe, and the build folder name the
# test commands below use.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DSWIPEKB_PACKAGE_VERSION=%{version}-%{release} -DSWIPEKB_UPDATE_TARGET=%{update_target}
cmake --build build

%check
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure -j%{_smp_build_ncpus}
python3 -m unittest discover -s tests/packaging -v
python3 -m unittest discover -s tests/integration -v

%install
DESTDIR=%{buildroot} cmake --install build
# WHY a packaged link instead of a scriptlet running "systemctl enable": updates are on from install, and removing the
# package removes the link (as on Fedora). "sudo swipe-keyboard updates off" turns them off.
mkdir -p %{buildroot}%{_unitdir}/timers.target.wants
ln -s ../swipe-keyboard-update.timer %{buildroot}%{_unitdir}/timers.target.wants/swipe-keyboard-update.timer
python3 scripts/package-check.py %{buildroot}

%preun
# WHY: on removal the update timer must stop now, or it fires once more at a service that is gone (card #502).
if [ "$1" -eq 0 ] && [ -d /run/systemd/system ]; then
    systemctl stop swipe-keyboard-update.timer >/dev/null 2>&1 || :
fi

%postun
if [ "$1" -eq 0 ] && [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
fi

%files
%{_bindir}/swipe-keyboard
%{_datadir}/applications/swipe-keyboard.desktop
%{_datadir}/swipe-keyboard/
%{_unitdir}/swipe-keyboard-update.service
%{_unitdir}/swipe-keyboard-update.timer
%{_unitdir}/timers.target.wants/swipe-keyboard-update.timer
%license %{_datadir}/licenses/swipe-keyboard/
