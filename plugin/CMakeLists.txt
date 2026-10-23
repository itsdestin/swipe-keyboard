find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick VirtualKeyboard Test)
# WHY KWindowSystem: WindowBlur asks KWin to blur behind the keyboard (KWindowEffects), the same call Plasma's
# panels use. It ships with Plasma, so it is always there wherever Plasma Keyboard runs.
find_package(KF6WindowSystem REQUIRED)
qt_standard_project_setup(REQUIRES 6.8)
set(SWIPEKB_TEST_MULTILINGUAL_MODELS "" CACHE PATH "Explicit read-only multilingual context model fixture")
set(SWIPEKB_TEST_LANGUAGE_PACKS "" CACHE PATH "Explicit generated multilingual pack fixture")

# WHY Core-only: activation must validate Qt metadata and all ELF relocations
# without a compositor, QML engine or access to the user's running keyboard.
qt_add_executable(swipekb-preflight preflight.cpp)
target_link_libraries(swipekb-preflight PRIVATE Qt6::Core)
install(TARGETS swipekb-preflight RUNTIME DESTINATION share/swipe-keyboard/integration)

# WHY a Core-only library: prediction consumers share verified values/rules without QML or GUI types.
set(CMAKE_AUTORCC ON)
add_library(swipekb_language STATIC languageprofile.cpp profilediagnostics.cpp languagetextrules.cpp language-assets.qrc ../engine/src/context.cpp)
target_include_directories(swipekb_language PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ../engine/include)
target_link_libraries(swipekb_language PUBLIC Qt6::Core)
qt_add_executable(tst_languageprofile tests/tst_languageprofile.cpp)
target_link_libraries(tst_languageprofile PRIVATE swipekb_language Qt6::Test)
add_test(NAME plugin_languageprofile COMMAND tst_languageprofile)
qt_add_executable(tst_profilediagnostics tests/tst_profilediagnostics.cpp)
target_link_libraries(tst_profilediagnostics PRIVATE swipekb_language Qt6::Test)
add_test(NAME plugin_profilediagnostics COMMAND tst_profilediagnostics)
qt_add_executable(swipekb-profilecheck ../integration/profilecheck.cpp)
target_link_libraries(swipekb-profilecheck PRIVATE swipekb_language)
install(TARGETS swipekb-profilecheck RUNTIME DESTINATION share/swipe-keyboard/integration)

# Pure text logic, no keyboard or engine: unit-tested on its own.
add_library(swipekb_session STATIC swipesession.cpp swipeoutcome.cpp nextwordsession.cpp predictionpolicy.cpp predictiondiagnostics.cpp wordswap.cpp)
target_include_directories(swipekb_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# WHY the engine: SwipeSession::contextWords forwards to swipekb::contextWords (one shared implementation).
target_link_libraries(swipekb_session PUBLIC Qt6::Core swipekb_engine)

qt_add_executable(tst_swipesession tests/tst_swipesession.cpp)
target_link_libraries(tst_swipesession PRIVATE swipekb_session Qt6::Test)
add_test(NAME plugin_session COMMAND tst_swipesession)

qt_add_executable(tst_swipeoutcome tests/tst_swipeoutcome.cpp)
target_link_libraries(tst_swipeoutcome PRIVATE swipekb_session Qt6::Test)
add_test(NAME plugin_outcome COMMAND tst_swipeoutcome)

# WHY these tests are independent of a compositor/installed keyboard: prediction
# insertion and policy authority must be pinned without activating a host backend.
foreach(feature IN ITEMS nextwordsession predictionpolicy predictiondiagnostics ownedhistory typedwordsession wordswap)
    qt_add_executable(tst_${feature} tests/tst_${feature}.cpp)
    target_link_libraries(tst_${feature} PRIVATE swipekb_session Qt6::Test)
    add_test(NAME plugin_${feature} COMMAND tst_${feature})
endforeach()
qt_add_executable(tst_nextwordworker tests/tst_nextwordworker.cpp nextwordworker.h nextwordworker.cpp)
target_link_libraries(tst_nextwordworker PRIVATE swipekb_session Qt6::Test)
add_test(NAME plugin_nextwordworker COMMAND tst_nextwordworker)
set_tests_properties(plugin_nextwordworker PROPERTIES ENVIRONMENT "SWIPEKB_TEST_ASSETS=${SWIPEKB_ASSETS}")
qt_add_executable(tst_backendpredictionpolicy tests/tst_backendpredictionpolicy.cpp)
target_include_directories(tst_backendpredictionpolicy PRIVATE ${CMAKE_SOURCE_DIR}/scripts/patches)
target_link_libraries(tst_backendpredictionpolicy PRIVATE Qt6::Core Qt6::Test)
add_test(NAME plugin_backendpredictionpolicy COMMAND tst_backendpredictionpolicy)
qt_add_executable(tst_predictioncapture tests/tst_predictioncapture.cpp)
target_compile_definitions(tst_predictioncapture PRIVATE SWIPEKB_CAPTURE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/SwipeCapture.qml")
target_link_libraries(tst_predictioncapture PRIVATE Qt6::Quick Qt6::Qml Qt6::VirtualKeyboard Qt6::Test)
add_test(NAME plugin_predictioncapture COMMAND tst_predictioncapture)
set_tests_properties(plugin_predictioncapture PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_IM_MODULE=qtvirtualkeyboard")
qt_add_executable(test_prediction ../engine/tests/test_prediction.cpp)
target_link_libraries(test_prediction PRIVATE swipekb_engine Qt6::Test)
add_test(NAME engine_prediction COMMAND test_prediction)
set(_engine_prediction_env "SWIPEKB_TEST_ASSETS=${SWIPEKB_ASSETS}")
if(SWIPEKB_TEST_MULTILINGUAL_MODELS)
    list(APPEND _engine_prediction_env "SWIPEKB_TEST_MULTILINGUAL_MODELS=${SWIPEKB_TEST_MULTILINGUAL_MODELS}")
endif()
set_tests_properties(engine_prediction PROPERTIES ENVIRONMENT "${_engine_prediction_env}")

# Opt-in swipe recorder (writes swipes.jsonl for offline tuning). Separate so it is tested without a keyboard.
add_library(swipekb_recorder STATIC swiperecorder.cpp)
target_include_directories(swipekb_recorder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(swipekb_recorder PUBLIC Qt6::Core)

qt_add_executable(tst_swiperecorder tests/tst_swiperecorder.cpp)
target_link_libraries(tst_swiperecorder PRIVATE swipekb_recorder Qt6::Test)
add_test(NAME plugin_recorder COMMAND tst_swiperecorder)

# The QML module Plasma Keyboard loads. The module folder holds qmldir, the plugin .so, the backing .so
# (libswipekeyboard) and the engine .so (libswipekb_engine), all found through $ORIGIN.
# WHY the backing library is SHARED: a static one makes Qt build a static plugin, which can't be loaded at runtime.
qt_add_library(swipekeyboard SHARED)
qt_add_qml_module(swipekeyboard
    URI SwipeKeyboard
    VERSION 1.0
    # WHY: QML AOT imports Qt_<minor>_PRIVATE_API even with public C++ targets.
    # Interpreting this small QML file preserves Qt's older-built -> newer-runtime ABI.
    NO_CACHEGEN
    OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard
    QML_FILES SwipeCapture.qml
    SOURCES swipeinputmethod.h swipeinputmethod.cpp nextwordworker.h nextwordworker.cpp windowblur.h windowblur.cpp
            learningmanager.h learningmanager.cpp
)
# WHY a static library of the project's own swipe engine (card #179): opt-in alternative to FUTO's recogniser. It uses
# NgramLM from the shared swipekb_engine, so ngram.cpp is not compiled a second time.
add_library(swipekb_ownswipe STATIC ownswipe.cpp ../engine/src/swipe_layout.cpp ../engine/src/swipe_net.cpp
    ../engine/src/swipe_beam.cpp ../engine/src/swipe_geo.cpp ../engine/src/swipe_engine.cpp ../engine/src/swipe_lm.cpp)
set_target_properties(swipekb_ownswipe PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(swipekb_ownswipe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ../engine/include)
target_link_libraries(swipekb_ownswipe PUBLIC swipekb_engine Qt6::Core)
target_link_libraries(swipekeyboard PRIVATE Qt6::Quick Qt6::VirtualKeyboard KF6::WindowSystem swipekb_session swipekb_recorder swipekb_engine swipekb_language swipekb_ownswipe)
# WHY: flags-only observations must not exist in ordinary release builds or change authorization policy.
option(SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC "PRIVATE flags-only Deck diagnostic (not a release)" OFF)
if(SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC)
    target_compile_definitions(swipekeyboard PRIVATE SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC=1)
endif()
# WHY: the public query is a production safety boundary, including invalid and deleted receivers.
qt_add_executable(tst_livehintsquery tests/tst_livehintsquery.cpp)
target_include_directories(tst_livehintsquery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_livehintsquery PRIVATE Qt6::Quick Qt6::Test)
add_test(NAME plugin_livehintsquery COMMAND tst_livehintsquery)
set_tests_properties(plugin_livehintsquery PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
qt_add_executable(tst_nextwordintegration tests/tst_nextwordintegration.cpp)
target_include_directories(tst_nextwordintegration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_nextwordintegration PRIVATE swipekeyboard Qt6::Quick Qt6::Qml Qt6::VirtualKeyboard Qt6::Test)
add_test(NAME plugin_nextwordintegration COMMAND tst_nextwordintegration)
set_tests_properties(plugin_nextwordintegration PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;QT_IM_MODULE=qtvirtualkeyboard;SWIPEKB_TEST_ASSETS=${SWIPEKB_ASSETS};SWIPEKB_TEST_PROFILE_ASSETS=${SWIPEKB_ASSETS};SWIPEKB_TEST_LANGUAGE_PACKS=${SWIPEKB_TEST_LANGUAGE_PACKS}")

# Phase 2 learning from the person's own typing: the model, its on-disk store, and the whole thing inside the real input
# method. The first two need only the engine library (the model lives there); the third is like tst_nextwordintegration.
qt_add_executable(tst_personalmodel tests/tst_personalmodel.cpp)
target_link_libraries(tst_personalmodel PRIVATE swipekb_engine Qt6::Test)
add_test(NAME plugin_personalmodel COMMAND tst_personalmodel)
qt_add_executable(tst_learningmanager tests/tst_learningmanager.cpp learningmanager.cpp learningmanager.h)
target_include_directories(tst_learningmanager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_learningmanager PRIVATE swipekb_engine Qt6::Test)
add_test(NAME plugin_learningmanager COMMAND tst_learningmanager)
qt_add_executable(tst_learningintegration tests/tst_learningintegration.cpp)
target_include_directories(tst_learningintegration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_learningintegration PRIVATE swipekeyboard Qt6::Quick Qt6::Qml Qt6::VirtualKeyboard Qt6::Test)
add_test(NAME plugin_learningintegration COMMAND tst_learningintegration)
set_tests_properties(plugin_learningintegration PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;QT_IM_MODULE=qtvirtualkeyboard;SWIPEKB_TEST_PROFILE_ASSETS=${SWIPEKB_ASSETS}")

# WHY register in the ordinary build too: the lightweight feature project is not the normal CTest gate.
# Fixtures are explicit opt-ins. Tests QSKIP when absent, but supplied invalid fixtures fail rather than pass.
set(_language_fixture_env)
foreach(fixture ENGLISH_ASSETS LANGUAGE_PACKS)
    set(SWIPEKB_TEST_${fixture} "" CACHE PATH "Explicit read-only language ${fixture} test fixture")
    if(SWIPEKB_TEST_${fixture})
        list(APPEND _language_fixture_env "SWIPEKB_TEST_${fixture}=${SWIPEKB_TEST_${fixture}}")
    endif()
endforeach()
qt_add_executable(tst_languagegeometry tests/tst_languagegeometry.cpp)
target_link_libraries(tst_languagegeometry PRIVATE swipekb_language swipekb_engine Qt6::Test)
add_test(NAME plugin_languagegeometry COMMAND tst_languagegeometry)
qt_add_executable(tst_languagepacks tests/tst_languagepacks.cpp)
target_link_libraries(tst_languagepacks PRIVATE swipekb_language swipekb_engine Qt6::Test)
add_test(NAME plugin_languagepacks COMMAND tst_languagepacks)
qt_add_executable(tst_languagecapture tests/tst_languagecapture.cpp)
target_link_libraries(tst_languagecapture PRIVATE Qt6::Quick Qt6::Qml Qt6::Test)
target_compile_definitions(tst_languagecapture PRIVATE SWIPEKB_CAPTURE_QML="${CMAKE_CURRENT_SOURCE_DIR}/SwipeCapture.qml")
add_test(NAME plugin_languagecapture COMMAND tst_languagecapture)
set_tests_properties(plugin_languagecapture PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_IM_MODULE=")
qt_add_executable(tst_languageintegration tests/tst_languageintegration.cpp)
target_include_directories(tst_languageintegration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_languageintegration PRIVATE swipekeyboard Qt6::Quick Qt6::Qml Qt6::VirtualKeyboard Qt6::Test)
add_test(NAME plugin_languageintegration COMMAND tst_languageintegration)
qt_add_executable(tst_ownswipeintegration tests/tst_ownswipeintegration.cpp)
target_include_directories(tst_ownswipeintegration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_ownswipeintegration PRIVATE swipekeyboard swipekb_ownswipe Qt6::Quick Qt6::Qml Qt6::VirtualKeyboard Qt6::Test)
target_compile_definitions(tst_ownswipeintegration PRIVATE SWIPEKB_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../tests/data")
add_test(NAME plugin_ownswipeintegration COMMAND tst_ownswipeintegration)
set_tests_properties(plugin_languageprofile plugin_languagepacks PROPERTIES ENVIRONMENT "${_language_fixture_env}")
# WHY CTest must expose missing-fixture coverage as skipped, not a misleading green model gate.
set_tests_properties(plugin_languageprofile plugin_languagepacks plugin_languageintegration PROPERTIES
    SKIP_RETURN_CODE 77)
set_tests_properties(plugin_languageintegration PROPERTIES ENVIRONMENT
    "${_language_fixture_env};QT_QPA_PLATFORM=offscreen;QT_IM_MODULE=qtvirtualkeyboard;QT_VIRTUALKEYBOARD_DESKTOP_DISABLE=1")
# WHY SHARED + same output folder: a STATIC backing library makes the plugin static too, which Qt cannot load.
# The three .so files (plugin, backing library, engine) sit side by side so the module folder is
# self-contained and relocatable.
set_target_properties(swipekeyboard PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard)
# WHY BUILD_WITH_INSTALL_RPATH: the default build RUNPATH also lists the build dir, so an incomplete installed
# copy would silently load libs from there. Forcing "$ORIGIN" only makes a missing sibling lib fail loudly.
set_target_properties(swipekeyboard swipekeyboardplugin swipekb_engine PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "\$ORIGIN")
# WHY: the bundle's QtVirtualKeyboard is not in the SDK/system loader path.
# Resolve only this sandbox's fixed /app/lib, without global LD_LIBRARY_PATH or
# widening the native $ORIGIN-only contract to a mutable build/host directory.
set(SWIPEKB_BUNDLE_LIBDIR "" CACHE PATH "Additional Flatpak-local native library directory")
if(SWIPEKB_BUNDLE_LIBDIR)
    if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/app" OR NOT SWIPEKB_BUNDLE_LIBDIR STREQUAL "/app/lib")
        message(FATAL_ERROR "Bundled library lookup is supported only for /app/lib under the /app prefix")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        message(FATAL_ERROR "This pinned Flatpak SDK library profile supports x86_64 only")
    endif()
    # WHY: the observed KDE SDK installs QtVirtualKeyboard into the Debian-style
    # multiarch subdirectory despite CMAKE_INSTALL_LIBDIR=lib. Both locations
    # are private /app roots; neither searches mutable host/build directories.
    set_property(TARGET swipekeyboard swipekeyboardplugin swipekb_engine
        APPEND PROPERTY INSTALL_RPATH ${SWIPEKB_BUNDLE_LIBDIR}
        "${SWIPEKB_BUNDLE_LIBDIR}/x86_64-linux-gnu")
endif()

# The blur shape (rounded corners as strips) is checked on its own, without a compositor.
qt_add_executable(tst_windowblur tests/tst_windowblur.cpp windowblur.cpp)
target_include_directories(tst_windowblur PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_windowblur PRIVATE Qt6::Quick Qt6::Test KF6::WindowSystem)
add_test(NAME plugin_windowblur COMMAND tst_windowblur)
set_tests_properties(plugin_windowblur PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(tst_pluginload tests/tst_pluginload.cpp)
target_link_libraries(tst_pluginload PRIVATE Qt6::Qml Qt6::Quick Qt6::Test Qt6::VirtualKeyboard)
target_compile_definitions(tst_pluginload PRIVATE SWIPEKB_QML_DIR="${SWIPEKB_QML_DIR}")
add_dependencies(tst_pluginload swipekeyboardplugin)
add_test(NAME plugin_load COMMAND tst_pluginload)
set_tests_properties(plugin_load PROPERTIES ENVIRONMENT "SWIPEKB_DATA=${SWIPEKB_ASSETS};QT_QPA_PLATFORM=offscreen")

# Load the distro's real InputPanel/layouts, not just standalone plugin components.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME plugin_public_qt_abi
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/package-check.py --elf
        $<TARGET_FILE:swipekeyboard> $<TARGET_FILE:swipekeyboardplugin>
        $<TARGET_FILE:swipekb_engine> $<TARGET_FILE:swipekb-preflight> $<TARGET_FILE:swipekb-config>)
add_test(NAME plugin_native_preflight
    COMMAND swipekb-preflight $<TARGET_FILE:swipekeyboardplugin>)
add_test(NAME plugin_preflight_failures
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/packaging/test_elf_compatibility.py
        ElfCompatibility.test_real_preflight_rejects_missing_library_and_unresolved_symbol)
set_tests_properties(plugin_preflight_failures PROPERTIES
    ENVIRONMENT "SWIPEKB_TEST_PREFLIGHT=$<TARGET_FILE:swipekb-preflight>")
qt_add_executable(tst_stocklayout tests/tst_stocklayout.cpp)
target_link_libraries(tst_stocklayout PRIVATE Qt6::Qml Qt6::Quick Qt6::Test)
add_dependencies(tst_stocklayout swipekeyboardplugin)
# WHY: Flatpak's bundled backend installs under /app; native builds retain the
# existing distro layout default, without testing an unrelated host backend.
set(SWIPEKB_STOCK_LAYOUTS "/usr/share/plasma/keyboard/layouts" CACHE PATH "Backend stock layout directory for loader tests")
set(SWIPEKB_BACKEND_QML_DIR "" CACHE PATH "Explicit bundled backend QML root for loader tests")
set(_swipekb_backend_import_args)
if(SWIPEKB_BACKEND_QML_DIR)
    list(APPEND _swipekb_backend_import_args --backend-qml-dir ${SWIPEKB_BACKEND_QML_DIR})
endif()
add_test(NAME plugin_stock_layout
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check-stock-layout.py
        $<TARGET_FILE:tst_stocklayout>
        --qml-dir ${SWIPEKB_QML_DIR} --model-data ${SWIPEKB_ASSETS}
        --stock-layouts ${SWIPEKB_STOCK_LAYOUTS} ${_swipekb_backend_import_args})
set_tests_properties(plugin_stock_layout PROPERTIES TIMEOUT 75)
