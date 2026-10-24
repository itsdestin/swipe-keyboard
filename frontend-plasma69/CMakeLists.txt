# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
# SPDX-License-Identifier: BSD-2-Clause
#
# Swipe Keyboard's front end for Plasma 6.9's rebuilt keyboard (plasma-keyboard >= 6.8.80): the QML module
# SwipeKeyboard69. It does not use Qt Virtual Keyboard and does not link or load FUTO code.
#
# WHY it also builds on its own (cmake -S frontend-plasma69): the top-level project still requires FUTO's
# ExecuTorch build for the legacy front end; this one must be buildable and testable without it.

cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(swipekb_plasma69 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif()
    set(SWIPEKB_QML_DIR ${CMAKE_BINARY_DIR}/qml)
    set(SWIPEKB_ASSETS "" CACHE PATH "Installed-layout model data for tests (read-only)")
    enable_testing()
endif()

set(SWIPEKB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick Test)
# WHY here too: qt_standard_project_setup is directory-scoped, and this directory is a sibling of plugin/.
qt_standard_project_setup(REQUIRES 6.8)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# WHY a private static library of the project's FUTO-free pieces (interim, see src/openengine.h): the shared
# swipekb_engine/swipekb_session targets still link FUTO's library today. When feature/179-drop-futo lands they become
# FUTO-free and this list shrinks to the front end's own files.
add_library(swipekb69_open STATIC
    ${SWIPEKB_ROOT}/engine/src/context.cpp
    ${SWIPEKB_ROOT}/engine/src/ngram.cpp
    ${SWIPEKB_ROOT}/engine/src/ngram_predictor.cpp
    ${SWIPEKB_ROOT}/engine/src/nwp_model.cpp
    ${SWIPEKB_ROOT}/engine/src/nwp_predictor.cpp
    ${SWIPEKB_ROOT}/engine/src/learning.cpp
    ${SWIPEKB_ROOT}/engine/src/spelling.cpp
    ${SWIPEKB_ROOT}/engine/src/swipe_layout.cpp
    ${SWIPEKB_ROOT}/engine/src/swipe_net.cpp
    ${SWIPEKB_ROOT}/engine/src/swipe_beam.cpp
    ${SWIPEKB_ROOT}/engine/src/swipe_geo.cpp
    ${SWIPEKB_ROOT}/engine/src/swipe_engine.cpp
    ${SWIPEKB_ROOT}/engine/src/swipe_lm.cpp
    ${SWIPEKB_ROOT}/plugin/languageprofile.cpp
    ${SWIPEKB_ROOT}/plugin/languagetextrules.cpp
    ${SWIPEKB_ROOT}/plugin/languageruntime.cpp
    ${SWIPEKB_ROOT}/plugin/language-assets.qrc
    ${SWIPEKB_ROOT}/plugin/ownswipe.cpp
    ${SWIPEKB_ROOT}/plugin/swipesession.cpp
    ${SWIPEKB_ROOT}/plugin/nextwordsession.cpp
    ${SWIPEKB_ROOT}/plugin/predictionpolicy.cpp
    ${SWIPEKB_ROOT}/plugin/wordswap.cpp
    ${SWIPEKB_ROOT}/plugin/nextwordworker.cpp
    ${SWIPEKB_ROOT}/plugin/nextwordworker.h
    ${SWIPEKB_ROOT}/plugin/learningmanager.cpp
    ${SWIPEKB_ROOT}/plugin/learningmanager.h
    src/openpredictor.cpp
    src/openengine.cpp
    src/openengine.h
    # The front end's own logic, also linked by the unit tests (no QML or KDE needed).
    src/swipecontroller.h src/swipecontroller.cpp src/editorport.h src/editorport.cpp)
target_include_directories(swipekb69_open PUBLIC ${SWIPEKB_ROOT}/engine/include ${SWIPEKB_ROOT}/plugin ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(swipekb69_open PUBLIC Qt6::Core)
# WHY: the embedded lock/word lists are packaged data; never serve a stale compile from a cache.
set_target_properties(swipekb69_open PROPERTIES C_COMPILER_LAUNCHER "" CXX_COMPILER_LAUNCHER "")

# The QML module plasma-keyboard 6.9 loads through our generated layout wrappers.
qt_add_library(swipekeyboard69 SHARED)
qt_add_qml_module(swipekeyboard69
    URI SwipeKeyboard69
    VERSION 1.0
    # WHY interpreted QML: AOT-compiled QML ties the module to the exact Qt minor it was built with.
    NO_CACHEGEN
    OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard69
    QML_FILES qml/SwipeLayer.qml qml/SuggestionStrip.qml qml/SwipeAttach.qml
    SOURCES src/swipehook.h src/swipehook.cpp)
target_link_libraries(swipekeyboard69 PRIVATE Qt6::Quick Qt6::Qml swipekb69_open)
target_include_directories(swipekeyboard69 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(swipekeyboard69 PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard69)
# WHY $ORIGIN only: the module folder is self-contained; a missing sibling must fail loudly, never load from the build dir.
set_target_properties(swipekeyboard69 swipekeyboard69plugin PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "\$ORIGIN")

include(GNUInstallDirs)
install(DIRECTORY "${SWIPEKB_QML_DIR}/SwipeKeyboard69" DESTINATION "${CMAKE_INSTALL_DATADIR}/swipe-keyboard/qml"
    FILES_MATCHING PATTERN "*.so*"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    PATTERN "qmldir" PATTERN "*.qml" PATTERN "*.qmltypes")

add_subdirectory(tests)
