# WHY Qt Core only: shared Unicode normalization/casing without a GUI/runtime keyboard dependency.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
add_library(swipekb_engine SHARED src/normalize.cpp src/context.cpp src/ngram.cpp src/spelling.cpp src/ngram_predictor.cpp src/nwp_model.cpp src/nwp_predictor.cpp src/prediction.cpp)
# WHY its own line: the personal-learning layer (card: open-engine Phase 2) stays separate from the predictors above.
target_sources(swipekb_engine PRIVATE src/learning.cpp)
# The swipe engine (network + dictionary search + geometry) and the LanguageEngine facade over all of it.
target_sources(swipekb_engine PRIVATE src/swipe_layout.cpp src/swipe_net.cpp src/swipe_beam.cpp src/swipe_geo.cpp
    src/swipe_engine.cpp src/swipe_lm.cpp src/language_engine.cpp)
target_include_directories(swipekb_engine PUBLIC include)
# WHY Qt Core is PUBLIC: Unicode normalisation and casing come from Qt, and tests/tools that use QString with the
# engine headers then need nothing extra.
target_link_libraries(swipekb_engine PUBLIC Qt6::Core)
# Sits next to the Qt plugin, which finds it through an $ORIGIN rpath.
set_target_properties(swipekb_engine PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard)

add_executable(test_normalize tests/test_normalize.cpp)
target_link_libraries(test_normalize PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_normalize COMMAND test_normalize)

add_executable(test_language_engine tests/test_language_engine.cpp)
target_link_libraries(test_language_engine PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_language COMMAND test_language_engine)
set_tests_properties(engine_language PROPERTIES ENVIRONMENT "SWIPEKB_ASSETS=${SWIPEKB_ASSETS}")

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
