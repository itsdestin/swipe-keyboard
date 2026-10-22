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
    "SWIPEKB_GAMINGD=$<TARGET_FILE:swipekb-gamingd>;SWIPEKB_REPLAY_EXE=$<TARGET_FILE:swipekb-replay>;SWIPEKB_ASSETS=${SWIPEKB_ASSETS};SWIPEKB_FUTO_LAYOUT=${CMAKE_SOURCE_DIR}/third_party/swipe-library/models/layouts/en_qwerty.json;SWIPEKB_LANG_DATASET=${CMAKE_SOURCE_DIR}/bench/languages/synthetic-centered-v1.json;SWIPEKB_ORACLE_JSON=${CMAKE_BINARY_DIR}/plugin-language-oracle.json")
# WHY a fixture: the daemon test compares against what the real plugin shows, so the plugin test must run first.
set_tests_properties(gaming_daemon PROPERTIES FIXTURES_REQUIRED plugin_oracle)
add_dependencies(swipekb-gamingd swipekb-replay)
