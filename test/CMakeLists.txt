# GoogleTest is fetched via Conan (declared as a test_requires in conanfile.py); the
# generated CMakeDeps make it available through find_package().
find_package(GTest REQUIRED)

include(GoogleTest)

set(MATCHING_TEST_FILES
  test_order_book.cpp
  test_input_parser.cpp
  test_order_generator.cpp
  test_spsc_queue.cpp
  test_agent.cpp
  test_engine_pipeline.cpp
  test_invariants.cpp
)

foreach(test_file IN LISTS MATCHING_TEST_FILES)
  get_filename_component(test_name "${test_file}" NAME_WE)
  add_executable(${test_name} ${test_file})
  target_link_libraries(${test_name}
    PRIVATE
      matching::matching
      GTest::gtest
      GTest::gtest_main
  )
  target_compile_features(${test_name} PRIVATE cxx_std_23)
  gtest_discover_tests(${test_name})
endforeach()

# Fuzzer target: opt-in via -DBUILD_FUZZERS=ON. Requires a clang toolchain with libFuzzer
# (-fsanitize=fuzzer). Not part of ctest — run manually with a corpus / time budget.
option(BUILD_FUZZERS "Build libFuzzer-based fuzzers (requires -fsanitize=fuzzer)" OFF)
if(BUILD_FUZZERS)
  add_executable(fuzz_input_parser fuzz_input_parser.cpp)
  target_link_libraries(fuzz_input_parser PRIVATE matching::matching)
  target_compile_features(fuzz_input_parser PRIVATE cxx_std_23)
  target_compile_options(fuzz_input_parser PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz_input_parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
