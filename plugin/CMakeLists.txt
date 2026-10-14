find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick VirtualKeyboard Test)
qt_standard_project_setup(REQUIRES 6.8)

# Pure text logic, no keyboard or engine: unit-tested on its own.
add_library(swipekb_session STATIC swipesession.cpp)
target_include_directories(swipekb_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(swipekb_session PUBLIC Qt6::Core)

qt_add_executable(tst_swipesession tests/tst_swipesession.cpp)
target_link_libraries(tst_swipesession PRIVATE swipekb_session Qt6::Test)
add_test(NAME plugin_session COMMAND tst_swipesession)

# The QML module Plasma Keyboard loads. The module folder holds qmldir, the plugin .so, the backing .so
# (libswipekeyboard) and the engine .so (libswipekb_engine), all found through $ORIGIN.
# WHY the backing library is SHARED: a static one makes Qt build a static plugin, which can't be loaded at runtime.
qt_add_library(swipekeyboard SHARED)
qt_add_qml_module(swipekeyboard
    URI SwipeKeyboard
    VERSION 1.0
    OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard
    QML_FILES SwipeCapture.qml
    SOURCES swipeinputmethod.h swipeinputmethod.cpp
)
target_link_libraries(swipekeyboard PRIVATE Qt6::Quick Qt6::VirtualKeyboard swipekb_session swipekb_engine)
# WHY SHARED + same output folder: a STATIC backing library makes the plugin static too, which Qt cannot load.
# The two .so files sit side by side so the module folder is self-contained and relocatable.
set_target_properties(swipekeyboard PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard)
# WHY BUILD_WITH_INSTALL_RPATH: the default build RUNPATH also lists the build dir, so an incomplete installed
# copy would silently load libs from there. Forcing "$ORIGIN" only makes a missing sibling lib fail loudly.
set_target_properties(swipekeyboard swipekeyboardplugin swipekb_engine PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "\$ORIGIN")

qt_add_executable(tst_pluginload tests/tst_pluginload.cpp)
target_link_libraries(tst_pluginload PRIVATE Qt6::Qml Qt6::Quick Qt6::Test)
target_compile_definitions(tst_pluginload PRIVATE SWIPEKB_QML_DIR="${SWIPEKB_QML_DIR}")
add_dependencies(tst_pluginload swipekeyboardplugin)
add_test(NAME plugin_load COMMAND tst_pluginload)
set_tests_properties(plugin_load PROPERTIES ENVIRONMENT "SWIPEKB_DATA=${SWIPEKB_ASSETS};QT_QPA_PLATFORM=offscreen")
