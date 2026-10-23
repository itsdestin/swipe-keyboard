# swipekb-replay: re-runs recorded swipes (plugin/swiperecorder.h writes them) through the engine offline,
# so recognition settings can be compared on the owner's real swipes without touching the live keyboard.
# WHY Qt Core: it already reads the recorder's JSON, and the plugin needs Qt anyway, so no new dependency.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Test)
set(CMAKE_AUTOMOC ON)   # the test uses Q_OBJECT

# WHY keep deterministic accounting and fixture validation in ordinary CTest, not only the feature build.
add_executable(swipekb-language-bench swipekb-language-bench.cpp languageevaluation.cpp)
target_link_libraries(swipekb-language-bench PRIVATE swipekb_engine swipekb_language)
add_executable(tst_languageevaluation tests/tst_languageevaluation.cpp languageevaluation.cpp)
target_include_directories(tst_languageevaluation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_languageevaluation PRIVATE swipekb_language Qt6::Test)
add_test(NAME tools_languageevaluation COMMAND tst_languageevaluation)

add_executable(swipekb-replay swipekb-replay.cpp)
target_link_libraries(swipekb-replay PRIVATE swipekb_engine Qt6::Core)

# The test builds a tiny synthetic recording, runs the tool on it and checks the table and summary.
add_executable(tst_replay tests/tst_replay.cpp)
target_link_libraries(tst_replay PRIVATE Qt6::Core Qt6::Test)
target_compile_definitions(tst_replay PRIVATE SWIPEKB_REPLAY_EXE="$<TARGET_FILE:swipekb-replay>")
add_dependencies(tst_replay swipekb-replay)
add_test(NAME tools_replay COMMAND tst_replay)
set_tests_properties(tools_replay PROPERTIES ENVIRONMENT "SWIPEKB_ASSETS=${SWIPEKB_ASSETS}")

# swipe-stats: daily "how often did a swipe need fixing" counts from the recordings (Python, stdlib only).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME tools_language_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_language_cli.py)
    set_tests_properties(tools_language_cli PROPERTIES ENVIRONMENT
        "SWIPEKB_PROFILECHECK_EXE=$<TARGET_FILE:swipekb-profilecheck>;SWIPEKB_LANGUAGE_BENCH_EXE=$<TARGET_FILE:swipekb-language-bench>")
    add_test(NAME tools_swipe_stats COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_swipe_stats.py)
endif()

# swipekb-bench: the scoreboard's engine. Replays FUTO's real-swipe test file through our swipekb::Recognizer
# (bench/score.sh --runner recognizer), so scores measure what ships.
set(SWIPEKB_FUTO_LAYOUT ${CMAKE_SOURCE_DIR}/third_party/swipe-library/models/layouts/en_qwerty.json)
add_executable(swipekb-bench swipekb-bench.cpp)
target_include_directories(swipekb-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(swipekb-bench PRIVATE swipekb_engine Qt6::Core)
target_compile_definitions(swipekb-bench PRIVATE SWIPEKB_DEFAULT_LAYOUT="${SWIPEKB_FUTO_LAYOUT}")

# Checks that the key rectangles built from FUTO's layout file normalise back to FUTO's key centres.
add_executable(tst_benchlayout tests/tst_benchlayout.cpp)
target_include_directories(tst_benchlayout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_benchlayout PRIVATE swipekb_engine Qt6::Core Qt6::Test)
target_compile_definitions(tst_benchlayout PRIVATE SWIPEKB_LAYOUT_JSON="${SWIPEKB_FUTO_LAYOUT}")
add_test(NAME tools_benchlayout COMMAND tst_benchlayout)
