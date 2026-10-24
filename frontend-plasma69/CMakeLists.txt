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

# WHY a static library of the shared engine sources (the same files the other front end uses, compiled into this
# module): the SwipeKeyboard69 folder stays self-contained, with no load path into the Plasma 6.7/6.8 front end's folder,
# and this directory still builds on its own. Everything here is FUTO-free (feature/179-drop-futo).
set(_engine ${SWIPEKB_ROOT}/engine/src)
set(_plugin ${SWIPEKB_ROOT}/plugin)
add_library(swipekb69_core STATIC
    ${_engine}/context.cpp ${_engine}/language_engine.cpp ${_engine}/learning.cpp ${_engine}/ngram.cpp
    ${_engine}/ngram_predictor.cpp ${_engine}/normalize.cpp ${_engine}/nwp_model.cpp ${_engine}/nwp_predictor.cpp
    ${_engine}/prediction.cpp ${_engine}/spelling.cpp ${_engine}/swipe_beam.cpp ${_engine}/swipe_engine.cpp
    ${_engine}/swipe_geo.cpp ${_engine}/swipe_layout.cpp ${_engine}/swipe_lm.cpp ${_engine}/swipe_net.cpp
    ${_plugin}/languageprofile.cpp ${_plugin}/languageruntime.cpp ${_plugin}/languagetextrules.cpp ${_plugin}/language-assets.qrc
    ${_plugin}/swipesession.cpp ${_plugin}/nextwordsession.cpp ${_plugin}/predictionpolicy.cpp ${_plugin}/wordswap.cpp
    ${_plugin}/nextwordworker.cpp ${_plugin}/nextwordworker.h ${_plugin}/learningmanager.cpp ${_plugin}/learningmanager.h
    # The front end's own logic, also linked by the unit tests (no QML or KDE needed).
    src/swipecontroller.h src/swipecontroller.cpp src/editorport.h src/editorport.cpp)
target_include_directories(swipekb69_core PUBLIC ${SWIPEKB_ROOT}/engine/include ${_plugin} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(swipekb69_core PUBLIC Qt6::Core)
# WHY: the embedded lock/word lists are packaged data; never serve a stale compile from a cache.
set_target_properties(swipekb69_core PROPERTIES C_COMPILER_LAUNCHER "" CXX_COMPILER_LAUNCHER "")

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
target_link_libraries(swipekeyboard69 PRIVATE Qt6::Quick Qt6::Qml swipekb69_core)
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
