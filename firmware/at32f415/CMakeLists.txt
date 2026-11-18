# Themis FalconFOC AT32F415 reference build. Role: assemble the locked V2
# firmware or exercise its Board1 port on the host. Responsibilities: select
# vendor startup/SDK inputs, link the V2 control path, and emit build artifacts.

cmake_minimum_required(VERSION 3.25)

project(
  falconfoc-at32f415
  VERSION 0.1.0
  LANGUAGES C ASM
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(repo "${CMAKE_CURRENT_LIST_DIR}/../..")
set(vendor "${repo}/platform/at/at32f415/vendor")
set(cmsis "${vendor}/cmsis/cm4")
set(startup "${cmsis}/device_support/startup/gcc")
add_subdirectory("${repo}/protocol" protocol)

add_library(
  falconfoc-board1 STATIC
  src/foc_board.c
  src/foc_serial.c
  src/foc_uart_buffer.c
  "${repo}/core/src/foc_core.c"
  "${repo}/core/src/foc_current.c"
  "${repo}/core/src/foc_impedance.c"
  "${repo}/core/src/foc_motion.c"
  "${repo}/core/src/foc_trajectory.c"
  "${repo}/calibration/src/foc_calibration.c"
  "${repo}/sensors/src/foc_encoder.c"
  "${repo}/g3/np/src/foc_device.c"
  "${repo}/g3/np/src/foc_device_sim.c"
  "${repo}/g3/safe/src/foc_safe.c"
  "${repo}/runtime/src/foc_pipeline.c"
  "${repo}/runtime/src/foc_runtime.c"
)
target_link_libraries(falconfoc-board1 PUBLIC falconfoc-wire)
target_include_directories(
  falconfoc-board1
  PUBLIC include
         "${repo}/core/include"
         "${repo}/g3/np/include"
         "${repo}/g3/safe/include"
         "${repo}/runtime/include"
         "${repo}/calibration/include"
         "${repo}/sensors/include"
         "${repo}/platform/at/at32f415/generated/inc"
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(falconfoc-board1 PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(NOT WIN32 AND NOT MSVC)
  target_link_libraries(falconfoc-board1 PUBLIC m)
endif()

if(CMAKE_CROSSCOMPILING)
  if(NOT MCU_CPU STREQUAL "cortex-m4"
     OR NOT MCU_FLOAT STREQUAL "soft"
     OR NOT MCU_FPU STREQUAL "none"
  )
    message(FATAL_ERROR "Use MCU_CPU=cortex-m4, MCU_FLOAT=soft and MCU_FPU=none")
  endif()
  set(image falconfoc-v2-at32f415)
  set(linker "${startup}/linker/AT32F415xB_FLASH.ld")
  set(stack "${CMAKE_CURRENT_LIST_DIR}/stack-budget.ld")
  add_executable(
    ${image}
    src/main.c
    "${startup}/startup_at32f415.s"
    "${cmsis}/device_support/system_at32f415.c"
    "${vendor}/drivers/src/at32f415_adc.c"
    "${vendor}/drivers/src/at32f415_crm.c"
    "${vendor}/drivers/src/at32f415_flash.c"
    "${vendor}/drivers/src/at32f415_gpio.c"
    "${vendor}/drivers/src/at32f415_spi.c"
    "${vendor}/drivers/src/at32f415_tmr.c"
    "${vendor}/drivers/src/at32f415_usart.c"
  )
  set_target_properties(${image} PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${linker};${stack}")
  target_compile_definitions(${image} PRIVATE AT32F415KBU7_4 USE_STDPERIPH_DRIVER)
  target_include_directories(
    ${image} PRIVATE "${cmsis}/core_support" "${cmsis}/device_support" "${vendor}/drivers/inc"
  )
  target_link_libraries(${image} PRIVATE falconfoc-board1)
  target_compile_options(falconfoc-board1 PRIVATE -Os -fstack-usage)
  target_compile_options(${image} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Os;-fstack-usage;-Wextra>)
  target_link_options(
    ${image}
    PRIVATE
    "-T${linker}"
    "-T${stack}"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${image}.map"
    -Wl,--print-memory-usage
  )
  add_custom_command(
    TARGET ${image}
    POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:${image}>" "${image}.bin"
    COMMAND "${CMAKE_OBJCOPY}" -O ihex "$<TARGET_FILE:${image}>" "${image}.hex"
    BYPRODUCTS "${image}.bin" "${image}.hex"
    VERBATIM
  )
else()
  enable_testing()
  add_executable(test-foc-board1 tests/test_foc_board.c)
  target_link_libraries(test-foc-board1 PRIVATE falconfoc-board1)
  add_test(NAME test-foc-board1 COMMAND test-foc-board1)
  add_executable(test-foc-board-config tests/test_foc_board_config.c)
  target_link_libraries(test-foc-board-config PRIVATE falconfoc-board1)
  add_test(NAME test-foc-board-config COMMAND test-foc-board-config)
  add_executable(test-foc-serial tests/test_foc_serial.c)
  target_link_libraries(test-foc-serial PRIVATE falconfoc-board1)
  add_test(NAME test-foc-serial COMMAND test-foc-serial)
  add_executable(foc-serial-sim tests/foc_serial_sim.c)
  target_link_libraries(foc-serial-sim PRIVATE falconfoc-board1)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME test-foc-python COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s
                                        "${repo}/tools/falconfoc-cli" -p test_falconfoc_cli.py -v
  )
  set_tests_properties(
    test-foc-python PROPERTIES ENVIRONMENT "FOC_SERIAL_SIM=$<TARGET_FILE:foc-serial-sim>"
  )
endif()
