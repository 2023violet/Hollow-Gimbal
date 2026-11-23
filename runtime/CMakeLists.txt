# Themis FalconFOC Runtime host project. Role: compose the current pipeline
# and three execution domains independently of firmware. Responsibilities:
# build the Core, Device, Safety, Mock, and Runtime and register their tests.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-pipeline
  VERSION 0.1.0
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_library(
  falconfoc-pipeline STATIC
  src/foc_pipeline.c
  src/foc_runtime.c
  ../core/src/foc_core.c
  ../core/src/foc_current.c
  ../core/src/foc_impedance.c
  ../core/src/foc_motion.c
  ../core/src/foc_motion_limits.c
  ../core/src/foc_following_error.c
  ../core/src/foc_trajectory.c
  ../g3/np/src/foc_device.c
  ../g3/np/src/foc_device_sim.c
  ../g3/safe/src/foc_safe.c
  ../g3/safe/src/foc_protect.c
)
target_include_directories(
  falconfoc-pipeline PUBLIC include ../core/include ../g3/np/include ../g3/safe/include
)
target_compile_definitions(falconfoc-pipeline PUBLIC FOC_RUNTIME_HOST_COGGING=1)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT WIN32 AND NOT MSVC)
  target_link_libraries(falconfoc-pipeline PUBLIC m)
endif()

add_executable(test-foc-pipeline tests/test_foc_pipeline.c)
target_link_libraries(test-foc-pipeline PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-pipeline COMMAND test-foc-pipeline)

add_executable(test-foc-runtime tests/test_foc_runtime.c)
target_link_libraries(test-foc-runtime PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-runtime COMMAND test-foc-runtime)

add_executable(test-foc-runtime-mailbox tests/test_foc_runtime_mailbox.c)
target_link_libraries(test-foc-runtime-mailbox PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-runtime-mailbox COMMAND test-foc-runtime-mailbox)

add_executable(test-foc-motion-model tests/test_foc_motion_model.c)
target_link_libraries(test-foc-motion-model PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-motion-model COMMAND test-foc-motion-model)

add_executable(test-foc-motion-runtime tests/test_foc_motion_runtime.c)
target_link_libraries(test-foc-motion-runtime PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-motion-runtime COMMAND test-foc-motion-runtime)
add_executable(test-foc-runtime-impedance tests/test_foc_runtime_impedance.c)
target_link_libraries(test-foc-runtime-impedance PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-runtime-impedance COMMAND test-foc-runtime-impedance)

add_executable(test-foc-runtime-protection tests/test_foc_runtime_protection.c)
target_link_libraries(test-foc-runtime-protection PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-runtime-protection COMMAND test-foc-runtime-protection)

add_executable(test-foc-remote tests/test_foc_remote.c)
target_link_libraries(test-foc-remote PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-remote COMMAND test-foc-remote)
add_executable(test-foc-long tests/test_foc_long.c)
target_link_libraries(test-foc-long PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-long COMMAND test-foc-long)

add_executable(test-foc-control01b tests/test_foc_control01b.c)
target_link_libraries(test-foc-control01b PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-control01b COMMAND test-foc-control01b)

add_executable(test-foc-system tests/test_foc_system.c)
target_link_libraries(test-foc-system PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-system COMMAND test-foc-system)

add_executable(test-foc-robustness tests/test_foc_robustness.c)
target_link_libraries(test-foc-robustness PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-robustness COMMAND test-foc-robustness)

add_executable(test-foc-ff-admission tests/test_foc_ff_admission.c)
target_link_libraries(test-foc-ff-admission PRIVATE falconfoc-pipeline)
add_test(NAME test-foc-ff-admission COMMAND test-foc-ff-admission)
