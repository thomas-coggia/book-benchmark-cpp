# GoogleTest is fetched via Conan (declared as a test_requires in conanfile.py); the
# generated CMakeDeps make it available through find_package().
find_package(GTest REQUIRED)

include(GoogleTest)

set(MATCHING_REPO_ROOT "${CMAKE_SOURCE_DIR}")
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/generated/matching_test_res_dir.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/matching_test_res_dir.hpp"
  @ONLY
)

set(MATCHING_TEST_FILES
  test_order_book.cpp
  test_input_parser.cpp
  test_order_generator.cpp
  test_spsc_queue.cpp
  test_agent.cpp
  test_engine_pipeline.cpp
  test_invariants.cpp
)

set(MATCHING_TEST_TARGETS "")
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
  list(APPEND MATCHING_TEST_TARGETS ${test_name})
endforeach()

target_include_directories(test_engine_pipeline PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Optionally install the test layout so the `test_package` Conan recipe can re-run the suite
# against the produced package. Kept opt-in (default OFF) so production deployments don't ship
# test binaries unless the workflow asks for them — Conan's `test_package` flow flips this on.
option(MATCHING_INSTALL_TESTS "Install test binaries and CTest config alongside the package" OFF)
if(MATCHING_INSTALL_TESTS)
  include(GNUInstallDirs)
  set(MATCHING_TEST_INSTALL_ROOT "${CMAKE_INSTALL_DATAROOTDIR}/matching/test")
  install(TARGETS ${MATCHING_TEST_TARGETS}
          RUNTIME DESTINATION "${MATCHING_TEST_INSTALL_ROOT}/bin")
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/res/"
          DESTINATION "${MATCHING_TEST_INSTALL_ROOT}/res")
  # Hand-written CTestTestfile.cmake: lets ctest --test-dir <install>/share/matching/test/
  # drive the installed binaries directly. We resolve paths relative to the file's own
  # location so the layout stays relocatable across cache moves.
  set(MATCHING_CTEST_FILE "${CMAKE_CURRENT_BINARY_DIR}/installed_CTestTestfile.cmake")
  file(WRITE "${MATCHING_CTEST_FILE}"
       "# Generated CTest harness for the installed matching test layout.\n"
       "get_filename_component(MATCHING_TEST_ROOT \"\${CMAKE_CURRENT_LIST_DIR}\" ABSOLUTE)\n"
  )
  foreach(test_target IN LISTS MATCHING_TEST_TARGETS)
    file(APPEND "${MATCHING_CTEST_FILE}"
         "add_test(NAME ${test_target} COMMAND \"\${MATCHING_TEST_ROOT}/bin/${test_target}\")\n"
         "set_tests_properties(${test_target} PROPERTIES ENVIRONMENT \"MATCHING_TEST_RES_DIR=\${MATCHING_TEST_ROOT}/res\")\n"
    )
  endforeach()
  install(FILES "${MATCHING_CTEST_FILE}"
          DESTINATION "${MATCHING_TEST_INSTALL_ROOT}"
          RENAME "CTestTestfile.cmake")
endif()

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
