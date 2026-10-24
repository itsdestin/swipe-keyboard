# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: BSD-3-Clause

# swipekb-gamingd: the Gaming Mode engine daemon (plan Section 4). Qt Core only (JSON); no GUI, no Network module.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
add_library(gamingd_protocol STATIC protocol.cpp)
target_include_directories(gamingd_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gamingd_protocol PUBLIC swipekb_engine Qt6::Core)

# Language choice. WHY it links the plugin's swipekb_language / swipekb_session libraries: the daemon must resolve
# profiles and rank candidates with the plugin's own code, not a copy (plan Section 13, milestone M7).
add_library(gamingd_languages STATIC languages.cpp)
target_include_directories(gamingd_languages PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gamingd_languages PUBLIC gamingd_protocol swipekb_language swipekb_session)

add_executable(swipekb-gamingd main.cpp server.cpp)
target_link_libraries(swipekb-gamingd PRIVATE gamingd_protocol gamingd_languages)
# WHY $ORIGIN paths: the engine library sits next to the Qt plugin in the module folder (build tree and install).
set_target_properties(swipekb-gamingd PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\$ORIGIN/../share/swipe-keyboard/qml/SwipeKeyboard;${SWIPEKB_QML_DIR}/SwipeKeyboard")

# WHY off by default: native Arch/Fedora packages must ship nothing under gaming/ (plan Section 4). The Flatpak
# build turns it on (packaging/flatpak/build-swipe.sh).
option(SWIPEKB_INSTALL_GAMINGD "Install swipekb-gamingd (Flatpak only)" OFF)
if(SWIPEKB_INSTALL_GAMINGD)
    install(TARGETS swipekb-gamingd RUNTIME DESTINATION bin)
endif()

add_executable(test_gamingd_protocol ${CMAKE_SOURCE_DIR}/tests/gaming/test_protocol.cpp)
target_link_libraries(test_gamingd_protocol PRIVATE gamingd_protocol GTest::gtest_main)
add_test(NAME gaming_protocol COMMAND test_gamingd_protocol)

add_executable(test_gamingd_languages ${CMAKE_SOURCE_DIR}/tests/gaming/test_languages.cpp)
target_link_libraries(test_gamingd_languages PRIVATE gamingd_languages GTest::gtest_main)
add_test(NAME gaming_languages COMMAND test_gamingd_languages)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME gaming_daemon COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/gaming/test_daemon.py)
set_tests_properties(gaming_daemon PROPERTIES TIMEOUT 600 ENVIRONMENT
    "SWIPEKB_GAMINGD=$<TARGET_FILE:swipekb-gamingd>;SWIPEKB_REPLAY_EXE=$<TARGET_FILE:swipekb-replay>;SWIPEKB_ASSETS=${SWIPEKB_ASSETS};SWIPEKB_LAYOUT_TSV=${CMAKE_SOURCE_DIR}/scripts/swipe/layouts/qwerty.tsv;SWIPEKB_LANG_DATASET=${CMAKE_SOURCE_DIR}/bench/languages/synthetic-centered-v1.json;SWIPEKB_ORACLE_JSON=${CMAKE_BINARY_DIR}/plugin-language-oracle.json")
# WHY a fixture: the daemon test compares against what the real plugin shows, so the plugin test must run first.
set_tests_properties(gaming_daemon PROPERTIES FIXTURES_REQUIRED plugin_oracle)
add_dependencies(swipekb-gamingd swipekb-replay)

# M5: the real helper talking to this daemon through a fake Steam (tests/gaming/test_commit_e2e.py). WHY here: it needs the built daemon.
# The helper-only tests (test_commit.py, test_helper.py, test_layer.py) need no build and are run directly with python3 -I.
# WHY only when Node is found: the fake Steam page runs in Node, and a plain native-package build machine may not have it. A
# missing tool must not turn the whole package build red, but it must not pass quietly either, so the gap is printed here and
# the test runs in every job whose build has Node (see the arch and fedora jobs in .github/workflows/packages.yml).
find_program(SWIPEKB_NODE node)
if(SWIPEKB_NODE)
    add_test(NAME gaming_commit_e2e COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/gaming/test_commit_e2e.py)
    set_tests_properties(gaming_commit_e2e PROPERTIES TIMEOUT 600 ENVIRONMENT
        "SWIPEKB_GAMINGD=$<TARGET_FILE:swipekb-gamingd>;SWIPEKB_REPLAY_EXE=$<TARGET_FILE:swipekb-replay>;SWIPEKB_ASSETS=${SWIPEKB_ASSETS};SWIPEKB_LAYOUT_TSV=${CMAKE_SOURCE_DIR}/scripts/swipe/layouts/qwerty.tsv")
else()
    message(STATUS "gaming_commit_e2e NOT registered: Node.js was not found (the fake Steam page needs it)")
endif()
