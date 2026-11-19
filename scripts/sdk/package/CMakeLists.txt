# FalconFOC source candidate entry. The original firmware build is separate.
cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-servo-sdk
  VERSION 0.1.0
  LANGUAGES C
)

option(FALCONFOC_SDK_CORE_ONLY "Build and expose only the portable Core" OFF)
option(FALCONFOC_SDK_BUILD_TESTS "Build Host Servo example and smoke tests" ${PROJECT_IS_TOP_LEVEL})

add_subdirectory(core)

if(NOT FALCONFOC_SDK_CORE_ONLY)
  add_subdirectory(simulation)
  if(FALCONFOC_SDK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(examples/servo)
    add_executable(test-sdk-impedance-output simulation/tests/test_foc_runtime_impedance_virtual.c)
    target_link_libraries(test-sdk-impedance-output PRIVATE falconfoc-virtual-motor)
    add_test(NAME test-sdk-impedance-output COMMAND test-sdk-impedance-output)

    add_executable(test-sdk-mechanical-id calibration/tests/test_foc_mechanical_id.c)
    target_link_libraries(test-sdk-mechanical-id PRIVATE falconfoc-calibration)
    add_test(NAME test-sdk-mechanical-id COMMAND test-sdk-mechanical-id)
  endif()
endif()
