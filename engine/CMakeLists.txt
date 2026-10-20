# WHY Qt Core only: shared Unicode normalization/casing without a GUI/runtime keyboard dependency.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
add_library(swipekb_engine SHARED src/normalize.cpp src/context.cpp src/ngram.cpp src/recognizer.cpp src/spelling.cpp src/nlm_loader.cpp src/prediction.cpp)
target_include_directories(swipekb_engine PUBLIC include)
# WHY PRIVATE: FUTO's library forces -fno-rtti on whatever compiles against it. Keeping it private
# confines that to this .so, so the Qt plugin (which needs RTTI) is unaffected.
target_link_libraries(swipekb_engine PRIVATE swipe_decoder executorch Qt6::Core ${CMAKE_DL_LIBS})
# Sits next to the Qt plugin, which finds it through an $ORIGIN rpath.
set_target_properties(swipekb_engine PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard)

# The optional neural word model (include/swipekb/nlm.hpp). WHY a separate library the engine opens itself
# (nlm_loader.cpp) instead of linking it: if llama.cpp is missing at run time, only this feature is lost, not
# the keyboard. WHY optional at build time: without llama.cpp's development files the keyboard still builds;
# the engine then reports "neural model unavailable" and ranks as before.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LLAMA IMPORTED_TARGET llama)
endif()
if(LLAMA_FOUND)
    add_library(swipekb_nlm MODULE src/nlm.cpp)
    target_include_directories(swipekb_nlm PRIVATE include)
    target_link_libraries(swipekb_nlm PRIVATE PkgConfig::LLAMA)
    # WHY no "lib" prefix override: CMake names it libswipekb_nlm.so, the name nlm_loader.cpp opens.
    set_target_properties(swipekb_nlm PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${SWIPEKB_QML_DIR}/SwipeKeyboard
                                                 CXX_VISIBILITY_PRESET hidden)
    add_dependencies(swipekb_engine swipekb_nlm)
else()
    message(STATUS "llama.cpp not found: building without the optional neural word model")
endif()

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

# The n-gram model file + lookups and the re-ranking, on a tiny hand-made model (no downloaded assets needed).
add_executable(test_ngram tests/test_ngram.cpp)
target_link_libraries(test_ngram PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_ngram COMMAND test_ngram)

# The neural re-ranking: ranking rule with a stand-in scorer, fallbacks, and the real model when present.
add_executable(test_nlm tests/test_nlm.cpp)
target_link_libraries(test_nlm PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_nlm COMMAND test_nlm)
set_tests_properties(engine_nlm PROPERTIES ENVIRONMENT "SWIPEKB_ASSETS=${SWIPEKB_ASSETS}")

# Apostrophes back into guesses ("lets" -> "let's"), on a tiny hand-made dictionary (no downloaded assets needed).
add_executable(test_spelling tests/test_spelling.cpp)
target_link_libraries(test_spelling PRIVATE swipekb_engine GTest::gtest_main)
add_test(NAME engine_spelling COMMAND test_spelling)
