# Themis FalconFOC G3-SAFE standalone host project. Role: build the no-power
# safety contract independently from firmware. Responsibilities: compile the
# safety state machine, connect Device request types, and run deterministic
# fault-injection tests without hardware access.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-g3-safe
  VERSION 0.1.0
  LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_library(falconfoc-device STATIC ../../g3/np/src/foc_device.c)
target_include_directories(
  falconfoc-device PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../g3/np/include"
                          "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include")

add_library(falconfoc-safe STATIC src/foc_safe.c)
target_include_directories(
  falconfoc-safe
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../g3/np/include"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include")
target_link_libraries(falconfoc-safe PUBLIC falconfoc-device)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-device PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(falconfoc-safe PRIVATE -Wall -Wextra -Wpedantic)
endif()

function(themis_add_falconfoc_safe_test TEST_NAME TEST_SOURCE)
  add_executable(${TEST_NAME} ${TEST_SOURCE})
  target_include_directories(
    ${TEST_NAME}
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../g3/np/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include")
  target_link_libraries(${TEST_NAME} PRIVATE falconfoc-safe falconfoc-device)
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

themis_add_falconfoc_safe_test(test-foc-safe-contract
                               tests/test_foc_safe_contract.c)
