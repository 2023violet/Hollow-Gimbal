# Themis FalconFOC motion-protection example. Role: exercise the production Runtime on a Host plant.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-motion-protection-example
  VERSION 0.1.0
  LANGUAGES C
)

set(FALCONFOC_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    CACHE PATH "FalconFOC checkout containing simulation/"
)
if(NOT EXISTS "${FALCONFOC_SOURCE_DIR}/simulation/CMakeLists.txt")
  message(FATAL_ERROR "Set FALCONFOC_SOURCE_DIR to a checkout containing simulation/.")
endif()

if(NOT TARGET falconfoc-virtual-motor)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/simulation" falconfoc-simulation EXCLUDE_FROM_ALL)
endif()

add_executable(falconfoc-motion-protection-demo main.c)
set_target_properties(
  falconfoc-motion-protection-demo
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS NO
)
target_link_libraries(falconfoc-motion-protection-demo PRIVATE falconfoc-virtual-motor)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-motion-protection-demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME test-falconfoc-motion-protection-example COMMAND falconfoc-motion-protection-demo)
