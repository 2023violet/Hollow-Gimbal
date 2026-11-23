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

option(FALCONFOC_RT02_TIMING_TRACE "Capture local Board1 execution and ADC/H0 timing" OFF)
option(FALCONFOC_BOARD_CAPTURE "Enable the locked Board1 SRAM capture recorder" OFF)
option(FALCONFOC_BOARD_PERSISTENCE "Link the AT32 Flash settings store (save, boot restore)" OFF)
set(FALCONFOC_BOARD_CAPTURE_BUILD_TAG
    "0"
    CACHE STRING "Declared recorder build tag (uint32)"
)
set(FALCONFOC_BOARD_CALIBRATION_SUPPLY
    "NONE"
    CACHE STRING "Build-time calibration supply of the ARM image: NONE or VERIFICATION"
)
set_property(CACHE FALCONFOC_BOARD_CALIBRATION_SUPPLY PROPERTY STRINGS NONE VERIFICATION)
set(FALCONFOC_BOARD_VERIFICATION_SUPPLY_ID
    "0x56464331"
    CACHE STRING "Supply identity accepted by the synthetic verification build"
)
set(FALCONFOC_BOARD_VERIFICATION_POLE_PAIRS
    "11"
    CACHE STRING "Expected motor pole pairs of the synthetic verification build"
)
if(NOT FALCONFOC_BOARD_CALIBRATION_SUPPLY MATCHES "^(NONE|VERIFICATION)$")
  message(FATAL_ERROR "FALCONFOC_BOARD_CALIBRATION_SUPPLY must be NONE or VERIFICATION")
endif()
if(NOT FALCONFOC_BOARD_VERIFICATION_SUPPLY_ID MATCHES "^0[xX][0-9a-fA-F]+$"
   OR NOT FALCONFOC_BOARD_VERIFICATION_POLE_PAIRS MATCHES "^[1-9][0-9]*$"
)
  message(FATAL_ERROR "Supply a hexadecimal supply ID and a positive pole-pair count")
endif()
# Range checks on the original text, before any C conversion can narrow it. The
# verification supply ID is a nonzero uint32; zero is reserved for NONE.
string(REGEX REPLACE "^0[xX]0*" "" supply_id_digits "${FALCONFOC_BOARD_VERIFICATION_SUPPLY_ID}")
string(LENGTH "${supply_id_digits}" supply_id_length)
if(supply_id_length EQUAL 0 OR supply_id_length GREATER 8)
  message(
    FATAL_ERROR
      "FALCONFOC_BOARD_VERIFICATION_SUPPLY_ID must be a nonzero uint32 (0x1..0xFFFFFFFF), got "
      "'${FALCONFOC_BOARD_VERIFICATION_SUPPLY_ID}'"
  )
endif()
string(LENGTH "${FALCONFOC_BOARD_VERIFICATION_POLE_PAIRS}" pole_pairs_length)
if(pole_pairs_length GREATER 2 OR FALCONFOC_BOARD_VERIFICATION_POLE_PAIRS GREATER 64)
  message(FATAL_ERROR "FALCONFOC_BOARD_VERIFICATION_POLE_PAIRS must be 1..64")
endif()

set(repo "${CMAKE_CURRENT_LIST_DIR}/../..")
set(vendor "${repo}/platform/at/at32f415/vendor")
set(cmsis "${vendor}/cmsis/cm4")
set(startup "${cmsis}/device_support/startup/gcc")
add_subdirectory("${repo}/protocol" protocol)

add_library(
  falconfoc-board1 STATIC
  src/foc_board.c
  src/foc_board_boot.c
  src/foc_board_calibrate.c
  src/foc_board_store.c
  src/foc_board_timing.c
  src/foc_board_capture.c
  src/foc_serial.c
  src/foc_uart_buffer.c
  "${repo}/core/src/foc_core.c"
  "${repo}/core/src/foc_current.c"
  "${repo}/core/src/foc_impedance.c"
  "${repo}/core/src/foc_motion.c"
  "${repo}/core/src/foc_motion_limits.c"
  "${repo}/core/src/foc_following_error.c"
  "${repo}/core/src/foc_trajectory.c"
  "${repo}/calibration/src/foc_calibration.c"
  "${repo}/calibration/src/foc_motor_id.c"
  "${repo}/calibration/src/foc_flux_id.c"
  "${repo}/sensors/src/foc_encoder.c"
  "${repo}/g3/np/src/foc_device.c"
  "${repo}/g3/np/src/foc_device_sim.c"
  "${repo}/g3/safe/src/foc_safe.c"
  "${repo}/g3/safe/src/foc_protect.c"
  "${repo}/runtime/src/foc_pipeline.c"
  "${repo}/runtime/src/foc_runtime.c"
)
target_link_libraries(falconfoc-board1 PUBLIC falconfoc-wire)
if(FALCONFOC_RT02_TIMING_TRACE)
  target_compile_definitions(falconfoc-board1 PUBLIC FALCONFOC_RT02_TIMING_TRACE=1)
endif()
if(FALCONFOC_BOARD_CAPTURE)
  if(NOT FALCONFOC_BOARD_CAPTURE_BUILD_TAG MATCHES "^(0|[1-9][0-9]*|0[xX][0-9a-fA-F]+)$")
    message(FATAL_ERROR "Supply a numeric FALCONFOC_BOARD_CAPTURE_BUILD_TAG")
  endif()
  target_compile_definitions(
    falconfoc-board1 PUBLIC FALCONFOC_BOARD_CAPTURE=1
                            FOC_CAPTURE_BUILD_TAG=${FALCONFOC_BOARD_CAPTURE_BUILD_TAG}
  )
endif()
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

# Read-only calibration supply. The default carries none; the synthetic
# verification supply is test data and is never linked by the default image.
add_library(
  falconfoc-board1-supply-none STATIC src/foc_board_supply_none.c src/foc_board_expectation.c
)
target_link_libraries(falconfoc-board1-supply-none PUBLIC falconfoc-board1)
target_compile_definitions(
  falconfoc-board1-supply-none PRIVATE FOC_BOARD_EXPECTED_SUPPLY_ID=0ULL
                                       FOC_BOARD_EXPECTED_POLE_PAIRS=7U
)
add_library(
  falconfoc-board1-supply-verification STATIC src/foc_board_supply_verification.c
                                              src/foc_board_expectation.c
)
target_link_libraries(falconfoc-board1-supply-verification PUBLIC falconfoc-board1)
target_compile_definitions(
  falconfoc-board1-supply-verification
  PRIVATE FOC_BOARD_EXPECTED_SUPPLY_ID=${FALCONFOC_BOARD_VERIFICATION_SUPPLY_ID}ULL
          FOC_BOARD_EXPECTED_SUPPLY_REQUIRED=1
          FOC_BOARD_EXPECTED_POLE_PAIRS=${FALCONFOC_BOARD_VERIFICATION_POLE_PAIRS}U
)
foreach(supply falconfoc-board1-supply-none falconfoc-board1-supply-verification)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${supply} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

if(CMAKE_CROSSCOMPILING)
  if(NOT MCU_CPU STREQUAL "cortex-m4"
     OR NOT MCU_FLOAT STREQUAL "soft"
     OR NOT MCU_FPU STREQUAL "none"
  )
    message(FATAL_ERROR "Use MCU_CPU=cortex-m4, MCU_FLOAT=soft and MCU_FPU=none")
  endif()
  if(FALCONFOC_BOARD_CALIBRATION_SUPPLY STREQUAL "VERIFICATION")
    set(image falconfoc-v2-at32f415-calibration-verification)
    set(supply falconfoc-board1-supply-verification)
  else()
    set(image falconfoc-v2-at32f415)
    set(supply falconfoc-board1-supply-none)
  endif()
  set(linker "${startup}/linker/AT32F415xB_FLASH.ld")
  set(stack "${CMAKE_CURRENT_LIST_DIR}/stack-budget.ld")
  set(store_reserve "${CMAKE_CURRENT_LIST_DIR}/store-reserve.ld")
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
  set_target_properties(
    ${image} PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${linker};${stack};${store_reserve}"
  )
  if(FALCONFOC_BOARD_PERSISTENCE)
    # Opt-in: the default image keeps no Flash erase/program code.
    target_sources(${image} PRIVATE src/foc_at32_store.c)
    target_compile_definitions(${image} PRIVATE FALCONFOC_BOARD_PERSISTENCE=1)
  endif()
  target_compile_definitions(${image} PRIVATE AT32F415KBU7_4 USE_STDPERIPH_DRIVER)
  target_include_directories(
    ${image} PRIVATE "${cmsis}/core_support" "${cmsis}/device_support" "${vendor}/drivers/inc"
  )
  target_link_libraries(${image} PRIVATE ${supply} falconfoc-board1)
  target_compile_options(falconfoc-board1 PRIVATE -Os -fstack-usage)
  target_compile_options(${supply} PRIVATE -Os -fstack-usage)
  target_compile_options(${image} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Os;-fstack-usage;-Wextra>)
  target_link_options(
    ${image}
    PRIVATE
    "-T${linker}"
    "-T${stack}"
    "-T${store_reserve}"
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
  add_executable(test-foc-pwm-port tests/test_foc_pwm_port.c src/foc_board_pwm_model.c)
  target_link_libraries(test-foc-pwm-port PRIVATE falconfoc-board1)
  add_test(NAME test-foc-pwm-port COMMAND test-foc-pwm-port)
  add_executable(test-foc-timing tests/test_foc_timing.c)
  target_link_libraries(test-foc-timing PRIVATE falconfoc-board1)
  add_test(NAME test-foc-timing COMMAND test-foc-timing)
  add_executable(test-foc-board-capture tests/test_foc_board_capture.c)
  target_link_libraries(test-foc-board-capture PRIVATE falconfoc-board1)
  add_test(NAME test-foc-board-capture COMMAND test-foc-board-capture)
  add_executable(test-foc-board-config tests/test_foc_board_config.c)
  target_link_libraries(test-foc-board-config PRIVATE falconfoc-board1)
  add_test(NAME test-foc-board-config COMMAND test-foc-board-config)
  add_executable(test-foc-board-boot tests/test_foc_board_boot.c)
  target_link_libraries(test-foc-board-boot PRIVATE falconfoc-board1-supply-verification)
  add_test(NAME test-foc-board-boot COMMAND test-foc-board-boot)
  add_executable(test-foc-board-boot-default tests/test_foc_board_boot_default.c)
  target_link_libraries(test-foc-board-boot-default PRIVATE falconfoc-board1-supply-none)
  add_test(NAME test-foc-board-boot-default COMMAND test-foc-board-boot-default)
  add_executable(test-foc-board-store tests/test_foc_board_store.c)
  target_link_libraries(test-foc-board-store PRIVATE falconfoc-board1-supply-none)
  add_test(NAME test-foc-board-store COMMAND test-foc-board-store)
  add_executable(test-foc-serial tests/test_foc_serial.c)
  target_link_libraries(test-foc-serial PRIVATE falconfoc-board1)
  add_test(NAME test-foc-serial COMMAND test-foc-serial)
  add_executable(foc-serial-sim tests/foc_serial_sim.c)
  target_link_libraries(foc-serial-sim PRIVATE falconfoc-board1-supply-none)
  add_executable(foc-serial-sim-verification tests/foc_serial_sim.c)
  target_link_libraries(foc-serial-sim-verification PRIVATE falconfoc-board1-supply-verification)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME test-foc-capture-import
           COMMAND "${Python3_EXECUTABLE}" -B
                   "${repo}/tools/falconfoc-calibration-replay/tests/test_board_capture.py"
  )
  set_tests_properties(
    test-foc-capture-import PROPERTIES ENVIRONMENT
                                       "FOC_CAPTURE_FIXTURE=$<TARGET_FILE:test-foc-board-capture>"
  )
  add_test(NAME test-foc-locked-timing-import
           COMMAND "${Python3_EXECUTABLE}" -B
                   "${repo}/tools/falconfoc-calibration-replay/tests/test_locked_timing.py"
  )
  # Formal boot -> production SRAM writer -> Host import -> Locked Timing, both boots.
  add_executable(foc-board-capture-boot-fixture tests/foc_board_capture_boot_fixture.c)
  target_link_libraries(foc-board-capture-boot-fixture PRIVATE falconfoc-board1-supply-verification)
  # FW-ADC-01: production ADC_ZERO trace -> existing estimator -> isolated Runtime.
  add_executable(foc-board-adc-candidate tests/foc_board_adc_candidate.c)
  target_link_libraries(foc-board-adc-candidate PRIVATE falconfoc-board1-supply-verification)
  add_test(NAME test-foc-adc-candidate
           COMMAND "${Python3_EXECUTABLE}" -B
                   "${repo}/tools/falconfoc-calibration-replay/tests/test_board_adc_candidate.py"
  )
  set_tests_properties(
    test-foc-adc-candidate
    PROPERTIES
      ENVIRONMENT
      "FOC_CAPTURE_BOOT_FIXTURE=$<TARGET_FILE:foc-board-capture-boot-fixture>;FOC_ADC_CANDIDATE=$<TARGET_FILE:foc-board-adc-candidate>"
  )
  add_test(NAME test-foc-capture-boot-import
           COMMAND "${Python3_EXECUTABLE}" -B
                   "${repo}/tools/falconfoc-calibration-replay/tests/test_board_capture_boot.py"
  )
  set_tests_properties(
    test-foc-capture-boot-import
    PROPERTIES ENVIRONMENT "FOC_CAPTURE_BOOT_FIXTURE=$<TARGET_FILE:foc-board-capture-boot-fixture>"
  )
  add_test(NAME test-foc-python COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s
                                        "${repo}/tools/falconfoc-cli" -p test_falconfoc_cli.py -v
  )
  set_tests_properties(
    test-foc-python PROPERTIES ENVIRONMENT "FOC_SERIAL_SIM=$<TARGET_FILE:foc-serial-sim>"
  )
  # M0 Board1 SIL: the production Board1 path (default build supply) against a
  # virtual motor, driven over the V1 wire by the CLI. Host only; never ARM.
  add_executable(
    falconfoc-board1-sil sil/foc_board_sil.c "${repo}/simulation/src/foc_virtual_motor.c"
  )
  target_include_directories(
    falconfoc-board1-sil PRIVATE "${repo}/simulation/include" "${repo}/simulation/src"
  )
  target_link_libraries(falconfoc-board1-sil PRIVATE falconfoc-board1-supply-none)
  add_test(NAME test-foc-board1-sil COMMAND "${Python3_EXECUTABLE}" -B
                                            "${repo}/tools/falconfoc-cli/test_board1_sil.py"
  )
  set_tests_properties(
    test-foc-board1-sil PROPERTIES ENVIRONMENT "FOC_BOARD1_SIL=$<TARGET_FILE:falconfoc-board1-sil>"
  )
endif()
