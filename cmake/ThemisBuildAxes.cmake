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
