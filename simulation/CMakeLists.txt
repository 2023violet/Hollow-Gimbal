# Themis FalconFOC Host virtual motor project. Role: build the deterministic
# Host PMSM Plant, Runtime-backed Current loop, and its contract tests.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-virtual-motor
  VERSION 0.1.0
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../runtime" runtime)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../protocol" protocol)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../calibration" calibration)

add_library(
  falconfoc-virtual-motor STATIC src/foc_virtual_motor.c src/foc_virtual_current.c
                                 src/foc_virtual_cogging.c
)
target_include_directories(
  falconfoc-virtual-motor
  PUBLIC include "${CMAKE_CURRENT_SOURCE_DIR}/../core/include"
         "${CMAKE_CURRENT_SOURCE_DIR}/../g3/np/include"
         "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include"
)
target_link_libraries(falconfoc-virtual-motor PUBLIC falconfoc-pipeline falconfoc-encoder)

target_compile_definitions(test-foc-motion-model PRIVATE FOC_VIRTUAL_SHARED_PLANT=1)
target_include_directories(test-foc-motion-model PRIVATE include)
target_link_libraries(test-foc-motion-model PRIVATE falconfoc-virtual-motor)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-virtual-motor PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT WIN32 AND NOT MSVC)
  target_link_libraries(falconfoc-virtual-motor PUBLIC m)
endif()

add_executable(test-foc-virtual-motor tests/test_foc_virtual_motor.c)
target_link_libraries(test-foc-virtual-motor PRIVATE falconfoc-virtual-motor)
add_test(NAME test-foc-virtual-motor COMMAND test-foc-virtual-motor)

add_executable(test-foc-virtual-sensor tests/test_foc_virtual_sensor.c)
target_link_libraries(test-foc-virtual-sensor PRIVATE falconfoc-virtual-motor)
add_test(NAME test-foc-virtual-sensor COMMAND test-foc-virtual-sensor)

add_executable(test-foc-virtual-current tests/test_foc_virtual_current.c)
target_link_libraries(test-foc-virtual-current PRIVATE falconfoc-virtual-motor)
add_test(NAME test-foc-virtual-current COMMAND test-foc-virtual-current)

add_executable(test-foc-periodic-chain tests/test_foc_periodic_chain.c)
target_link_libraries(test-foc-periodic-chain PRIVATE falconfoc-virtual-motor falconfoc-calibration)
add_test(NAME test-foc-periodic-chain COMMAND test-foc-periodic-chain)

add_executable(falconfoc-virtual-current src/falconfoc_virtual_current.c)
target_link_libraries(falconfoc-virtual-current PRIVATE falconfoc-virtual-motor)

add_executable(falconfoc-virtual-motion src/falconfoc_virtual_motion.c)
target_link_libraries(falconfoc-virtual-motion PRIVATE falconfoc-virtual-motor)

add_executable(
  falconfoc-virtual-device
  src/falconfoc_virtual_device.c
  "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/at32f415/src/foc_serial.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/at32f415/src/foc_uart_buffer.c"
)
target_include_directories(
  falconfoc-virtual-device
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/at32f415/include"
          "${CMAKE_CURRENT_SOURCE_DIR}/../calibration/include"
          "${CMAKE_CURRENT_SOURCE_DIR}/../sensors/include"
)
target_link_libraries(falconfoc-virtual-device PRIVATE falconfoc-virtual-motor falconfoc-wire)

add_executable(test-foc-motor-id-virtual tests/test_foc_motor_id_virtual.c)
target_link_libraries(
  test-foc-motor-id-virtual PRIVATE falconfoc-virtual-motor falconfoc-calibration
)
add_test(NAME test-foc-motor-id-virtual COMMAND test-foc-motor-id-virtual)

add_executable(test-foc-sensor-est tests/test_foc_sensor_est.c tests/foc_sensor_pll_experiment.c)
target_link_libraries(test-foc-sensor-est PRIVATE falconfoc-virtual-motor)
add_test(NAME test-foc-sensor-est COMMAND test-foc-sensor-est)

add_executable(test-foc-impedance-virtual tests/test_foc_impedance_virtual.c)
target_link_libraries(test-foc-impedance-virtual PRIVATE falconfoc-virtual-motor)
add_test(NAME test-foc-impedance-virtual COMMAND test-foc-impedance-virtual)

add_executable(
  test-foc-runtime-impedance-virtual tests/test_foc_runtime_impedance_virtual.c
                                     ../core/src/foc_impedance_feasibility.c
)
target_link_libraries(test-foc-runtime-impedance-virtual PRIVATE falconfoc-virtual-motor)
add_test(NAME test-foc-runtime-impedance-virtual COMMAND test-foc-runtime-impedance-virtual)
add_test(NAME test-foc-impedance-static-hold COMMAND test-foc-runtime-impedance-virtual
                                                     --static-hold
)

add_executable(
  test-foc-impedance-robustness
  tests/test_foc_impedance_robustness.c ../core/src/foc_impedance_tuning.c
  ../core/src/foc_impedance_feedforward.c
)
target_link_libraries(test-foc-impedance-robustness PRIVATE falconfoc-virtual-motor)
add_test(NAME test-foc-impedance-robustness COMMAND test-foc-impedance-robustness)
add_test(NAME test-foc-impedance-runtime-damping COMMAND test-foc-impedance-robustness
                                                         --runtime-damping
)
add_test(NAME test-foc-impedance-damping-noise COMMAND test-foc-impedance-robustness
                                                       --damping-noise-holdout
)

add_test(NAME test-foc-impedance-motion-feedforward COMMAND test-foc-impedance-robustness
                                                            --motion-feedforward-holdout
)
add_test(NAME test-foc-impedance-motion-reference-probe COMMAND test-foc-impedance-robustness
                                                                --motion-feedforward-legacy
)

add_test(NAME test-foc-impedance-joint-characterization COMMAND test-foc-impedance-robustness
                                                                --joint-control-characterization
)
add_test(NAME test-foc-impedance-joint-holdout COMMAND test-foc-impedance-robustness
                                                       --joint-control-holdout
)

add_executable(test-foc-mechanical-id-virtual tests/test_foc_mechanical_id_virtual.c)
target_link_libraries(
  test-foc-mechanical-id-virtual PRIVATE falconfoc-virtual-motor falconfoc-calibration
)
add_test(NAME test-foc-mechanical-id-virtual COMMAND test-foc-mechanical-id-virtual)

add_executable(test-foc-virtual-calibrated-driver tests/test_foc_virtual_calibrated_driver.c)
target_link_libraries(test-foc-virtual-calibrated-driver PRIVATE falconfoc-virtual-motor)
add_test(NAME test-foc-virtual-calibrated-driver COMMAND test-foc-virtual-calibrated-driver)

add_executable(test-foc-virtual-cogging tests/test_foc_virtual_cogging.c)
target_link_libraries(test-foc-virtual-cogging PRIVATE falconfoc-virtual-motor)
add_test(NAME test-foc-virtual-cogging COMMAND test-foc-virtual-cogging)

if(PROJECT_IS_TOP_LEVEL)
  add_subdirectory(../control_plane control_plane EXCLUDE_FROM_ALL)
  add_library(
    falconfoc-virtual-calibration STATIC src/foc_virtual_calibration.c
                                         src/foc_virtual_calibration_electrical.c
  )
  target_link_libraries(
    falconfoc-virtual-calibration PUBLIC falconfoc-virtual-motor falconfoc-calibration-application
  )
  add_executable(
    falconfoc-virtual-calibrate src/falconfoc_virtual_calibrate.c
                                src/foc_virtual_calibration_package.c
  )
  target_link_libraries(falconfoc-virtual-calibrate PRIVATE falconfoc-virtual-calibration)
  target_include_directories(falconfoc-virtual-calibrate PRIVATE ../components/utils/crc)
  add_executable(
    test-foc-virtual-calibration-electrical tests/test_foc_virtual_calibration_electrical.c
  )
  target_link_libraries(
    test-foc-virtual-calibration-electrical PRIVATE falconfoc-virtual-calibration
  )
  add_test(NAME test-foc-virtual-calibration-electrical
           COMMAND test-foc-virtual-calibration-electrical
  )
  add_executable(test-foc-virtual-calibration-session tests/test_foc_virtual_calibration_session.c)
  target_link_libraries(test-foc-virtual-calibration-session PRIVATE falconfoc-virtual-calibration)
  add_test(NAME test-foc-virtual-calibration-session COMMAND test-foc-virtual-calibration-session)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME test-foc-virtual-calibration-cli
      COMMAND
        "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_virtual_calibration_cli.py"
        $<TARGET_FILE:falconfoc-virtual-calibrate>
    )
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(falconfoc-virtual-calibration PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(falconfoc-virtual-calibrate PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
