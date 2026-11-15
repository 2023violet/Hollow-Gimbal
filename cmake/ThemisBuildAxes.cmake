# Themis build-axis helper. Role: - declare stable public build-axis cache
# entries - validate host/firmware lane combinations - derive normalized
# profile/flavor compile-time contracts for the root build assembly

include_guard(GLOBAL)

function(themis_build_axes_declare_cache)
  set(_themis_default_build_firmware OFF)
  if(CMAKE_CROSSCOMPILING)
    set(_themis_default_build_firmware ON)
  endif()

  set(_themis_default_build_host_tests ON)
  if(CMAKE_CROSSCOMPILING)
    set(_themis_default_build_host_tests OFF)
  endif()

  option(THEMIS_BUILD_FIRMWARE "Build the embedded firmware ELF/HEX/BIN artifacts."
         ${_themis_default_build_firmware}
  )
  option(THEMIS_BUILD_HOST_TESTS "Build native host-side component tests."
         ${_themis_default_build_host_tests}
  )
  set(THEMIS_DEVDRV_PROFILE
      "full-diag"
      CACHE STRING "Themis lite resource profile: tiny, standard, or full-diag."
  )
  set_property(CACHE THEMIS_DEVDRV_PROFILE PROPERTY STRINGS tiny standard full-diag)
  set(THEMIS_PRODUCT_FLAVOR
      "carrier-demo"
      CACHE STRING "Themis product flavor: carrier-demo or headless-tiny."
  )
  set_property(CACHE THEMIS_PRODUCT_FLAVOR PROPERTY STRINGS carrier-demo headless-tiny)
  option(THEMIS_ENABLE_LIBC_FLOAT_PRINTF
         "Force newlib-nano floating-point printf support into firmware builds." OFF
  )
  option(THEMIS_ENABLE_LIBC_FLOAT_SCANF
         "Force newlib-nano floating-point scanf support into firmware builds." OFF
  )
  option(THEMIS_ENABLE_BOARD_BENCH
         "Expose real-board benchmark command surface in non-full-diag firmware images." OFF
  )
  option(
    THEMIS_ENABLE_BENCH_DIAG
    "Build the dedicated real-board diagnostic image with VOFA and energized calibration disabled."
    OFF
  )
  option(THEMIS_ENABLE_BENCH_FIXED_DUTY_TEST
         "Build the bench-only fixed 40/50/60 percent PWM isolation test." OFF
  )
  option(THEMIS_ENABLE_BENCH_RAW_SIXSTEP_TEST
         "Build the bench-only 1 Hz electrical raw six-step PWM test." OFF
  )
  option(THEMIS_ENABLE_BENCH_RAW_SINE_TEST
         "Build the bench-only 1 Hz electrical raw sine PWM test." OFF
  )
  option(THEMIS_ENABLE_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST
         "Build the bench-only 1 Hz raw sine PWM test matched to six-step vector magnitude." OFF
  )
  option(THEMIS_ENABLE_BENCH_RAW_SINE_20_TEST
         "Build the bench-only 1 Hz electrical raw sine PWM test at 20 percent duty amplitude." OFF
  )
  set(THEMIS_MOTOR_BENCH_RUN_BUDGET_MS
      "50"
      CACHE STRING "Bench-only motor run budget in milliseconds (50, 200, or 400)."
  )
  set_property(CACHE THEMIS_MOTOR_BENCH_RUN_BUDGET_MS PROPERTY STRINGS 50 200 400)
endfunction()

function(themis_build_axes_validate)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(WARNING "Themis CMake is primarily validated with GNU/Clang style toolchains. "
                    "Detected compiler: ${CMAKE_C_COMPILER_ID}"
    )
  endif()

  if(THEMIS_BUILD_FIRMWARE AND NOT CMAKE_CROSSCOMPILING)
    message(
      FATAL_ERROR
        "THEMIS_BUILD_FIRMWARE=ON requires a cross-compiling toolchain. "
        "Pass -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-gcc-toolchain.cmake "
        "or disable firmware build for a native-only configure."
    )
  endif()

  if(THEMIS_BUILD_HOST_TESTS AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Host tests must be configured in a native build tree. "
                        "Disable THEMIS_BUILD_HOST_TESTS for cross builds."
    )
  endif()

  if(NOT THEMIS_BUILD_FIRMWARE AND NOT THEMIS_BUILD_HOST_TESTS)
    message(
      FATAL_ERROR "At least one of THEMIS_BUILD_FIRMWARE or THEMIS_BUILD_HOST_TESTS must be ON."
    )
  endif()

  if(NOT THEMIS_MOTOR_BENCH_RUN_BUDGET_MS MATCHES "^(50|200|400|1000|30000)$")
    message(FATAL_ERROR "THEMIS_MOTOR_BENCH_RUN_BUDGET_MS must be 50/200/400/1000/30000. "
                        "Got '${THEMIS_MOTOR_BENCH_RUN_BUDGET_MS}'."
    )
  endif()
  if(NOT THEMIS_ENABLE_BENCH_DIAG AND NOT THEMIS_MOTOR_BENCH_RUN_BUDGET_MS STREQUAL "50")
    message(FATAL_ERROR "A non-50 ms motor bench budget requires THEMIS_ENABLE_BENCH_DIAG=ON.")
  endif()
  if(THEMIS_ENABLE_BENCH_FIXED_DUTY_TEST AND NOT THEMIS_ENABLE_BENCH_DIAG)
    message(FATAL_ERROR "THEMIS_ENABLE_BENCH_FIXED_DUTY_TEST requires THEMIS_ENABLE_BENCH_DIAG=ON.")
  endif()
  if(THEMIS_ENABLE_BENCH_RAW_SIXSTEP_TEST AND NOT THEMIS_ENABLE_BENCH_DIAG)
    message(
      FATAL_ERROR "THEMIS_ENABLE_BENCH_RAW_SIXSTEP_TEST requires THEMIS_ENABLE_BENCH_DIAG=ON."
    )
  endif()
  if(THEMIS_ENABLE_BENCH_RAW_SINE_TEST AND NOT THEMIS_ENABLE_BENCH_DIAG)
    message(FATAL_ERROR "THEMIS_ENABLE_BENCH_RAW_SINE_TEST requires THEMIS_ENABLE_BENCH_DIAG=ON.")
  endif()
  if(THEMIS_ENABLE_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST AND NOT THEMIS_ENABLE_BENCH_DIAG)
    message(
      FATAL_ERROR
        "THEMIS_ENABLE_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST requires THEMIS_ENABLE_BENCH_DIAG=ON."
    )
  endif()
  if(THEMIS_ENABLE_BENCH_RAW_SINE_20_TEST AND NOT THEMIS_ENABLE_BENCH_DIAG)
    message(
      FATAL_ERROR "THEMIS_ENABLE_BENCH_RAW_SINE_20_TEST requires THEMIS_ENABLE_BENCH_DIAG=ON."
    )
  endif()
  if((THEMIS_ENABLE_BENCH_FIXED_DUTY_TEST AND THEMIS_ENABLE_BENCH_RAW_SIXSTEP_TEST)
     OR (THEMIS_ENABLE_BENCH_FIXED_DUTY_TEST AND THEMIS_ENABLE_BENCH_RAW_SINE_TEST)
     OR (THEMIS_ENABLE_BENCH_FIXED_DUTY_TEST AND THEMIS_ENABLE_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST)
     OR (THEMIS_ENABLE_BENCH_RAW_SIXSTEP_TEST AND THEMIS_ENABLE_BENCH_RAW_SINE_TEST)
     OR (THEMIS_ENABLE_BENCH_RAW_SIXSTEP_TEST AND THEMIS_ENABLE_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST)
     OR (THEMIS_ENABLE_BENCH_RAW_SIXSTEP_TEST AND THEMIS_ENABLE_BENCH_RAW_SINE_20_TEST)
     OR (THEMIS_ENABLE_BENCH_RAW_SINE_TEST AND THEMIS_ENABLE_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST)
     OR (THEMIS_ENABLE_BENCH_RAW_SINE_TEST AND THEMIS_ENABLE_BENCH_RAW_SINE_20_TEST)
     OR (THEMIS_ENABLE_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST AND THEMIS_ENABLE_BENCH_RAW_SINE_20_TEST)
  )
    message(FATAL_ERROR "Only one raw PWM bench test may be enabled at a time.")
  endif()

  string(TOLOWER "${THEMIS_DEVDRV_PROFILE}" _themis_devdrv_profile)
  if(NOT _themis_devdrv_profile MATCHES "^(tiny|standard|full-diag)$")
    message(FATAL_ERROR "THEMIS_DEVDRV_PROFILE must be one of: tiny, standard, full-diag. "
                        "Got '${THEMIS_DEVDRV_PROFILE}'."
    )
  endif()

  string(TOLOWER "${THEMIS_PRODUCT_FLAVOR}" _themis_product_flavor)
  if(NOT _themis_product_flavor MATCHES "^(carrier-demo|headless-tiny)$")
    message(FATAL_ERROR "THEMIS_PRODUCT_FLAVOR must be one of: carrier-demo, headless-tiny. "
                        "Got '${THEMIS_PRODUCT_FLAVOR}'."
    )
  endif()

  if(_themis_devdrv_profile STREQUAL "tiny")
    set(_themis_devdrv_profile_define "TM_BUILD_PROFILE_TINY=1")
  elseif(_themis_devdrv_profile STREQUAL "standard")
    set(_themis_devdrv_profile_define "TM_BUILD_PROFILE_STANDARD=1")
  else()
    set(_themis_devdrv_profile_define "TM_BUILD_PROFILE_FULL_DIAG=1")
  endif()

  if(_themis_product_flavor STREQUAL "headless-tiny")
    set(_themis_product_flavor_define "TM_PRODUCT_FLAVOR_HEADLESS_TINY=1")
  else()
    set(_themis_product_flavor_define "TM_PRODUCT_FLAVOR_CARRIER_DEMO=1")
  endif()

  set(THEMIS_DEVDRV_PROFILE_NORMALIZED
      "${_themis_devdrv_profile}"
      PARENT_SCOPE
  )
  set(THEMIS_PRODUCT_FLAVOR_NORMALIZED
      "${_themis_product_flavor}"
      PARENT_SCOPE
  )
  set(THEMIS_DEVDRV_PROFILE_COMPILE_DEFINITION
      "${_themis_devdrv_profile_define}"
      PARENT_SCOPE
  )
  set(THEMIS_PRODUCT_FLAVOR_COMPILE_DEFINITION
      "${_themis_product_flavor_define}"
      PARENT_SCOPE
  )
endfunction()
