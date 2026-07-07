# Themis root build entry. Role: - declare stable public build entry points -
# orchestrate metadata/contract/status helpers in one readable sequence -
# compose layer subdirectories and the final firmware/host products

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
include(ThemisPlatformMetadata)
include(ThemisBuildAxes)
include(ThemisFirmwareAssembly)
include(ThemisContracts)
include(ThemisStatusReport)

themis_build_axes_declare_cache()

# Standard daily build entry --------------------------------------------------------------------
themis_metadata_declare_platform_selection_cache()
set(THEMIS_FIRMWARE_TARGET_NAME
    "${THEMIS_PROJECT_NAME}"
    CACHE STRING "Canonical firmware executable target and artifact basename."
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

# Expert metadata override entry ----------------------------------------------------------------
themis_metadata_declare_platform_override_cache()

# Build-axis validation -------------------------------------------------------------------------
themis_build_axes_validate()

# Firmware-only metadata assembly ---------------------------------------------------------------
if(THEMIS_BUILD_FIRMWARE)
  themis_firmware_assemble("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

themis_define_contract_targets("${CMAKE_CURRENT_SOURCE_DIR}")
themis_emit_status_report()

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
