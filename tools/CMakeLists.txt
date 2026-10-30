# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

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
# WHY the plugin's profile library: the replay sets the engine up exactly like the keyboard and the daemon.
target_link_libraries(swipekb-replay PRIVATE swipekb_engine swipekb_language Qt6::Core)

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

# swipekb-bench: the swipe scoreboard's engine. Replays recorded swipes through swipekb::LanguageEngine (the class the
# keyboard uses), so scores measure what ships (bench/score.sh).
add_executable(swipekb-bench swipekb-bench.cpp)
target_link_libraries(swipekb-bench PRIVATE swipekb_engine Qt6::Core)
# swipekb-sentence-check: exact spelling (apostrophes count) of swipes and next-word suggestions in held-out sentences.
add_executable(swipekb-sentence-check swipekb-sentence-check.cpp)
target_link_libraries(swipekb-sentence-check PRIVATE swipekb_engine Qt6::Core)
# swipekb-shadow-words: rare added words that would win a common word's clean swipe (word-list build, --drop lists).
add_executable(swipekb-shadow-words swipekb-shadow-words.cpp)
target_link_libraries(swipekb-shadow-words PRIVATE swipekb_engine Qt6::Core)
# The swipe engine's own scoring tool (network + search settings sweeps, per-board numbers).
add_executable(swipekb-swipenet-bench swipekb-swipenet-bench.cpp)
target_link_libraries(swipekb-swipenet-bench PRIVATE swipekb_engine Qt6::Core)

# Swap scoreboard (card #500): the five swap measures are plain counting, tested on their own.
add_library(swapmeasures STATIC swapmeasures.cpp)
target_include_directories(swapmeasures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_executable(tst_swapmeasures tests/tst_swapmeasures.cpp)
target_link_libraries(tst_swapmeasures PRIVATE swapmeasures Qt6::Core Qt6::Test)
add_test(NAME tools_swapmeasures COMMAND tst_swapmeasures)
# The junk rule both scoreboards share (benchdata.h): its English thresholds are pinned against the shipped list.
add_executable(tst_junkrule tests/tst_junkrule.cpp)
target_link_libraries(tst_junkrule PRIVATE swipekb_engine Qt6::Core Qt6::Test)
add_test(NAME tools_junkrule COMMAND tst_junkrule)
set_tests_properties(tools_junkrule PROPERTIES ENVIRONMENT "SWIPEKB_ASSETS=${SWIPEKB_ASSETS}")
# swipekb-swap-bench: the swap scoreboard's engine (card #500). Baseline mode runs the shipped WordSwap::build itself.
add_executable(swipekb-swap-bench swipekb-swap-bench.cpp)
target_link_libraries(swipekb-swap-bench PRIVATE swipekb_engine swipekb_session swipekb_swapflow swipekb_language swapmeasures Qt6::Core)

# swipekb-rescore-bench: joint-model experiment step 1 (headroom of re-ranking a swipe, tuning dumps and the speed of
# re-scoring swipes with the neural next-word model; ContextRescore).
# WHY the plugin's next-word worker compiled in (step 2, --product): the speed is measured through the very code the
# keyboard runs, the swipe borrowing the worker's model under its lock.
add_executable(swipekb-rescore-bench swipekb-rescore-bench.cpp ${PROJECT_SOURCE_DIR}/plugin/nextwordworker.cpp
    ${PROJECT_SOURCE_DIR}/plugin/nextwordworker.h)
target_include_directories(swipekb-rescore-bench PRIVATE ${PROJECT_SOURCE_DIR}/plugin)
target_link_libraries(swipekb-rescore-bench PRIVATE swipekb_engine swipekb_language swipekb_session Qt6::Core)

# The user-experience scoreboard's typing half (bench/ux-score.sh): the typing simulator is plain counting, tested with
# made-up bars; swipekb-typing-bench feeds it the bar both front ends build (plugin/barflow.h).
add_library(typingsim STATIC typingsim.cpp)
target_include_directories(typingsim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(typingsim PUBLIC swipekb_engine Qt6::Core)
add_executable(tst_typingsim tests/tst_typingsim.cpp)
target_link_libraries(tst_typingsim PRIVATE typingsim Qt6::Test)
add_test(NAME tools_typingsim COMMAND tst_typingsim)
add_executable(swipekb-typing-bench swipekb-typing-bench.cpp)
target_link_libraries(swipekb-typing-bench PRIVATE typingsim swipekb_swapflow swipekb_language swipekb_engine Qt6::Core)
# The owner's recordings reader (recordings.h) and the swap bench's --recordings mode: truth rules, and that it only reads.
add_executable(tst_recordings tests/tst_recordings.cpp)
target_link_libraries(tst_recordings PRIVATE swipekb_engine Qt6::Core Qt6::Test)
target_compile_definitions(tst_recordings PRIVATE SWIPEKB_SWAP_BENCH_EXE="$<TARGET_FILE:swipekb-swap-bench>")
add_dependencies(tst_recordings swipekb-swap-bench)
add_test(NAME tools_recordings COMMAND tst_recordings)
set_tests_properties(tools_recordings PROPERTIES ENVIRONMENT "SWIPEKB_ASSETS=${SWIPEKB_ASSETS}")
