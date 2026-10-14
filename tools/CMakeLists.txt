# swipekb-replay: re-runs recorded swipes (plugin/swiperecorder.h writes them) through the engine offline,
# so recognition settings can be compared on the owner's real swipes without touching the live keyboard.
# WHY Qt Core: it already reads the recorder's JSON, and the plugin needs Qt anyway, so no new dependency.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Test)
set(CMAKE_AUTOMOC ON)   # the test uses Q_OBJECT

add_executable(swipekb-replay swipekb-replay.cpp)
target_link_libraries(swipekb-replay PRIVATE swipekb_engine Qt6::Core)

# The test builds a tiny synthetic recording, runs the tool on it and checks the table and summary.
add_executable(tst_replay tests/tst_replay.cpp)
target_link_libraries(tst_replay PRIVATE Qt6::Core Qt6::Test)
target_compile_definitions(tst_replay PRIVATE SWIPEKB_REPLAY_EXE="$<TARGET_FILE:swipekb-replay>")
add_dependencies(tst_replay swipekb-replay)
add_test(NAME tools_replay COMMAND tst_replay)
set_tests_properties(tools_replay PROPERTIES ENVIRONMENT "SWIPEKB_ASSETS=${SWIPEKB_ASSETS}")
