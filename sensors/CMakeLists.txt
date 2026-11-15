# Themis FalconFOC portable encoder project. Role: build the MCU-independent
# magnetic encoder algorithms and deterministic Host tests without SPI, RTOS,
# Runtime, vendor SDK, or hardware dependencies.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-sensors
  VERSION 0.1.0
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_library(falconfoc-encoder STATIC src/foc_encoder.c)
target_include_directories(falconfoc-encoder PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-encoder PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT WIN32 AND NOT MSVC)
  target_link_libraries(falconfoc-encoder PUBLIC m)
endif()

add_executable(test-foc-encoder tests/test_foc_encoder.c)
target_link_libraries(test-foc-encoder PRIVATE falconfoc-encoder)
add_test(NAME test-foc-encoder COMMAND test-foc-encoder)

add_executable(test-foc-sensor-reference tests/test_foc_sensor_reference.c)
target_link_libraries(test-foc-sensor-reference PRIVATE falconfoc-encoder)
add_test(NAME test-foc-sensor-reference COMMAND test-foc-sensor-reference)
