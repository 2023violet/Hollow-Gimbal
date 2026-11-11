# Themis FalconFOC binary protocol. Role: assemble the portable wire codec.
# Responsibilities: expose the C11 codec and run shared V1 wire vector tests.

cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(
    falconfoc-wire
    VERSION 0.1.0
    LANGUAGES C
  )
  enable_testing()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(falconfoc-wire STATIC src/foc_protocol.c ../components/utils/cobs/cobs.c)
target_include_directories(
  falconfoc-wire
  PUBLIC include
  PRIVATE ../components/utils/cobs
)
target_compile_features(falconfoc-wire PUBLIC c_std_11)
set_target_properties(falconfoc-wire PROPERTIES C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-wire PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(CMAKE_CROSSCOMPILING)
  target_compile_options(falconfoc-wire PRIVATE -Os -fstack-usage)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(test-foc-protocol tests/test_foc_protocol.c)
  target_include_directories(test-foc-protocol PRIVATE ../components/utils/cobs)
  target_link_libraries(test-foc-protocol PRIVATE falconfoc-wire)
  add_test(NAME test-foc-protocol COMMAND test-foc-protocol
                                          "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors.tsv"
  )
endif()
