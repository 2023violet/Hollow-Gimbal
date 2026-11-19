# Themis FalconFOC Servo example. Role: show a standalone Host application
# linked against the public Core and Virtual Motor targets.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-servo-example
  VERSION 0.1.0
  LANGUAGES C
)

set(FALCONFOC_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    CACHE PATH "FalconFOC checkout containing core/ and simulation/"
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

add_executable(falconfoc-servo-demo main.c)
set_target_properties(
  falconfoc-servo-demo
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS NO
)
target_link_libraries(falconfoc-servo-demo PRIVATE falconfoc-virtual-motor falconfoc-core)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-servo-demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME test-falconfoc-servo-demo COMMAND falconfoc-servo-demo)
add_test(NAME test-falconfoc-servo-faults COMMAND falconfoc-servo-demo --faults)
