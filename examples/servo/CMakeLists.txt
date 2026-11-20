# Themis FalconFOC Servo example. Role: show a standalone Host application
# linked against the public Control Plane, Core and Virtual Motor targets.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-servo-example
  VERSION 0.1.0
  LANGUAGES C
)

set(FALCONFOC_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    CACHE PATH "FalconFOC checkout containing Core, Simulation and Control Plane"
)
if(NOT EXISTS "${FALCONFOC_SOURCE_DIR}/core/CMakeLists.txt"
   OR NOT EXISTS "${FALCONFOC_SOURCE_DIR}/simulation/CMakeLists.txt"
)
  message(FATAL_ERROR "Set FALCONFOC_SOURCE_DIR to the FalconFOC checkout.")
endif()

if(NOT TARGET falconfoc-core)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/core" falconfoc-core EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET falconfoc-virtual-motor)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/simulation" falconfoc-simulation EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET falconfoc-control-plane)
  if(NOT EXISTS "${FALCONFOC_SOURCE_DIR}/control_plane/CMakeLists.txt")
    message(FATAL_ERROR "Servo requires the FalconFOC Control Plane source target.")
  endif()
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/control_plane" falconfoc-control-plane EXCLUDE_FROM_ALL)
endif()

add_executable(falconfoc-servo-demo main.c)
set_target_properties(
  falconfoc-servo-demo
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS NO
)
target_link_libraries(
  falconfoc-servo-demo PRIVATE falconfoc-virtual-motor falconfoc-control-plane falconfoc-core
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-servo-demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME test-falconfoc-servo-demo COMMAND falconfoc-servo-demo)
add_test(NAME test-falconfoc-servo-faults COMMAND falconfoc-servo-demo --faults)
add_test(NAME test-falconfoc-servo-inspect COMMAND falconfoc-servo-demo --inspect --iq-limit 1.8)
add_test(NAME test-falconfoc-servo-configured COMMAND falconfoc-servo-demo --iq-limit 1.8)
add_test(NAME test-falconfoc-servo-recovery COMMAND falconfoc-servo-demo --recovery)
add_test(NAME test-falconfoc-servo-help COMMAND falconfoc-servo-demo --help)
add_test(NAME test-falconfoc-servo-invalid-config COMMAND falconfoc-servo-demo --inspect --iq-limit
                                                          3
)
set_tests_properties(test-falconfoc-servo-invalid-config PROPERTIES WILL_FAIL TRUE)
