find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick VirtualKeyboard Test)
qt_standard_project_setup(REQUIRES 6.8)

# Pure text logic, no keyboard or engine: unit-tested on its own.
add_library(swipekb_session STATIC swipesession.cpp)
target_include_directories(swipekb_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(swipekb_session PUBLIC Qt6::Core)

qt_add_executable(tst_swipesession tests/tst_swipesession.cpp)
target_link_libraries(tst_swipesession PRIVATE swipekb_session Qt6::Test)
add_test(NAME plugin_session COMMAND tst_swipesession)
