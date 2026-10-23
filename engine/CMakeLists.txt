# WHY Qt Core only: shared Unicode normalization/casing without a GUI/runtime keyboard dependency.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
add_library(swipekb_engine SHARED src/normalize.cpp src/context.cpp src/ngram.cpp src/recognizer.cpp src/spelling.cpp src/ngram_predictor.cpp src/prediction.cpp)
# WHY its own line: the personal-learning layer (card: open-engine Phase 2) stays separate from the predictors above.
target_sources(swipekb_engine PRIVATE src/learning.cpp)
target_include_directories(swipekb_engine PUBLIC include)
# WHY PRIVATE: FUTO's library forces -fno-rtti on whatever compiles against it. Keeping it private
# confines that to this .so, so the Qt plugin (which needs RTTI) is unaffected.
target_link_libraries(swipekb_engine PRIVATE swipe_decoder executorch Qt6::Core ${CMAKE_DL_LIBS})
# Sits next to the Qt plugin, which finds it through an $ORIGIN rpath.
set_target_properties(swipekb_engine PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard)

add_executable(test_normalize tests/test_normalize.cpp)
target_link_libraries(test_normalize PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_normalize COMMAND test_normalize)

add_executable(test_recognizer tests/test_recognizer.cpp)
# WHY explicit: the multilingual completion fixture uses QTemporaryDir; the engine's Qt dependency is private.
target_link_libraries(test_recognizer PRIVATE swipekb_engine GTest::gtest_main Qt6::Core)
add_test(NAME engine_recognizer COMMAND test_recognizer)
set_tests_properties(engine_recognizer PROPERTIES ENVIRONMENT "SWIPEKB_ASSETS=${SWIPEKB_ASSETS}")

add_executable(test_context tests/test_context.cpp)
target_link_libraries(test_context PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_context COMMAND test_context)

# The n-gram model file + lookups and the re-ranking, on a tiny hand-made model (no downloaded assets needed).
add_executable(test_ngram tests/test_ngram.cpp)
target_link_libraries(test_ngram PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_ngram COMMAND test_ngram)

# Apostrophes back into guesses ("lets" -> "let's"), on a tiny hand-made dictionary (no downloaded assets needed).
add_executable(test_spelling tests/test_spelling.cpp)
target_link_libraries(test_spelling PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_spelling COMMAND test_spelling)
