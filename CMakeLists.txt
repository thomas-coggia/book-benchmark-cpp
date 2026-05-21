cmake_minimum_required(VERSION 3.23)

project(matching VERSION 0.1.0 LANGUAGES CXX)

# --- Compiler hardening -------------------------------------------------------------------
# Errors for categories that catch real bugs; warnings for merely fishy ones. Sanitizer
# flags come from Conan profile [conf] tools.build:cxxflags via CMAKE_*_FLAGS_INIT.
set(MATCHING_FLAGS_ERROR
  "-Werror=all -Werror=suggest-override -Werror=pedantic -Werror=missing-field-initializers -Werror=sign-compare -Wno-unused"
)
set(MATCHING_FLAGS_WARNING
  "-Wimplicit-fallthrough -Wunused -Wdeprecated -Wignored-qualifiers"
)
set(MATCHING_COMMON_FLAGS
  "${MATCHING_FLAGS_ERROR} ${MATCHING_FLAGS_WARNING} -ftemplate-backtrace-limit=0"
)

set(CMAKE_C_FLAGS_RELEASE "${MATCHING_COMMON_FLAGS} -O3 -DNDEBUG -ggdb -march=native")
set(CMAKE_CXX_FLAGS_RELEASE "${MATCHING_COMMON_FLAGS} -O3 -DNDEBUG -ggdb -march=native")
set(CMAKE_C_FLAGS_DEBUG "${MATCHING_COMMON_FLAGS} -O1 -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_DEBUG "${MATCHING_COMMON_FLAGS} -O1 -g -fno-omit-frame-pointer")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)

# --- Mostly header-only library + signal_handler ------------------------------------------
# matching_runtime is the single non-header TU: it owns the process-global std::stop_source
# the signal handler flips on SIGINT/SIGTERM. Everything else under include/matching/ stays
# header-only.
find_package(Threads REQUIRED)
find_package(cxxopts REQUIRED CONFIG)

add_library(matching_runtime STATIC src/signal_handler.cpp)
target_include_directories(matching_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(matching_runtime PUBLIC cxx_std_23)
target_link_libraries(matching_runtime PUBLIC Threads::Threads)

add_library(matching::matching ALIAS matching_runtime)

# Compile shim so compile_commands.json picks up every header for clangd / clang-tidy.
add_library(matching_compile STATIC
  include/matching/compile.cpp
)
target_link_libraries(matching_compile PRIVATE matching::matching)
target_compile_features(matching_compile PRIVATE cxx_std_23)

# --- Application binary -------------------------------------------------------------------
add_executable(matching_engine src/matching_engine.cpp)
target_link_libraries(matching_engine PRIVATE matching::matching cxxopts::cxxopts)
target_compile_features(matching_engine PRIVATE cxx_std_23)

# --- Benchmark binary ---------------------------------------------------------------------
add_executable(benchmark src/benchmark.cpp)
target_link_libraries(benchmark PRIVATE matching::matching cxxopts::cxxopts)
target_compile_features(benchmark PRIVATE cxx_std_23)

# --- Tests --------------------------------------------------------------------------------
enable_testing()
add_subdirectory(test)
