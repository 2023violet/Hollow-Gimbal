# Themis FalconFOC Host inspector. Role: build the read-only image CLI.
cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-config-inspector
  VERSION 0.1.0
  LANGUAGES C
)

if(NOT TARGET falconfoc-virtual-motor)
  add_subdirectory(../../simulation falconfoc-simulation EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET falconfoc-config-diagnostics)
  add_subdirectory(../../control_plane falconfoc-control-plane EXCLUDE_FROM_ALL)
endif()

add_library(falconfoc-config-fixture-profile STATIC fixture_profile.c)
target_link_libraries(
  falconfoc-config-fixture-profile PUBLIC falconfoc-config-store falconfoc-virtual-motor
)
target_include_directories(falconfoc-config-fixture-profile PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(falconfoc-config-inspector main.c diagnostics_output.c)
target_link_libraries(
  falconfoc-config-inspector PRIVATE falconfoc-config-diagnostics falconfoc-config-fixture-profile
)

set(inspector_targets falconfoc-config-inspector falconfoc-config-fixture-profile)
if(PROJECT_IS_TOP_LEVEL)
  include(CTest)
  if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(falconfoc-config-fixtures tests/make_fixtures.c)
    target_link_libraries(
      falconfoc-config-fixtures PRIVATE falconfoc-config-store-host
                                        falconfoc-config-fixture-profile
    )
    list(APPEND inspector_targets falconfoc-config-fixtures)
    add_test(
      NAME test-falconfoc-config-inspector
      COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_inspector.py"
        "$<TARGET_FILE:falconfoc-config-inspector>" "$<TARGET_FILE:falconfoc-config-fixtures>"
    )
  endif()
endif()

foreach(target IN LISTS inspector_targets)
  set_target_properties(
    ${target}
    PROPERTIES C_STANDARD 11
               C_STANDARD_REQUIRED YES
               C_EXTENSIONS OFF
  )
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
