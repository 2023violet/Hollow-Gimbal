cmake_minimum_required(VERSION 3.25)
project(external-falconfoc-core LANGUAGES C)

set(FALCONFOC_SDK_CORE_ONLY ON)
add_subdirectory("${FALCONFOC_SOURCE_DIR}" sdk EXCLUDE_FROM_ALL)
if(TARGET falconfoc-pipeline OR TARGET falconfoc-virtual-motor)
  message(FATAL_ERROR "Core-only consumer unexpectedly has Host Runtime targets.")
endif()
add_executable(external-falconfoc-core main.c)
target_link_libraries(external-falconfoc-core PRIVATE falconfoc-core)
set_target_properties(
  external-falconfoc-core
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS OFF
)
enable_testing()
add_test(NAME external-falconfoc-core COMMAND external-falconfoc-core)
