add_library(swipekb_engine SHARED src/normalize.cpp src/context.cpp src/recognizer.cpp)
target_include_directories(swipekb_engine PUBLIC include)
# WHY PRIVATE: FUTO's library forces -fno-rtti on whatever compiles against it. Keeping it private
# confines that to this .so, so the Qt plugin (which needs RTTI) is unaffected.
target_link_libraries(swipekb_engine PRIVATE swipe_decoder executorch)
# Sits next to the Qt plugin, which finds it through an $ORIGIN rpath.
set_target_properties(swipekb_engine PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard)

add_executable(test_normalize tests/test_normalize.cpp)
target_link_libraries(test_normalize PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_normalize COMMAND test_normalize)

add_executable(test_recognizer tests/test_recognizer.cpp)
target_link_libraries(test_recognizer PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_recognizer COMMAND test_recognizer)
set_tests_properties(engine_recognizer PROPERTIES ENVIRONMENT "SWIPEKB_ASSETS=${SWIPEKB_ASSETS}")

add_executable(test_context tests/test_context.cpp)
target_link_libraries(test_context PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_context COMMAND test_context)
