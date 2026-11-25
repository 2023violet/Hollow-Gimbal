# Themis FalconFOC G3-NP standalone host project. Role: build the provisional
# Core/Device contract without the root firmware graph. Responsibilities:
# compose the G2 Core, Device mapping, closed-gate simulator, and no-power host
# tests with explicit public include boundaries.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-g3-np
  VERSION 0.1.0
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_library(falconfoc-core STATIC ../../core/src/foc_core.c ../../core/src/foc_trig.c)
target_include_directories(falconfoc-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include")

add_library(falconfoc-device STATIC src/foc_device.c)
target_include_directories(
  falconfoc-device PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                          "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include"
)
target_link_libraries(falconfoc-device PUBLIC falconfoc-core)

add_library(falconfoc-device-sim STATIC src/foc_device_sim.c)
target_include_directories(
  falconfoc-device-sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                              "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include"
)
target_link_libraries(falconfoc-device-sim PUBLIC falconfoc-device falconfoc-core)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(falconfoc-device PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(falconfoc-device-sim PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT WIN32 AND NOT MSVC)
  target_link_libraries(falconfoc-core PUBLIC m)
endif()

function(themis_add_falconfoc_np_test TEST_NAME TEST_SOURCE)
  add_executable(${TEST_NAME} ${TEST_SOURCE})
  target_include_directories(
    ${TEST_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                         "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include"
  )
  target_link_libraries(${TEST_NAME} PRIVATE falconfoc-device-sim falconfoc-device falconfoc-core)
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

themis_add_falconfoc_np_test(test-foc-device-contract tests/test_foc_device_contract.c)
themis_add_falconfoc_np_test(test-foc-device-sim tests/test_foc_device_sim.c)
