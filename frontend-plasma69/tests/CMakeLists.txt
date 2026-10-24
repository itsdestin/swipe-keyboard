# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
# SPDX-License-Identifier: BSD-2-Clause

# The controller's rules against a fake text field and the real (FUTO-free) engine files.
set(SWIPEKB_TEST_MODEL_DATA "" CACHE PATH "Installed-layout data folder (dict/, models/, language-packs/) for the 6.9 tests")
# WHY default to the build's own assets: the full build already fetched them in the installed layout.
if(NOT SWIPEKB_TEST_MODEL_DATA AND SWIPEKB_ASSETS AND EXISTS "${SWIPEKB_ASSETS}/dict/en_swipe.combined")
    set(SWIPEKB_TEST_MODEL_DATA "${SWIPEKB_ASSETS}")
endif()
qt_add_executable(tst_plasma69_controller tst_controller.cpp fakeeditor.h)
target_link_libraries(tst_plasma69_controller PRIVATE swipekb69_open Qt6::Test)
add_test(NAME plasma69_controller COMMAND tst_plasma69_controller)
set_tests_properties(plasma69_controller PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
    ENVIRONMENT "SWIPEKB_TEST_MODEL_DATA=${SWIPEKB_TEST_MODEL_DATA};QT_QPA_PLATFORM=offscreen")

# WHY a symbol check: the 6.9 module must never carry FUTO's or ExecuTorch's code (owner decision 2026-10-23).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME plasma69_no_futo
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_no_futo.py
        $<TARGET_FILE:swipekeyboard69> $<TARGET_FILE:swipekeyboard69plugin>)

# Against KDE's real keyboard core (plasma-keyboard master), built in a scratch prefix: see harness/README.
set(SWIPEKB_PLASMA_KEYBOARD_SOURCE "" CACHE PATH "plasma-keyboard (6.8.80+) source tree for the end-to-end tests")
if(SWIPEKB_PLASMA_KEYBOARD_SOURCE)
    add_subdirectory(harness)
endif()
