# Themis shared platform/CPU-class metadata helper. Role: - centralize platform selection cache,
# metadata loading, and override precedence shared by the root build entry and the ARM GCC
# toolchain - keep metadata owner resolution consistent across configure lanes

include_guard(GLOBAL)

function(themis_metadata_resolve_path_with_base out_var raw_path base_dir)
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
      "${base_dir}"
    )
  endif()

  set(${out_var}
      "${_resolved_path}"
      PARENT_SCOPE
  )
endfunction()

function(themis_metadata_resolve_repo_path out_var raw_path repo_root)
  themis_metadata_resolve_path_with_base("${out_var}" "${raw_path}" "${repo_root}")
endfunction()

function(themis_metadata_require_default var_name source_file)
  if((NOT DEFINED ${var_name}) OR ("${${var_name}}" STREQUAL ""))
    message(FATAL_ERROR "Themis metadata '${var_name}' must be defined in ${source_file}")
  endif()
endfunction()

function(themis_metadata_declare_platform_selection_cache)
  set(THEMIS_PLATFORM_VENDOR
      "at"
      CACHE STRING "Themis platform vendor directory name."
  )
  set(THEMIS_PLATFORM_SOC
      "at32f415"
      CACHE STRING "Themis platform SoC directory name."
  )
  set(THEMIS_PLATFORM_DIR_OVERRIDE
      ""
      CACHE PATH "Optional absolute or source-relative platform directory override."
  )
  set(THEMIS_CPU_CLASS_OVERRIDE
      ""
      CACHE STRING "Optional CPU-class metadata override."
  )
endfunction()

function(themis_metadata_declare_platform_override_cache)
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
  set(THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE
      ""
      CACHE STRING "Optional platform compile-definition override list."
  )
endfunction()

function(themis_metadata_load_platform_defaults repo_root)
  set(_themis_platform_dir_default
      "${repo_root}/platform/${THEMIS_PLATFORM_VENDOR}/${THEMIS_PLATFORM_SOC}"
  )
  themis_metadata_resolve_repo_path(
    _themis_platform_dir_override "${THEMIS_PLATFORM_DIR_OVERRIDE}" "${repo_root}"
  )

  if(_themis_platform_dir_override)
    set(_themis_platform_dir "${_themis_platform_dir_override}")
  else()
    set(_themis_platform_dir "${_themis_platform_dir_default}")
  endif()

  set(_themis_platform_defaults_file "${_themis_platform_dir}/ThemisPlatformDefaults.cmake")
  if(NOT EXISTS "${_themis_platform_defaults_file}")
    message(
      FATAL_ERROR "Themis firmware platform metadata is missing: ${_themis_platform_defaults_file}"
    )
  endif()

  include("${_themis_platform_defaults_file}")

  foreach(
    _required_platform_default_var IN
    ITEMS THEMIS_PLATFORM_TARGET_DEFAULT
          THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT
          THEMIS_PLATFORM_STARTUP_SOURCE_DEFAULT
          THEMIS_PLATFORM_LINKER_SCRIPT_DEFAULT
          THEMIS_PLATFORM_COMPILE_DEFINITIONS_DEFAULT
          THEMIS_PLATFORM_CPU_CLASS_DEFAULT
  )
    themis_metadata_require_default(
      ${_required_platform_default_var} "${_themis_platform_defaults_file}"
    )
  endforeach()

  set(THEMIS_PLATFORM_DIR
      "${_themis_platform_dir}"
      PARENT_SCOPE
  )
  set(THEMIS_PLATFORM_DEFAULTS_FILE
      "${_themis_platform_defaults_file}"
      PARENT_SCOPE
  )

  foreach(
    _platform_default_var IN
    ITEMS THEMIS_PLATFORM_TARGET_DEFAULT
          THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT
          THEMIS_PLATFORM_STARTUP_SOURCE_DEFAULT
          THEMIS_PLATFORM_LINKER_SCRIPT_DEFAULT
          THEMIS_PLATFORM_COMPILE_DEFINITIONS_DEFAULT
          THEMIS_PLATFORM_CPU_CLASS_DEFAULT
  )
    set(${_platform_default_var}
        "${${_platform_default_var}}"
        PARENT_SCOPE
    )
  endforeach()
endfunction()

function(themis_metadata_load_cpu_defaults repo_root)
  if(THEMIS_CPU_CLASS_OVERRIDE)
    set(_themis_cpu_class "${THEMIS_CPU_CLASS_OVERRIDE}")
  else()
    set(_themis_cpu_class "${THEMIS_PLATFORM_CPU_CLASS_DEFAULT}")
  endif()

  set(_themis_cpu_defaults_file
      "${repo_root}/platform/cpu/${_themis_cpu_class}/ThemisCpuClassDefaults.cmake"
  )
  if(NOT EXISTS "${_themis_cpu_defaults_file}")
    message(FATAL_ERROR "Themis CPU-class metadata is missing: ${_themis_cpu_defaults_file}")
  endif()

  include("${_themis_cpu_defaults_file}")

  foreach(
    _required_cpu_default_var IN
    ITEMS THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT
          THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT
          THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT
          THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT
          THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT
          THEMIS_CPU_ARM_MATH_DEFINE_DEFAULT
  )
    themis_metadata_require_default(${_required_cpu_default_var} "${_themis_cpu_defaults_file}")
  endforeach()

  set(THEMIS_CPU_CLASS
      "${_themis_cpu_class}"
      PARENT_SCOPE
  )
  set(THEMIS_CPU_DEFAULTS_FILE
      "${_themis_cpu_defaults_file}"
      PARENT_SCOPE
  )

  foreach(
    _cpu_default_var IN
    ITEMS THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT
          THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT
          THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT
          THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT
          THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT
          THEMIS_CPU_ARM_MATH_DEFINE_DEFAULT
  )
    set(${_cpu_default_var}
        "${${_cpu_default_var}}"
        PARENT_SCOPE
    )
  endforeach()
endfunction()
