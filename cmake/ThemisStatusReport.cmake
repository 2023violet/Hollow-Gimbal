# Themis configure status helper. Role: - print one stable configure summary for
# host and firmware lanes - classify whether metadata came from standard cache or
# expert overrides - keep root build narration free from raw status plumbing

include_guard(GLOBAL)

function(themis_emit_status_report)
  set(_themis_metadata_entry_lane "standard-cache")
  if(THEMIS_BUILD_FIRMWARE
     AND (THEMIS_PLATFORM_DIR_OVERRIDE
          OR THEMIS_PLATFORM_TARGET_OVERRIDE
          OR THEMIS_ENTRY_SOURCE_OVERRIDE
          OR THEMIS_STARTUP_SOURCE_OVERRIDE
          OR THEMIS_LINKER_SCRIPT_OVERRIDE
          OR THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE
          OR THEMIS_CPU_CLASS_OVERRIDE
         )
  )
    set(_themis_metadata_entry_lane "expert-override")
  endif()

  message(STATUS "Themis build firmware : ${THEMIS_BUILD_FIRMWARE}")
  message(STATUS "Themis build host tests : ${THEMIS_BUILD_HOST_TESTS}")
  message(STATUS "Themis devdrv profile : ${THEMIS_DEVDRV_PROFILE_NORMALIZED}")
  message(STATUS "Themis product flavor : ${THEMIS_PRODUCT_FLAVOR_NORMALIZED}")
  message(STATUS "Themis firmware target : ${THEMIS_FIRMWARE_TARGET_NAME}")
  message(STATUS "Themis libc float printf : ${THEMIS_ENABLE_LIBC_FLOAT_PRINTF}")
  message(STATUS "Themis libc float scanf : ${THEMIS_ENABLE_LIBC_FLOAT_SCANF}")
  message(STATUS "Themis board bench : ${THEMIS_ENABLE_BOARD_BENCH}")
  message(STATUS "Themis metadata lane : ${_themis_metadata_entry_lane}")

  if(THEMIS_BUILD_FIRMWARE)
    message(STATUS "Themis platform : ${THEMIS_PLATFORM_VENDOR}/${THEMIS_PLATFORM_SOC}")
    message(STATUS "Themis CPU class : ${THEMIS_CPU_CLASS}")
    message(STATUS "Themis linker script : ${THEMIS_LINKER_SCRIPT}")
  endif()
endfunction()
