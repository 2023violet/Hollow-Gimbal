cmake_minimum_required(VERSION 3.25)
project(external-falconfoc-servo LANGUAGES C)
add_subdirectory("${FALCONFOC_SOURCE_DIR}" sdk EXCLUDE_FROM_ALL)
add_executable(external-servo-consumer main.c)
target_link_libraries(external-servo-consumer PRIVATE falconfoc-virtual-motor falconfoc-core)
set_target_properties(
  external-servo-consumer
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS OFF
)
enable_testing()
add_test(NAME external-servo-consumer COMMAND external-servo-consumer)
