# Themis FalconFOC portable calibration project. Role: build bounded
# current-zero, rotor-reference, standstill R/L, mechanical J/B, and offline encoder LUT
# candidate estimators without Runtime, Board, Device, Safety, or MCU dependencies.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-calibration
  VERSION 0.1.0
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

if(NOT TARGET falconfoc-encoder)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../sensors" encoder)
endif()

add_library(
  falconfoc-calibration STATIC
  src/foc_calibration.c
  src/foc_motor_id.c
  src/foc_flux_id.c
  src/foc_encoder_calibration.c
  src/foc_mechanical_id.c
  src/foc_calibration_math.c
  # One foc_sincosf() definition; Calibration links no Core library.
  ../core/src/foc_trig.c
)
target_link_libraries(falconfoc-calibration PUBLIC falconfoc-encoder)
target_include_directories(falconfoc-calibration PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Header-only foc_trig.h; Calibration links no Core library.
target_include_directories(falconfoc-calibration PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../core/include")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-calibration PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT WIN32 AND NOT MSVC)
  target_link_libraries(falconfoc-calibration PUBLIC m)
endif()

add_executable(test-foc-calibration tests/test_foc_calibration.c)
target_link_libraries(test-foc-calibration PRIVATE falconfoc-calibration)
add_test(NAME test-foc-calibration COMMAND test-foc-calibration)

add_executable(test-foc-motor-id tests/test_foc_motor_id.c)
target_link_libraries(test-foc-motor-id PRIVATE falconfoc-calibration)
add_test(NAME test-foc-motor-id COMMAND test-foc-motor-id)

add_executable(test-foc-flux-id tests/test_foc_flux_id.c)
target_link_libraries(test-foc-flux-id PRIVATE falconfoc-calibration)
add_test(NAME test-foc-flux-id COMMAND test-foc-flux-id)

add_executable(test-foc-encoder-calibration tests/test_foc_encoder_calibration.c)
target_link_libraries(test-foc-encoder-calibration PRIVATE falconfoc-calibration)
add_test(NAME test-foc-encoder-calibration COMMAND test-foc-encoder-calibration)

add_executable(test-foc-mechanical-id tests/test_foc_mechanical_id.c)
target_link_libraries(test-foc-mechanical-id PRIVATE falconfoc-calibration)
add_test(NAME test-foc-mechanical-id COMMAND test-foc-mechanical-id)

add_library(
  falconfoc-calibration-workflow STATIC EXCLUDE_FROM_ALL
  src/foc_calibration_workflow.c src/foc_bootstrap.c ../core/src/foc_impedance_tuning.c
)
target_include_directories(falconfoc-calibration-workflow PUBLIC ../core/include)
target_link_libraries(falconfoc-calibration-workflow PUBLIC falconfoc-calibration)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-calibration-workflow PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(falconfoc-measurement-trace STATIC EXCLUDE_FROM_ALL src/foc_measurement_trace.c)
target_link_libraries(falconfoc-measurement-trace PUBLIC falconfoc-calibration-workflow)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-measurement-trace PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PROJECT_IS_TOP_LEVEL)
  add_executable(test-foc-bootstrap tests/test_foc_bootstrap.c)
  target_link_libraries(test-foc-bootstrap PRIVATE falconfoc-calibration-workflow)
  add_test(NAME test-foc-bootstrap COMMAND test-foc-bootstrap)

  add_executable(test-foc-calibration-workflow tests/test_foc_calibration_workflow.c)
  target_link_libraries(test-foc-calibration-workflow PRIVATE falconfoc-calibration-workflow)
  add_test(NAME test-foc-calibration-workflow COMMAND test-foc-calibration-workflow)

  add_executable(test-foc-measurement-trace tests/test_foc_measurement_trace.c)
  target_link_libraries(test-foc-measurement-trace PRIVATE falconfoc-measurement-trace)
  add_test(NAME test-foc-measurement-trace COMMAND test-foc-measurement-trace)
endif()
