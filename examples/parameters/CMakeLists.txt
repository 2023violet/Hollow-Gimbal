# Themis FalconFOC parameter example. Role: build a standalone Host consumer.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-parameter-example
  VERSION 0.1.0
  LANGUAGES C
)

set(FALCONFOC_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    CACHE PATH "FalconFOC checkout containing control_plane/ and simulation/"
)
if(NOT EXISTS "${FALCONFOC_SOURCE_DIR}/control_plane/CMakeLists.txt"
   OR NOT EXISTS "${FALCONFOC_SOURCE_DIR}/simulation/CMakeLists.txt"
)
  message(
    FATAL_ERROR
      "Set FALCONFOC_SOURCE_DIR to a checkout containing the parameter API and simulation."
  )
endif()

if(NOT TARGET falconfoc-virtual-motor)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/simulation" falconfoc-simulation EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET falconfoc-control-plane)
  add_subdirectory("${FALCONFOC_SOURCE_DIR}/control_plane" falconfoc-control-plane EXCLUDE_FROM_ALL)
endif()

add_executable(falconfoc-parameter-demo main.c)
set_target_properties(
  falconfoc-parameter-demo
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS NO
)
target_link_libraries(
  falconfoc-parameter-demo PRIVATE falconfoc-control-plane falconfoc-virtual-motor
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-parameter-demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(falconfoc-config-store-demo config_store.c)
set_target_properties(
  falconfoc-config-store-demo
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS NO
)
target_link_libraries(
  falconfoc-config-store-demo PRIVATE falconfoc-config-store-host falconfoc-virtual-motor
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-config-store-demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME test-falconfoc-parameters-example COMMAND falconfoc-parameter-demo)
add_test(NAME test-falconfoc-config-store-demo COMMAND falconfoc-config-store-demo)
