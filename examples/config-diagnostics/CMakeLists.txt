# Themis FalconFOC configuration diagnostics example. Role: build an external
# Host consumer.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-config-diagnostics-example
  VERSION 0.1.0
  LANGUAGES C
)

set(FALCONFOC_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    CACHE PATH "FalconFOC source tree containing control_plane/ and simulation/"
)
if(NOT EXISTS "${FALCONFOC_SOURCE_DIR}/control_plane/CMakeLists.txt"
   OR NOT EXISTS "${FALCONFOC_SOURCE_DIR}/simulation/CMakeLists.txt"
)
  message(FATAL_ERROR "Set FALCONFOC_SOURCE_DIR to the FalconFOC source tree.")
endif()

if(NOT TARGET falconfoc-virtual-motor)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/simulation" falconfoc-simulation EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET falconfoc-control-plane)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/control_plane" falconfoc-control-plane EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET falconfoc-config-diagnostics)
  message(FATAL_ERROR "FALCONFOC_SOURCE_DIR must provide the falconfoc-config-diagnostics target.")
endif()

add_executable(falconfoc-config-diagnostics-demo main.c)
set_target_properties(
  falconfoc-config-diagnostics-demo
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS OFF
)
target_link_libraries(
  falconfoc-config-diagnostics-demo PRIVATE falconfoc-config-diagnostics
                                            falconfoc-config-store-host falconfoc-virtual-motor
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-config-diagnostics-demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME test-falconfoc-config-diagnostics-demo COMMAND falconfoc-config-diagnostics-demo)
