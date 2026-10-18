find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick VirtualKeyboard Test)
# WHY KWindowSystem: WindowBlur asks KWin to blur behind the keyboard (KWindowEffects), the same call Plasma's
# panels use. It ships with Plasma, so it is always there wherever Plasma Keyboard runs.
find_package(KF6WindowSystem REQUIRED)
qt_standard_project_setup(REQUIRES 6.8)

# WHY Core-only: activation must validate Qt metadata and all ELF relocations
# without a compositor, QML engine or access to the user's running keyboard.
qt_add_executable(swipekb-preflight preflight.cpp)
target_link_libraries(swipekb-preflight PRIVATE Qt6::Core)
install(TARGETS swipekb-preflight RUNTIME DESTINATION share/swipe-keyboard/integration)

# Pure text logic, no keyboard or engine: unit-tested on its own.
add_library(swipekb_session STATIC swipesession.cpp swipeoutcome.cpp)
target_include_directories(swipekb_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# WHY the engine: SwipeSession::contextWords forwards to swipekb::contextWords (one shared implementation).
target_link_libraries(swipekb_session PUBLIC Qt6::Core swipekb_engine)

qt_add_executable(tst_swipesession tests/tst_swipesession.cpp)
target_link_libraries(tst_swipesession PRIVATE swipekb_session Qt6::Test)
add_test(NAME plugin_session COMMAND tst_swipesession)

qt_add_executable(tst_swipeoutcome tests/tst_swipeoutcome.cpp)
target_link_libraries(tst_swipeoutcome PRIVATE swipekb_session Qt6::Test)
add_test(NAME plugin_outcome COMMAND tst_swipeoutcome)

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
    SOURCES swipeinputmethod.h swipeinputmethod.cpp windowblur.h windowblur.cpp
)
target_link_libraries(swipekeyboard PRIVATE Qt6::Quick Qt6::VirtualKeyboard KF6::WindowSystem swipekb_session swipekb_recorder swipekb_engine)
# WHY SHARED + same output folder: a STATIC backing library makes the plugin static too, which Qt cannot load.
# The three .so files (plugin, backing library, engine) sit side by side so the module folder is
# self-contained and relocatable.
set_target_properties(swipekeyboard PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard)
# WHY BUILD_WITH_INSTALL_RPATH: the default build RUNPATH also lists the build dir, so an incomplete installed
# copy would silently load libs from there. Forcing "$ORIGIN" only makes a missing sibling lib fail loudly.
set_target_properties(swipekeyboard swipekeyboardplugin swipekb_engine PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "\$ORIGIN")

# The blur shape (rounded corners as strips) is checked on its own, without a compositor.
qt_add_executable(tst_windowblur tests/tst_windowblur.cpp windowblur.cpp)
target_include_directories(tst_windowblur PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_windowblur PRIVATE Qt6::Quick Qt6::Test KF6::WindowSystem)
add_test(NAME plugin_windowblur COMMAND tst_windowblur)
set_tests_properties(plugin_windowblur PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(tst_pluginload tests/tst_pluginload.cpp)
target_link_libraries(tst_pluginload PRIVATE Qt6::Qml Qt6::Quick Qt6::Test)
target_compile_definitions(tst_pluginload PRIVATE SWIPEKB_QML_DIR="${SWIPEKB_QML_DIR}")
add_dependencies(tst_pluginload swipekeyboardplugin)
add_test(NAME plugin_load COMMAND tst_pluginload)
set_tests_properties(plugin_load PROPERTIES ENVIRONMENT "SWIPEKB_DATA=${SWIPEKB_ASSETS};QT_QPA_PLATFORM=offscreen")

# Load the distro's real InputPanel/layouts, not just standalone plugin components.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME plugin_public_qt_abi
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/package-check.py --elf
        $<TARGET_FILE:swipekeyboard> $<TARGET_FILE:swipekeyboardplugin>
        $<TARGET_FILE:swipekb_engine> $<TARGET_FILE:swipekb-preflight>)
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
add_test(NAME plugin_stock_layout
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check-stock-layout.py
        $<TARGET_FILE:tst_stocklayout>
        --qml-dir ${SWIPEKB_QML_DIR} --model-data ${SWIPEKB_ASSETS})
set_tests_properties(plugin_stock_layout PROPERTIES TIMEOUT 75)
