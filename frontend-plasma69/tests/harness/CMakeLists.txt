# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
# SPDX-License-Identifier: MIT
#
# End-to-end tests against KDE's REAL rebuilt keyboard core, built from a plasma-keyboard source tree
# (SWIPEKB_PLASMA_KEYBOARD_SOURCE, e.g. a clone of master or of the 6.9 beta tag) into this build folder only.
#
# WHY only these pieces: the full plasma-keyboard application needs PlasmaQuick >= 6.7.90 and a compositor. Its
# keyboard logic (virtualkeyboard/), its QML components, its shared layouts and its language packages are what our
# front end touches, and they build against the installed Qt/KF6 unmodified. The application-only QML module
# (org.kde.plasma.keyboard: Feedback, PlasmaKeyboardSettings) is a small stub, and a fake Wayland backend stands in
# for the compositor. Nothing is installed; nothing touches the running keyboard.

set(PK ${SWIPEKB_PLASMA_KEYBOARD_SOURCE})
if(NOT EXISTS ${PK}/virtualkeyboard/inputengine.h)
    message(FATAL_ERROR "SWIPEKB_PLASMA_KEYBOARD_SOURCE is not a plasma-keyboard 6.8.80+ tree: ${PK}")
endif()
find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick QuickControls2 Test)
# WHY private Gui headers: KDE's core uses them (as the real build does); this test binary is never shipped.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6GuiPrivate REQUIRED NO_MODULE)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6I18n REQUIRED)

set(PK_QML ${CMAKE_CURRENT_BINARY_DIR}/qml)
set(PK_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/prefix)

# --- KDE's keyboard core library (unmodified sources) ------------------------------------------------------------
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/logging.h
    "#pragma once\n#include <QLoggingCategory>\nQ_DECLARE_LOGGING_CATEGORY(PlasmaKeyboard)\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/logging.cpp
    "#include \"logging.h\"\nQ_LOGGING_CATEGORY(PlasmaKeyboard, \"org.kde.plasma.keyboard\")\n")
file(GLOB PK_VK_SOURCES ${PK}/virtualkeyboard/*.cpp ${PK}/virtualkeyboard/*.h
    ${PK}/virtualkeyboard/textcomposers/*.cpp ${PK}/virtualkeyboard/textcomposers/*.h)
qt_add_library(pk_virtualkeyboard SHARED)
qt_add_qml_module(pk_virtualkeyboard
    URI org.kde.plasma.keyboard.virtualkeyboard
    VERSION 1.0
    OUTPUT_DIRECTORY ${PK_QML}/org/kde/plasma/keyboard/virtualkeyboard
    SOURCES ${PK_VK_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/gen/logging.cpp
    DEPENDENCIES QtQuick)
target_include_directories(pk_virtualkeyboard PUBLIC ${PK}/virtualkeyboard ${PK}/virtualkeyboard/textcomposers ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(pk_virtualkeyboard PUBLIC Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Quick KF6::CoreAddons)

# --- QML-only modules from KDE's tree. WHY flat aliases: ECM's ecm_target_qml_sources puts every file at the module
# root, so components/keys/Key.qml sees BreezeConstants.qml; we reproduce that layout.
function(pk_qml_module target uri dir)
    file(GLOB_RECURSE files ${dir}/*.qml)
    foreach(f IN LISTS files)
        get_filename_component(name ${f} NAME)
        set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${name})
        if(name MATCHES "^(BreezeConstants|FlickKeyPrivate)\\.qml$")
            set_source_files_properties(${f} PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
        endif()
    endforeach()
    string(REPLACE "." "/" path ${uri})
    qt_add_library(${target} SHARED)
    qt_add_qml_module(${target} URI ${uri} VERSION 1.0 NO_CACHEGEN NO_LINT
        OUTPUT_DIRECTORY ${PK_QML}/${path} QML_FILES ${files})
endfunction()
pk_qml_module(pk_components org.kde.plasma.keyboard.virtualkeyboard.components ${PK}/virtualkeyboard/components)
pk_qml_module(pk_defaultplugin org.kde.plasma.keyboard.layouts.defaultplugin ${PK}/layouts/defaultplugin)
# Stand-in for the application's own module (Feedback sounds, PlasmaKeyboardSettings).
set(stub_files ${CMAKE_CURRENT_SOURCE_DIR}/stub/Feedback.qml ${CMAKE_CURRENT_SOURCE_DIR}/stub/PlasmaKeyboardSettings.qml)
set_source_files_properties(${stub_files} PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
foreach(f IN LISTS stub_files)
    get_filename_component(name ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
qt_add_library(pk_appstub SHARED)
qt_add_qml_module(pk_appstub URI org.kde.plasma.keyboard VERSION 1.0 NO_CACHEGEN NO_LINT
    OUTPUT_DIRECTORY ${PK_QML}/org/kde/plasma/keyboard QML_FILES ${stub_files})

# --- KDE's language packages, "installed" into a scratch prefix the way plasma_install_package does ----------------
file(STRINGS ${PK}/layouts/CMakeLists.txt package_lines REGEX "plasma_install_package")
foreach(line IN LISTS package_lines)
    if(line MATCHES "plasma_install_package\\(([^ ]+) ([^ ]+) ")
        file(COPY ${PK}/layouts/${CMAKE_MATCH_1}/ DESTINATION ${PK_PREFIX}/share/plasma/keyboard/keyboardpackages/${CMAKE_MATCH_2})
    endif()
endforeach()

# --- the test -----------------------------------------------------------------------------------------------------
configure_file(Host.qml.in ${CMAKE_CURRENT_BINARY_DIR}/Host.qml @ONLY)
qt_add_executable(tst_plasma69_e2e tst_e2e.cpp)
target_link_libraries(tst_plasma69_e2e PRIVATE pk_virtualkeyboard Qt6::Quick Qt6::Test KF6::I18n KF6::I18nQml)
target_compile_definitions(tst_plasma69_e2e PRIVATE
    PK_HOST_QML="${CMAKE_CURRENT_BINARY_DIR}/Host.qml"
    PK_QML_DIR="${PK_QML}"
    PK_PREFIX="${PK_PREFIX}"
    SWIPEKB_QML_DIR="${SWIPEKB_QML_DIR}"
    SWIPEKB_GENERATOR="${SWIPEKB_ROOT}/integration/plasma69.py")
add_dependencies(tst_plasma69_e2e swipekeyboard69plugin pk_componentsplugin pk_defaultpluginplugin pk_appstubplugin pk_virtualkeyboardplugin)
add_test(NAME plasma69_e2e COMMAND tst_plasma69_e2e)
set_tests_properties(plasma69_e2e PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
    ENVIRONMENT "SWIPEKB_TEST_MODEL_DATA=${SWIPEKB_TEST_MODEL_DATA};QT_QPA_PLATFORM=offscreen;QML_NO_TOUCH_COMPRESSION=1")
