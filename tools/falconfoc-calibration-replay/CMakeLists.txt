# Themis Host replay runner. Role: exercise typed trace validation and the
# existing calibration workflow from a standalone, public C11 consumer.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-calibration-replay
  VERSION 0.1.0
  LANGUAGES C
)

set(FALCONFOC_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    CACHE PATH "FalconFOC source tree containing calibration/"
)
if(NOT TARGET falconfoc-measurement-trace)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/calibration" falconfoc-calibration EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET falconfoc-control-plane)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/control_plane" falconfoc-control-plane EXCLUDE_FROM_ALL)
endif()

add_executable(falconfoc-calibration-replay-runner replay_runner.c)
target_link_libraries(
  falconfoc-calibration-replay-runner PRIVATE falconfoc-measurement-trace falconfoc-control-plane
)
set_target_properties(
  falconfoc-calibration-replay-runner
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS OFF
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-calibration-replay-runner PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED
  )
  add_test(NAME test-falconfoc-calibration-replay
           COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py"
                   --runner "$<TARGET_FILE:falconfoc-calibration-replay-runner>"
  )
endif()
