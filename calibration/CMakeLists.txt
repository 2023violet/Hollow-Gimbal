# Themis FalconFOC portable calibration project. Role: build bounded Float32
# current-zero and rotor-reference candidate estimators without Runtime, Board,
# Device, Safety, Flash, MCU, or hardware dependencies.

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

add_library(falconfoc-calibration STATIC src/foc_calibration.c)
target_include_directories(falconfoc-calibration PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-calibration PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT WIN32 AND NOT MSVC)
  target_link_libraries(falconfoc-calibration PUBLIC m)
endif()

add_executable(test-foc-calibration tests/test_foc_calibration.c)
target_link_libraries(test-foc-calibration PRIVATE falconfoc-calibration)
add_test(NAME test-foc-calibration COMMAND test-foc-calibration)
