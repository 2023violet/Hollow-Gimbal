# Themis root build entry. Role: - define build axes, platform selection, and contract targets -
# compose layer subdirectories and final firmware product - keep child CMakeLists declarative while
# global policy stays centralized

cmake_minimum_required(VERSION 3.25)

set(THEMIS_PROJECT_NAME "falcon-foc-at32")

project(
  ${THEMIS_PROJECT_NAME}
  VERSION 2.1.0
  DESCRIPTION "Themis-aligned Falcon FOC firmware build"
  LANGUAGES C CXX ASM
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ThemisBuild)

function(themis_resolve_repo_path out_var raw_path)
  if("${raw_path}" STREQUAL "")
    set(${out_var}
        ""
        PARENT_SCOPE
    )
    return()
  endif()

  if(IS_ABSOLUTE "${raw_path}")
    get_filename_component(_resolved_path "${raw_path}" ABSOLUTE)
  else()
    get_filename_component(
      _resolved_path
      "${raw_path}"
      ABSOLUTE
      BASE_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}"
    )
  endif()

  set(${out_var}
      "${_resolved_path}"
      PARENT_SCOPE
  )
endfunction()

if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  message(WARNING "Themis CMake is primarily validated with GNU/Clang style toolchains. "
                  "Detected compiler: ${CMAKE_C_COMPILER_ID}"
  )
endif()

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

set(_themis_default_feature_shell OFF)
set(_themis_default_feature_vofa OFF)
if(_themis_devdrv_profile STREQUAL "tiny")
  set(_themis_default_feature_ipc OFF)
else()
  set(_themis_default_feature_ipc OFF)
endif()
if(_themis_devdrv_profile STREQUAL "full-diag")
  set(_themis_default_feature_task_budget ON)
  set(_themis_default_feature_verbose_log ON)
else()
  set(_themis_default_feature_task_budget OFF)
  set(_themis_default_feature_verbose_log OFF)
endif()

option(THEMIS_FEATURE_SHELL "Compile FalconFoc shell transport, session, and commands."
       ${_themis_default_feature_shell}
)
option(THEMIS_FEATURE_VOFA "Compile FalconFoc VOFA telemetry surfaces."
       ${_themis_default_feature_vofa}
)
option(THEMIS_FEATURE_IPC "Compile Themis IPC runtime and broker task."
       ${_themis_default_feature_ipc}
)
option(THEMIS_FEATURE_TASK_BUDGET "Compile task-budget probe support."
       ${_themis_default_feature_task_budget}
)
option(THEMIS_FEATURE_VERBOSE_LOG "Compile verbose logging defaults."
       ${_themis_default_feature_verbose_log}
)

string(TOLOWER "${THEMIS_PRODUCT_FLAVOR}" _themis_product_flavor)
if(NOT _themis_product_flavor MATCHES "^(carrier-demo|headless-tiny)$")
  message(FATAL_ERROR "THEMIS_PRODUCT_FLAVOR must be one of: carrier-demo, headless-tiny. "
                      "Got '${THEMIS_PRODUCT_FLAVOR}'."
  )
endif()

set(_themis_devdrv_profile_define "")
if(_themis_devdrv_profile STREQUAL "tiny")
  set(_themis_devdrv_profile_define "TM_BUILD_PROFILE_TINY=1")
elseif(_themis_devdrv_profile STREQUAL "standard")
  set(_themis_devdrv_profile_define "TM_BUILD_PROFILE_STANDARD=1")
else()
  set(_themis_devdrv_profile_define "TM_BUILD_PROFILE_FULL_DIAG=1")
endif()

set(_themis_product_flavor_define "")
if(_themis_product_flavor STREQUAL "headless-tiny")
  set(_themis_product_flavor_define "TM_PRODUCT_FLAVOR_HEADLESS_TINY=1")
else()
  set(_themis_product_flavor_define "TM_PRODUCT_FLAVOR_CARRIER_DEMO=1")
endif()

set(THEMIS_PLATFORM_VENDOR
    "at"
    CACHE STRING "Themis platform vendor directory name."
)
set(THEMIS_PLATFORM_SOC
    "at32f415"
    CACHE STRING "Themis platform SoC directory name."
)
set(THEMIS_FIRMWARE_TARGET_NAME
    "falcon-foc-at32"
    CACHE STRING "Canonical firmware executable target and artifact basename."
)
set(THEMIS_PLATFORM_DIR_OVERRIDE
    ""
    CACHE PATH "Optional absolute or source-relative platform directory override."
)
set(THEMIS_PLATFORM_TARGET_OVERRIDE
    ""
    CACHE STRING "Optional exported platform target-name override."
)
set(THEMIS_ENTRY_SOURCE_OVERRIDE
    ""
    CACHE FILEPATH "Optional firmware entry source override."
)
set(THEMIS_STARTUP_SOURCE_OVERRIDE
    ""
    CACHE FILEPATH "Optional platform startup assembly override."
)
set(THEMIS_LINKER_SCRIPT_OVERRIDE
    ""
    CACHE FILEPATH "Optional firmware linker-script override."
)
set(THEMIS_OUTPUT_DIR
    "${CMAKE_BINARY_DIR}/output"
    CACHE PATH "Directory for firmware artifacts."
)
set(THEMIS_FREERTOS_HEAP_IMPL
    "heap_4"
    CACHE STRING "Selected FreeRTOS heap backend implementation."
)
set_property(
  CACHE THEMIS_FREERTOS_HEAP_IMPL
  PROPERTY STRINGS
           heap_1
           heap_2
           heap_3
           heap_4
           heap_5
)
set(THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE
    ""
    CACHE STRING "Optional platform compile-definition override list."
)

set(_themis_platform_dir_default
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/${THEMIS_PLATFORM_VENDOR}/${THEMIS_PLATFORM_SOC}"
)
set(_themis_platform_target_default "${THEMIS_PLATFORM_SOC}-hal")
set(_themis_entry_source_default
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/${THEMIS_PLATFORM_VENDOR}/${THEMIS_PLATFORM_SOC}/generated/src/main.c"
)
set(_themis_startup_source_default
    "${_themis_platform_dir_default}/vendor/cmsis/cm4/device_support/startup/gcc/startup_at32f415.s"
)
set(_themis_linker_script_default
    "${_themis_platform_dir_default}/vendor/cmsis/cm4/device_support/startup/gcc/linker/AT32F415xB_FLASH.ld"
)
set(_themis_platform_compile_definitions_default "")
if((THEMIS_PLATFORM_VENDOR STREQUAL "at") AND (THEMIS_PLATFORM_SOC STREQUAL "at32f415"))
  set(_themis_platform_compile_definitions_default
      "AT32F415KBU7_4;USE_STDPERIPH_DRIVER;ARM_MATH_CM4;ARM_MATH_MATRIX_CHECK;ARM_MATH_ROUNDING;ARM_MATH_LOOPUNROLL"
  )
endif()

themis_resolve_repo_path(_themis_platform_dir_override "${THEMIS_PLATFORM_DIR_OVERRIDE}")
themis_resolve_repo_path(_themis_entry_source_override "${THEMIS_ENTRY_SOURCE_OVERRIDE}")
themis_resolve_repo_path(_themis_startup_source_override "${THEMIS_STARTUP_SOURCE_OVERRIDE}")
themis_resolve_repo_path(_themis_linker_script_override "${THEMIS_LINKER_SCRIPT_OVERRIDE}")

if(_themis_platform_dir_override)
  set(THEMIS_PLATFORM_DIR "${_themis_platform_dir_override}")
else()
  set(THEMIS_PLATFORM_DIR "${_themis_platform_dir_default}")
endif()

if(THEMIS_PLATFORM_TARGET_OVERRIDE)
  set(THEMIS_PLATFORM_TARGET "${THEMIS_PLATFORM_TARGET_OVERRIDE}")
else()
  set(THEMIS_PLATFORM_TARGET "${_themis_platform_target_default}")
endif()

if(_themis_entry_source_override)
  set(THEMIS_ENTRY_SOURCE "${_themis_entry_source_override}")
else()
  set(THEMIS_ENTRY_SOURCE "${_themis_entry_source_default}")
endif()

if(_themis_startup_source_override)
  set(THEMIS_STARTUP_SOURCE "${_themis_startup_source_override}")
else()
  set(THEMIS_STARTUP_SOURCE "${_themis_startup_source_default}")
endif()

if(_themis_linker_script_override)
  set(_themis_linker_script_source "${_themis_linker_script_override}")
else()
  set(_themis_linker_script_source "${_themis_linker_script_default}")
endif()

if(THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE)
  set(THEMIS_PLATFORM_COMPILE_DEFINITIONS "${THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE}")
else()
  set(THEMIS_PLATFORM_COMPILE_DEFINITIONS "${_themis_platform_compile_definitions_default}")
endif()

if(THEMIS_FEATURE_SHELL)
  set(_themis_default_linker_min_heap_size "0x200")
else()
  set(_themis_default_linker_min_heap_size "0x0")
endif()
set(THEMIS_LINKER_MIN_HEAP_SIZE
    "${_themis_default_linker_min_heap_size}"
    CACHE STRING "Minimum linker-reserved heap size for the firmware image."
)
set(THEMIS_LINKER_MIN_STACK_SIZE
    "0x400"
    CACHE STRING "Minimum linker-reserved stack size for the firmware image."
)

if(THEMIS_BUILD_FIRMWARE)
  set(_themis_generated_linker_dir "${CMAKE_BINARY_DIR}/generated/linker")
  file(MAKE_DIRECTORY "${_themis_generated_linker_dir}")
  get_filename_component(_themis_linker_script_name "${_themis_linker_script_source}" NAME)
  set(_themis_generated_linker_script
      "${_themis_generated_linker_dir}/${_themis_linker_script_name}"
  )
  configure_file("${_themis_linker_script_source}" "${_themis_generated_linker_script}" @ONLY)
  set(THEMIS_LINKER_SCRIPT "${_themis_generated_linker_script}")
else()
  set(THEMIS_LINKER_SCRIPT "${_themis_linker_script_source}")
endif()

if(THEMIS_BUILD_FIRMWARE)
  foreach(_required_path IN ITEMS "${THEMIS_PLATFORM_DIR}" "${THEMIS_ENTRY_SOURCE}"
                                  "${THEMIS_STARTUP_SOURCE}" "${THEMIS_LINKER_SCRIPT}"
  )
    if(NOT EXISTS "${_required_path}")
      message(FATAL_ERROR "Themis firmware path does not exist: ${_required_path}")
    endif()
  endforeach()
endif()

add_library(themis_root_headers INTERFACE)
target_include_directories(
  themis_root_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                "${CMAKE_CURRENT_SOURCE_DIR}/include/config"
)
set_target_properties(themis_root_headers PROPERTIES FOLDER "themis/contracts")

add_library(themis_build_contract INTERFACE)
target_compile_definitions(
  themis_build_contract
  INTERFACE $<$<CONFIG:Debug>:DEBUG>
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:NDEBUG>
            ${_themis_devdrv_profile_define}
            ${_themis_product_flavor_define}
            "TM_FEATURE_SHELL=$<IF:$<BOOL:${THEMIS_FEATURE_SHELL}>,1,0>"
            "TM_FEATURE_VOFA=$<IF:$<BOOL:${THEMIS_FEATURE_VOFA}>,1,0>"
            "TM_FEATURE_IPC=$<IF:$<BOOL:${THEMIS_FEATURE_IPC}>,1,0>"
            "TM_FEATURE_TASK_BUDGET=$<IF:$<BOOL:${THEMIS_FEATURE_TASK_BUDGET}>,1,0>"
            "TM_FEATURE_VERBOSE_LOG=$<IF:$<BOOL:${THEMIS_FEATURE_VERBOSE_LOG}>,1,0>"
            $<$<BOOL:${THEMIS_ENABLE_BOARD_BENCH}>:TM_ENABLE_BOARD_BENCH=1>
)
target_compile_options(
  themis_build_contract
  INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-std=gnu17>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-std=gnu++17>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-fmessage-length=0>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fmessage-length=0>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-fno-common>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-common>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-fstack-usage>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fstack-usage>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wextra>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wextra>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wshadow>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wshadow>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wundef>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wundef>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wno-pedantic>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wno-pedantic>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-ffunction-sections>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-ffunction-sections>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-fdata-sections>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fdata-sections>
    $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:C,GNU,Clang>>:-Og>
    $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-Og>
    $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:C,GNU,Clang>>:-g3>
    $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-g3>
    $<$<AND:$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>,$<COMPILE_LANG_AND_ID:C,GNU,Clang>>:-Os>
    $<$<AND:$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-Os>
    $<$<AND:$<CONFIG:RelWithDebInfo>,$<COMPILE_LANG_AND_ID:C,GNU,Clang>>:-O2>
    $<$<AND:$<CONFIG:RelWithDebInfo>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-O2>
)
set_target_properties(themis_build_contract PROPERTIES FOLDER "themis/contracts")

add_library(themis_platform_contract INTERFACE)
target_link_libraries(themis_platform_contract INTERFACE themis_root_headers)
if(THEMIS_PLATFORM_COMPILE_DEFINITIONS)
  target_compile_definitions(
    themis_platform_contract INTERFACE ${THEMIS_PLATFORM_COMPILE_DEFINITIONS}
  )
endif()
set_target_properties(themis_platform_contract PROPERTIES FOLDER "themis/contracts")

add_library(themis_host_test_contract INTERFACE)
target_link_libraries(themis_host_test_contract INTERFACE themis_root_headers themis_build_contract)
target_include_directories(
  themis_host_test_contract INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/subsys/assert"
)
target_compile_definitions(themis_host_test_contract INTERFACE THEMIS_HOST_TEST=1)
set_target_properties(themis_host_test_contract PROPERTIES FOLDER "themis/contracts")

message(STATUS "Themis build firmware : ${THEMIS_BUILD_FIRMWARE}")
message(STATUS "Themis build host tests : ${THEMIS_BUILD_HOST_TESTS}")
message(STATUS "Themis devdrv profile : ${_themis_devdrv_profile}")
message(STATUS "Themis product flavor : ${_themis_product_flavor}")
message(STATUS "Themis feature shell : ${THEMIS_FEATURE_SHELL}")
message(STATUS "Themis feature vofa : ${THEMIS_FEATURE_VOFA}")
message(STATUS "Themis feature ipc : ${THEMIS_FEATURE_IPC}")
message(STATUS "Themis feature task budget : ${THEMIS_FEATURE_TASK_BUDGET}")
message(STATUS "Themis feature verbose log : ${THEMIS_FEATURE_VERBOSE_LOG}")
message(STATUS "Themis firmware target : ${THEMIS_FIRMWARE_TARGET_NAME}")
message(STATUS "Themis libc float printf : ${THEMIS_ENABLE_LIBC_FLOAT_PRINTF}")
message(STATUS "Themis libc float scanf : ${THEMIS_ENABLE_LIBC_FLOAT_SCANF}")
message(STATUS "Themis board bench : ${THEMIS_ENABLE_BOARD_BENCH}")
if(THEMIS_BUILD_FIRMWARE)
  message(STATUS "Themis platform : ${THEMIS_PLATFORM_VENDOR}/${THEMIS_PLATFORM_SOC}")
  message(STATUS "Themis linker script : ${THEMIS_LINKER_SCRIPT}")
  message(STATUS "Themis linker min heap : ${THEMIS_LINKER_MIN_HEAP_SIZE}")
  message(STATUS "Themis linker min stack : ${THEMIS_LINKER_MIN_STACK_SIZE}")
endif()

add_subdirectory(components/utils)
add_subdirectory(components/protocol)

if(THEMIS_BUILD_FIRMWARE)
  add_subdirectory(
    "${THEMIS_PLATFORM_DIR}"
    "${CMAKE_BINARY_DIR}/platform/${THEMIS_PLATFORM_VENDOR}_${THEMIS_PLATFORM_SOC}"
  )
  add_subdirectory(kernel/freertos)
  add_subdirectory(subsys)
  add_subdirectory(components/algorithm)
  add_subdirectory(bsp)
  add_subdirectory(applications)

  themis_add_firmware(
    NAME
    ${THEMIS_FIRMWARE_TARGET_NAME}
    ENTRY_SOURCE
    "${THEMIS_ENTRY_SOURCE}"
    STARTUP_SOURCE
    "${THEMIS_STARTUP_SOURCE}"
    LINKER_SCRIPT
    "${THEMIS_LINKER_SCRIPT}"
    OUTPUT_DIR
    "${THEMIS_OUTPUT_DIR}"
    PRIVATE_DEPS
    protocol
    utils
    freertos
    ${THEMIS_PLATFORM_TARGET}
    WHOLE_ARCHIVE_DEPS
    algorithm
    applications
    bsp
    subsys-core
    SYSTEM_LIBS
    m
    c
    FOLDER
    "themis/firmware"
  )
endif()

if(THEMIS_BUILD_HOST_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
