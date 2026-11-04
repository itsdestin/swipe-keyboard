# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# Kept separately so staging contracts can exercise installation without compiling Qt.
include(GNUInstallDirs)
if(NOT DEFINED SWIPEKB_SOURCE)
    set(SWIPEKB_SOURCE "${CMAKE_SOURCE_DIR}")
endif()
set(_runtime "${CMAKE_INSTALL_DATADIR}/swipe-keyboard")
# WHY executable mode on ELF libraries: RPM's find-debuginfo scans executable files;
# a generic DIRECTORY copy otherwise makes every .so mode 0644 and emits an empty debugsource package.
install(DIRECTORY "${SWIPEKB_QML_DIR}/SwipeKeyboard" DESTINATION "${_runtime}/qml"
    FILES_MATCHING PATTERN "*.so*"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    PATTERN "qmldir" PATTERN "*.qml" PATTERN "*.qmltypes")
install(FILES "${SWIPEKB_ASSETS}/dict/en_swipe.combined" DESTINATION "${_runtime}/dict")
# WHY required, not OPTIONAL: a normal installation must contain every selectable language pack (and the shared
# swipe model that sits beside them).
install(DIRECTORY "${SWIPEKB_ASSETS}/language-packs/" DESTINATION "${_runtime}/language-packs")
install(DIRECTORY "${SWIPEKB_ASSETS}/licenses/" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/swipe-keyboard")
# WHY the whole LICENSES/ folder: the code is MIT (the keyboard add-on layers: KDE's GPL choice), the data files keep their own;
# every licence named by an SPDX header or REUSE.toml must travel with the package.
install(DIRECTORY "${SWIPEKB_SOURCE}/LICENSES/" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/swipe-keyboard/LICENSES")
install(FILES "${SWIPEKB_SOURCE}/LICENSE" "${SWIPEKB_SOURCE}/packaging/assets.lock.json"
    "${SWIPEKB_SOURCE}/packaging/NOTICE" "${SWIPEKB_SOURCE}/packaging/language-models/TATOEBA-CC-BY-2.0-FR.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/swipe-keyboard")
install(FILES "${SWIPEKB_SOURCE}/packaging/SOURCE-PROVENANCE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/swipe-keyboard" OPTIONAL)
install(FILES "${SWIPEKB_SOURCE}/integration/setup.py" "${SWIPEKB_SOURCE}/integration/layout.py" "${SWIPEKB_SOURCE}/integration/learning.py"
    "${SWIPEKB_SOURCE}/integration/plasma69.py"
    "${SWIPEKB_SOURCE}/integration/swipe-keyboard.desktop" DESTINATION "${_runtime}/integration")
install(PROGRAMS "${SWIPEKB_SOURCE}/integration/swipe-keyboard" DESTINATION "${_runtime}/integration")
# Automatic updates (plan: docs/active/plans/2026-10-23-auto-updates.md): the updater, its daily timer and the public
# update key. The Fedora repository file is installed only where dnf is the package manager.
# WHY an option: the Deck Flatpak is updated by Flatpak itself and must not carry system units (its build sets OFF).
option(SWIPEKB_NATIVE_UPDATES "Install the native updater, its daily timer and the update key" ON)
option(SWIPEKB_DNF_REPO "Install the dnf update repository file (Fedora)" OFF)
# WHY a target (Kubuntu/Debian/openSUSE, card #502): each such package is built against one distro release's own Qt and
# KDE, and the updater must only ever fetch that release's builds (deb/<target>/ or opensuse/<target>/ on the site).
set(SWIPEKB_UPDATE_TARGET "" CACHE STRING "distro release this package is built for, for example kubuntu-26.10")
if(SWIPEKB_NATIVE_UPDATES)
    install(PROGRAMS "${SWIPEKB_SOURCE}/integration/update.py" DESTINATION "${_runtime}/integration")
    install(FILES "${SWIPEKB_SOURCE}/packaging/updates/update-key.asc" "${SWIPEKB_SOURCE}/packaging/updates/update-key.gpg"
        DESTINATION "${_runtime}/integration")
    install(FILES "${SWIPEKB_SOURCE}/packaging/updates/swipe-keyboard-update.service"
        "${SWIPEKB_SOURCE}/packaging/updates/swipe-keyboard-update.timer" DESTINATION "lib/systemd/system")
    if(SWIPEKB_DNF_REPO)
        install(FILES "${SWIPEKB_SOURCE}/packaging/updates/swipe-keyboard.repo" DESTINATION "/etc/yum.repos.d")
    endif()
    if(SWIPEKB_UPDATE_TARGET)
        if(NOT SWIPEKB_UPDATE_TARGET MATCHES "^[a-z]+(-[a-z0-9.]+)?$")
            message(FATAL_ERROR "SWIPEKB_UPDATE_TARGET looks wrong: ${SWIPEKB_UPDATE_TARGET} (for example kubuntu-26.10)")
        endif()
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/update-target" "${SWIPEKB_UPDATE_TARGET}\n")
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/update-target" DESTINATION "${_runtime}/integration")
    endif()
endif()
install(PROGRAMS "${SWIPEKB_SOURCE}/integration/swipe-keyboard" DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${SWIPEKB_SOURCE}/integration/swipe-keyboard.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
# WHY the source contract is authoritative even for unstamped developer builds.
file(READ "${SWIPEKB_SOURCE}/packaging/release-targets.json" _release_contract)
string(JSON _release_version GET "${_release_contract}" version)
string(JSON _release_number GET "${_release_contract}" release)
set(SWIPEKB_PACKAGE_VERSION "${_release_version}-${_release_number}" CACHE STRING "Native package version for support diagnostics")
install(FILES "${SWIPEKB_SOURCE}/packaging/source-identity.json" DESTINATION "${_runtime}" OPTIONAL)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/runtime.json"
    "{\"qt\": \"${Qt6Core_VERSION_MAJOR}.${Qt6Core_VERSION_MINOR}\", \"package\": \"${SWIPEKB_PACKAGE_VERSION}\"}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/runtime.json" DESTINATION "${_runtime}")
