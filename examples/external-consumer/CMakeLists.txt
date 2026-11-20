# Use the installed Core and, optionally, the standard Runtime source target.
cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-external-consumer
  VERSION 0.1.0
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(FalconFOCCore CONFIG REQUIRED)
enable_testing()

add_executable(core-consumer core_consumer.c)
target_link_libraries(core-consumer PRIVATE FalconFOC::Core)
add_test(NAME external-core-consumer COMMAND core-consumer)

set(FALCONFOC_RUNTIME_SOURCE_DIR
    ""
    CACHE PATH "Optional source tree with Core, Runtime, G3 and Sensors"
)
if(FALCONFOC_RUNTIME_SOURCE_DIR)
  add_subdirectory("${FALCONFOC_RUNTIME_SOURCE_DIR}/runtime" runtime EXCLUDE_FROM_ALL)
  add_subdirectory("${FALCONFOC_RUNTIME_SOURCE_DIR}/sensors" sensors EXCLUDE_FROM_ALL)
  add_executable(runtime-consumer runtime_consumer.c)
  target_link_libraries(runtime-consumer PRIVATE falconfoc-pipeline falconfoc-encoder)
  add_test(NAME external-runtime-consumer COMMAND runtime-consumer)
endif()
