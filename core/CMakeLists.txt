# Themis FalconFOC Core standalone host project. Role: build the portable Core
# without the root firmware graph. Responsibilities: define the Core static
# library, expose its public include boundary, and register Host tests for
# transforms, SVPWM, voltage open loop, d/q current control, and motion loops.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-core
  VERSION 0.1.0
  LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_library(falconfoc-core STATIC src/foc_core.c src/foc_current.c src/foc_motion.c)
target_include_directories(falconfoc-core
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT WIN32 AND NOT MSVC)
  target_link_libraries(falconfoc-core PUBLIC m)
endif()

function(themis_add_falconfoc_test TEST_NAME TEST_SOURCE)
  add_executable(${TEST_NAME} ${TEST_SOURCE})
  target_link_libraries(${TEST_NAME} PRIVATE falconfoc-core)
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

themis_add_falconfoc_test(test-foc-math tests/test_foc_math.c)
themis_add_falconfoc_test(test-foc-svpwm tests/test_foc_svpwm.c)
themis_add_falconfoc_test(test-foc-core tests/test_foc_core.c)
themis_add_falconfoc_test(test-foc-current tests/test_foc_current.c)
themis_add_falconfoc_test(test-foc-motion tests/test_foc_motion.c)
