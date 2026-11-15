# Themis FalconFOC Core standalone host project. Role: build the portable Core
# without the root firmware graph. Responsibilities: define the Core static
# library, expose its public include boundary, and register Host tests for
# transforms, SVPWM, voltage open loop, d/q current control, and motion loops.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-core
  VERSION 0.1.0
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_library(
  falconfoc-core STATIC src/foc_core.c src/foc_current.c src/foc_motion.c src/foc_trajectory.c
)
target_include_directories(falconfoc-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

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
themis_add_falconfoc_test(test-foc-math-reference tests/test_foc_math_reference.c)
themis_add_falconfoc_test(test-foc-svpwm tests/test_foc_svpwm.c)
themis_add_falconfoc_test(test-foc-core tests/test_foc_core.c)
themis_add_falconfoc_test(test-foc-current tests/test_foc_current.c)
themis_add_falconfoc_test(test-foc-current-reference tests/test_foc_current_reference.c)
themis_add_falconfoc_test(test-foc-pmsm-reference tests/test_foc_pmsm_reference.c)
themis_add_falconfoc_test(test-foc-motion tests/test_foc_motion.c)
themis_add_falconfoc_test(test-foc-motion-reference tests/test_foc_motion_reference.c)

add_executable(test-foc-baseline tests/test_foc_baseline.c ../g3/np/src/foc_device.c)
target_link_libraries(test-foc-baseline PRIVATE falconfoc-core)
target_include_directories(
  test-foc-baseline
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../g3/np/include"
          "${CMAKE_CURRENT_SOURCE_DIR}/../applications/algo"
          "${CMAKE_CURRENT_SOURCE_DIR}/../tests/host/support"
)
add_test(NAME test-foc-baseline COMMAND test-foc-baseline --golden)

add_executable(test-legacy-sincos-baseline tests/test_legacy_sincos_baseline.c)
target_link_libraries(test-legacy-sincos-baseline PRIVATE falconfoc-core)
target_include_directories(
  test-legacy-sincos-baseline
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../applications/algo"
          "${CMAKE_CURRENT_SOURCE_DIR}/../tests/host/support"
)
add_test(NAME test-legacy-sincos-baseline COMMAND test-legacy-sincos-baseline)

add_executable(test-foc-sector-reference tests/test_foc_sector_reference.c)
target_link_libraries(test-foc-sector-reference PRIVATE falconfoc-core)
add_test(NAME test-foc-sector-reference COMMAND test-foc-sector-reference)

add_executable(test-foc-feedforward tests/test_foc_feedforward.c)
target_link_libraries(test-foc-feedforward PRIVATE falconfoc-core)
add_test(NAME test-foc-feedforward COMMAND test-foc-feedforward)

add_executable(test-foc-trajectory tests/test_foc_trajectory.c)
target_link_libraries(test-foc-trajectory PRIVATE falconfoc-core)
add_test(NAME test-foc-trajectory COMMAND test-foc-trajectory)
