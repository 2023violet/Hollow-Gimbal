# Themis firmware assembly helper. Role: - load platform and CPU-class metadata
# for firmware lanes - derive resolved startup/entry/linker/platform contracts -
# validate the final firmware path set before layer manifests consume it

include_guard(GLOBAL)

function(themis_firmware_assemble repo_root)
  themis_metadata_load_platform_defaults("${repo_root}")
  themis_metadata_load_cpu_defaults("${repo_root}")

  themis_metadata_resolve_repo_path(
    _themis_entry_source_override "${THEMIS_ENTRY_SOURCE_OVERRIDE}" "${repo_root}"
  )
  themis_metadata_resolve_repo_path(
    _themis_startup_source_override "${THEMIS_STARTUP_SOURCE_OVERRIDE}" "${repo_root}"
  )
  themis_metadata_resolve_repo_path(
    _themis_linker_script_override "${THEMIS_LINKER_SCRIPT_OVERRIDE}" "${repo_root}"
  )

  themis_metadata_resolve_path_with_base(
    _themis_entry_source_default "${THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT}" "${THEMIS_PLATFORM_DIR}"
  )
  themis_metadata_resolve_path_with_base(
    _themis_startup_source_default "${THEMIS_PLATFORM_STARTUP_SOURCE_DEFAULT}"
    "${THEMIS_PLATFORM_DIR}"
  )
  themis_metadata_resolve_path_with_base(
    _themis_linker_script_default "${THEMIS_PLATFORM_LINKER_SCRIPT_DEFAULT}"
    "${THEMIS_PLATFORM_DIR}"
  )

  set(_themis_cpu_toolchain_cpu "${THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT}")
  set(_themis_cpu_toolchain_fpu "${THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT}")
  set(_themis_cpu_toolchain_float "${THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT}")
  set(_themis_cpu_freertos_port_glob "${THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT}")
  set(_themis_cpu_freertos_port_include_dir "${THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT}")
  set(_themis_cpu_arm_math_define "${THEMIS_CPU_ARM_MATH_DEFINE_DEFAULT}")

  get_filename_component(
    _themis_freertos_port_glob_dir "${_themis_cpu_freertos_port_glob}" DIRECTORY
  )
  themis_metadata_resolve_path_with_base(
    _themis_freertos_port_glob_dir_abs "${_themis_freertos_port_glob_dir}"
    "${repo_root}/kernel/freertos"
  )
  themis_metadata_resolve_path_with_base(
    _themis_freertos_port_include_dir_abs "${_themis_cpu_freertos_port_include_dir}"
    "${repo_root}/kernel/freertos"
  )

  if(THEMIS_PLATFORM_TARGET_OVERRIDE)
    set(_themis_platform_target "${THEMIS_PLATFORM_TARGET_OVERRIDE}")
  else()
    set(_themis_platform_target "${THEMIS_PLATFORM_TARGET_DEFAULT}")
  endif()

  if(_themis_entry_source_override)
    set(_themis_entry_source "${_themis_entry_source_override}")
  else()
    set(_themis_entry_source "${_themis_entry_source_default}")
  endif()

  if(_themis_startup_source_override)
    set(_themis_startup_source "${_themis_startup_source_override}")
  else()
    set(_themis_startup_source "${_themis_startup_source_default}")
  endif()

  if(_themis_linker_script_override)
    set(_themis_linker_script "${_themis_linker_script_override}")
  else()
    set(_themis_linker_script "${_themis_linker_script_default}")
  endif()

  if(THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE)
    set(_themis_platform_compile_definitions "${THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE}")
  else()
    set(_themis_platform_compile_definitions "${THEMIS_PLATFORM_COMPILE_DEFINITIONS_DEFAULT}")
  endif()

  foreach(
    _required_path IN
    ITEMS "${THEMIS_PLATFORM_DIR}"
          "${_themis_entry_source}"
          "${_themis_startup_source}"
          "${_themis_linker_script}"
          "${_themis_freertos_port_glob_dir_abs}"
          "${_themis_freertos_port_include_dir_abs}"
  )
    if(NOT EXISTS "${_required_path}")
      message(FATAL_ERROR "Themis firmware path does not exist: ${_required_path}")
    endif()
  endforeach()

  foreach(
    _export_var IN
    ITEMS THEMIS_PLATFORM_DIR
          THEMIS_PLATFORM_DEFAULTS_FILE
          THEMIS_PLATFORM_TARGET_DEFAULT
          THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT
          THEMIS_PLATFORM_STARTUP_SOURCE_DEFAULT
          THEMIS_PLATFORM_LINKER_SCRIPT_DEFAULT
          THEMIS_PLATFORM_COMPILE_DEFINITIONS_DEFAULT
          THEMIS_PLATFORM_CPU_CLASS_DEFAULT
          THEMIS_CPU_CLASS
          THEMIS_CPU_DEFAULTS_FILE
  )
    set(${_export_var}
        "${${_export_var}}"
        PARENT_SCOPE
    )
  endforeach()

  set(THEMIS_CPU_TOOLCHAIN_CPU
      "${_themis_cpu_toolchain_cpu}"
      PARENT_SCOPE
  )
  set(THEMIS_CPU_TOOLCHAIN_FPU
      "${_themis_cpu_toolchain_fpu}"
      PARENT_SCOPE
  )
  set(THEMIS_CPU_TOOLCHAIN_FLOAT
      "${_themis_cpu_toolchain_float}"
      PARENT_SCOPE
  )
  set(THEMIS_CPU_FREERTOS_PORT_GLOB
      "${_themis_cpu_freertos_port_glob}"
      PARENT_SCOPE
  )
  set(THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR
      "${_themis_cpu_freertos_port_include_dir}"
      PARENT_SCOPE
  )
  set(THEMIS_CPU_ARM_MATH_DEFINE
      "${_themis_cpu_arm_math_define}"
      PARENT_SCOPE
  )
  set(THEMIS_PLATFORM_TARGET
      "${_themis_platform_target}"
      PARENT_SCOPE
  )
  set(THEMIS_ENTRY_SOURCE
      "${_themis_entry_source}"
      PARENT_SCOPE
  )
  set(THEMIS_STARTUP_SOURCE
      "${_themis_startup_source}"
      PARENT_SCOPE
  )
  set(THEMIS_LINKER_SCRIPT
      "${_themis_linker_script}"
      PARENT_SCOPE
  )
  set(THEMIS_PLATFORM_COMPILE_DEFINITIONS
      "${_themis_platform_compile_definitions}"
      PARENT_SCOPE
  )
endfunction()
