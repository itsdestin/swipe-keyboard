# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

Name: swipe-keyboard
Version: 0.5.1
Release: 1%{?dist}
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
ExclusiveArch: x86_64 aarch64
BuildRequires: gcc-c++, binutils, cmake >= 3.29, ninja-build, python3
BuildRequires: qt6-qtbase-devel >= 6.8, qt6-qtdeclarative-devel, qt6-qtvirtualkeyboard-devel
# Kirigami's QML module is required by the native accent-colour test.
BuildRequires: kf6-kwindowsystem-devel, kf6-kconfig-devel, kf6-kirigami, gtest-devel
# Exercise the generated overlay against actual distro layouts, not a hand-written fixture.
BuildRequires: plasma-keyboard
# Real KConfigWatcher regression uses only dbus-run-session's private bus and busctl.
BuildRequires: dbus-daemon, systemd
Requires: python3, systemd, dnf, qt6-qtbase, qt6-qttools, kf6-kconfig
# Fedora's plasma-keyboard has epoch 1, so the lower bound needs the 1: prefix to mean 6.7.
# WHY no upper bound: a Plasma 6.9 upgrade must never be blocked by this package. On 6.9+ the launcher
# (integration/setup.py) sees the newer keyboard and starts the plain KDE keyboard instead.
Requires: plasma-keyboard >= 1:6.7
# WHY conditional: only the Plasma 6.7/6.8 front end links Qt Virtual Keyboard; Plasma 6.9's rebuilt keyboard has none,
# so the library is required only while plasma-keyboard is older than the 6.9 series (6.8.80). The automatic ELF
# requirement is filtered for the same reason. Kirigami: the 6.9 suggestion row uses its theme colours.
Requires: (qt6-qtvirtualkeyboard if plasma-keyboard < 1:6.8.80)
Requires: kf6-kirigami
%global __requires_exclude ^libQt6VirtualKeyboard\\.so.*$
# RPM generates the other Qt/KF/libstdc++ ABI dependencies automatically from installed ELF.

%description
English (US/UK), Spanish, French and German swipe typing, current-word completions,
and next-word suggestions for Plasma Keyboard, with its own open swipe engine. Spanish
Spain and Mexico share one next-word model; the next-word models (built from open web text
and Tatoeba sentences) are compact, general baselines, not region-specific Spanish training or a Gboard
parity claim. On stock Plasma, the next-word model uses confirmed words typed by
this keyboard in the current field; stock does not expose every raw application
completion preference. Sensitive and restricted fields remain blocked. The bundled
Deck backend keeps its raw-policy bridge and stricter application vetoes.
Activation is an explicit per-user operation; package transactions never change
KWin settings. See the setup guide for tested KDE/Qt baselines and quality limits.

%prep
%autosetup

%build
scripts/fetch-assets.sh
%cmake -G Ninja -DSWIPEKB_PACKAGE_VERSION=%{version}-%{release} -DSWIPEKB_DNF_REPO=ON
%cmake_build

%check
QT_QPA_PLATFORM=offscreen %ctest
python3 -m unittest discover -s tests/packaging -v
python3 -m unittest discover -s tests/integration -v

%install
%cmake_install
# WHY a packaged link instead of an install scriptlet running "systemctl enable": updates are on from install,
# and removing the package removes the link. "sudo swipe-keyboard updates off" turns them off.
mkdir -p %{buildroot}%{_unitdir}/timers.target.wants
ln -s ../swipe-keyboard-update.timer %{buildroot}%{_unitdir}/timers.target.wants/swipe-keyboard-update.timer
python3 scripts/package-check.py %{buildroot}

%files
%{_bindir}/swipe-keyboard
%{_datadir}/applications/swipe-keyboard.desktop
%{_datadir}/swipe-keyboard/
%{_unitdir}/swipe-keyboard-update.service
%{_unitdir}/swipe-keyboard-update.timer
%{_unitdir}/timers.target.wants/swipe-keyboard-update.timer
%config(noreplace) %{_sysconfdir}/yum.repos.d/swipe-keyboard.repo
%license %{_datadir}/licenses/swipe-keyboard/
