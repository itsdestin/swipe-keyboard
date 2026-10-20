Name: swipe-keyboard
Version: 0.2.1
Release: 1%{?dist}
Summary: Multilingual swipe input and English next-word suggestions for Plasma Keyboard
License: GPL-3.0-or-later AND Apache-2.0 AND CC-BY-SA-4.0 AND LicenseRef-FUTO-Model-Weights-1.0
URL: https://github.com/itsdestin/swipe-keyboard
Source0: %{name}-%{version}.tar.gz
ExclusiveArch: x86_64 aarch64
BuildRequires: gcc-c++, binutils, cmake >= 3.29, ninja-build, make, git, curl, python3, python3-pip
BuildRequires: qt6-qtbase-devel >= 6.8, qt6-qtdeclarative-devel, qt6-qtvirtualkeyboard-devel
# Kirigami's QML module is required by the native accent-colour test.
BuildRequires: kf6-kwindowsystem-devel, kf6-kconfig-devel, kf6-kirigami, gtest-devel
# Exercise the generated overlay against actual distro layouts, not a hand-written fixture.
BuildRequires: plasma-keyboard
# Real KConfigWatcher regression uses only dbus-run-session's private bus and busctl.
BuildRequires: dbus-daemon, systemd
Requires: python3, systemd, qt6-qtbase, qt6-qttools, kf6-kconfig
# Fedora's plasma-keyboard has epoch 1. Keep the series gate effective.
Requires: plasma-keyboard >= 1:6.7
Requires: plasma-keyboard < 1:6.9
# RPM generates Qt/KF/libstdc++ ABI dependencies automatically from installed ELF.

%description
English (US/UK), Spanish, French and German swipe recognition for Plasma Keyboard,
Powered by FUTO Swipe. Contextual next-word suggestions are English-only and require
an affirmative supported backend policy bridge; other backends retain swipe/taps.
Activation is an explicit per-user operation; package transactions never change
KWin settings. See the setup guide for tested KDE/Qt baselines, known language
quality limits and exact desktop verification scope.

%prep
%autosetup

%build
scripts/fetch-assets.sh
scripts/build-futo.sh
%cmake -G Ninja -DUSE_MARCH_NATIVE=OFF -DSWIPEKB_PACKAGE_VERSION=%{version}-%{release}
%cmake_build

%check
QT_QPA_PLATFORM=offscreen %ctest
python3 -m unittest discover -s tests/packaging -v
python3 -m unittest discover -s tests/integration -v

%install
%cmake_install
python3 scripts/package-check.py %{buildroot}

%files
%{_bindir}/swipe-keyboard
%{_datadir}/applications/swipe-keyboard.desktop
%{_datadir}/swipe-keyboard/
%license %{_datadir}/licenses/swipe-keyboard/
