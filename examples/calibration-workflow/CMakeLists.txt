# Themis FalconFOC calibration workflow example. Role: build a standalone C11
# Host consumer against the public workflow and application targets.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-calibration-workflow-example
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
  message(FATAL_ERROR "Set FALCONFOC_SOURCE_DIR to a FalconFOC source tree.")
endif()

if(NOT TARGET falconfoc-virtual-motor)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/simulation" falconfoc-simulation EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET falconfoc-calibration-application)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/control_plane" falconfoc-control-plane EXCLUDE_FROM_ALL)
endif()

add_executable(falconfoc-calibration-workflow-demo main.c)
target_link_libraries(
  falconfoc-calibration-workflow-demo PRIVATE falconfoc-calibration-application
                                              falconfoc-virtual-motor
)
set_target_properties(
  falconfoc-calibration-workflow-demo
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS OFF
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-calibration-workflow-demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME test-falconfoc-calibration-workflow-demo COMMAND falconfoc-calibration-workflow-demo)
