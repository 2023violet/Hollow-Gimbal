# Themis FalconFOC parameter project. Role: build the C11 control-plane facade.
cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-control-plane
  VERSION 0.1.0
  LANGUAGES C
)

if(NOT TARGET falconfoc-pipeline)
  add_subdirectory(../runtime falconfoc-runtime EXCLUDE_FROM_ALL)
endif()

add_library(falconfoc-control-plane STATIC src/foc_parameter.c)
target_link_libraries(falconfoc-control-plane PUBLIC falconfoc-pipeline)
target_include_directories(falconfoc-control-plane PUBLIC include)
target_compile_features(falconfoc-control-plane PUBLIC c_std_11)
set_target_properties(
  falconfoc-control-plane
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS OFF
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-control-plane PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(config_crc_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_library(
  falconfoc-config-store STATIC src/foc_config_codec.c src/foc_config_store.c
                                "${config_crc_dir}/crc.c"
)
target_link_libraries(falconfoc-config-store PUBLIC falconfoc-control-plane)
target_include_directories(falconfoc-config-store PRIVATE "${config_crc_dir}")

add_library(falconfoc-config-store-host STATIC EXCLUDE_FROM_ALL src/foc_config_store_host.c)
target_link_libraries(falconfoc-config-store-host PUBLIC falconfoc-config-store)

add_library(falconfoc-config-diagnostics STATIC EXCLUDE_FROM_ALL src/foc_config_diagnostics.c)
target_link_libraries(falconfoc-config-diagnostics PUBLIC falconfoc-config-store)

if(NOT TARGET falconfoc-calibration-workflow)
  add_subdirectory(../calibration falconfoc-calibration EXCLUDE_FROM_ALL)
endif()
add_library(
  falconfoc-calibration-application STATIC EXCLUDE_FROM_ALL src/foc_calibration_application.c
)
target_link_libraries(
  falconfoc-calibration-application PUBLIC falconfoc-calibration-workflow
                                           falconfoc-config-diagnostics
)

foreach(target IN ITEMS falconfoc-config-store falconfoc-config-store-host
                        falconfoc-config-diagnostics falconfoc-calibration-application
)
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

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_executable(test-foc-parameter tests/test_foc_parameter.c)
  target_link_libraries(test-foc-parameter PRIVATE falconfoc-control-plane)
  set_target_properties(
    test-foc-parameter
    PROPERTIES C_STANDARD 11
               C_STANDARD_REQUIRED YES
               C_EXTENSIONS OFF
  )
  add_test(NAME test-foc-parameter COMMAND test-foc-parameter)

  add_executable(test-foc-config-codec tests/test_foc_config_codec.c)
  target_link_libraries(test-foc-config-codec PRIVATE falconfoc-config-store)
  target_include_directories(test-foc-config-codec PRIVATE "${config_crc_dir}")
  add_test(NAME test-foc-config-codec COMMAND test-foc-config-codec)

  add_executable(test-foc-config-store tests/test_foc_config_store.c)
  target_link_libraries(test-foc-config-store PRIVATE falconfoc-config-store-host)
  target_include_directories(test-foc-config-store PRIVATE "${config_crc_dir}")
  add_test(NAME test-foc-config-store COMMAND test-foc-config-store)

  add_executable(test-foc-config-diagnostics tests/test_foc_config_diagnostics.c)
  target_link_libraries(
    test-foc-config-diagnostics PRIVATE falconfoc-config-diagnostics falconfoc-config-store-host
  )
  target_include_directories(test-foc-config-diagnostics PRIVATE "${config_crc_dir}")
  add_test(NAME test-foc-config-diagnostics COMMAND test-foc-config-diagnostics)

  add_executable(test-foc-calibration-application tests/test_foc_calibration_application.c)
  target_link_libraries(test-foc-calibration-application PRIVATE falconfoc-calibration-application)
  add_test(NAME test-foc-calibration-application COMMAND test-foc-calibration-application)
endif()
