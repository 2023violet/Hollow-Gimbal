# Themis reusable CMake helper layer. Role: - normalize path and source collection - provide
# declarative module, host-test, and firmware helper entry points - keep leaf CMakeLists focused on
# ownership facts instead of boilerplate

include_guard(GLOBAL)

function(themis_resolve_paths out_var)
  cmake_parse_arguments(
    ARG
    ""
    "BASE_DIR"
    "PATHS"
    ${ARGN}
  )

  if(NOT ARG_BASE_DIR)
    set(ARG_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
  endif()

  set(_resolved_paths)
  foreach(_path IN LISTS ARG_PATHS)
    if(IS_ABSOLUTE "${_path}")
      set(_candidate "${_path}")
    else()
      set(_candidate "${ARG_BASE_DIR}/${_path}")
    endif()

    if(NOT EXISTS "${_candidate}")
      message(FATAL_ERROR "Themis build path does not exist: ${_candidate}")
    endif()

    list(APPEND _resolved_paths "${_candidate}")
  endforeach()

  list(REMOVE_DUPLICATES _resolved_paths)
  set(${out_var}
      "${_resolved_paths}"
      PARENT_SCOPE
  )
endfunction()

function(themis_collect_sources out_var)
  cmake_parse_arguments(
    ARG
    "ALLOW_EMPTY"
    "BASE_DIR"
    "GLOBS;EXCLUDE"
    ${ARGN}
  )

  if(NOT ARG_BASE_DIR)
    set(ARG_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
  endif()

  set(_sources)
  foreach(_glob IN LISTS ARG_GLOBS)
    if(IS_ABSOLUTE "${_glob}")
      set(_pattern "${_glob}")
    else()
      set(_pattern "${ARG_BASE_DIR}/${_glob}")
    endif()

    file(GLOB _matched CONFIGURE_DEPENDS "${_pattern}")
    list(APPEND _sources ${_matched})
  endforeach()

  foreach(_exclude IN LISTS ARG_EXCLUDE)
    if(IS_ABSOLUTE "${_exclude}")
      set(_exclude_path "${_exclude}")
    else()
      set(_exclude_path "${ARG_BASE_DIR}/${_exclude}")
    endif()

    list(REMOVE_ITEM _sources "${_exclude_path}")
  endforeach()

  list(REMOVE_DUPLICATES _sources)
  list(SORT _sources)

  if((NOT _sources) AND (NOT ARG_ALLOW_EMPTY))
    message(FATAL_ERROR "Themis source collection returned an empty list under '${ARG_BASE_DIR}' "
                        "for globs: ${ARG_GLOBS}"
    )
  endif()

  set(${out_var}
      "${_sources}"
      PARENT_SCOPE
  )
endfunction()

function(themis_collect_subdirectories out_var)
  cmake_parse_arguments(
    ARG
    ""
    "BASE_DIR"
    "EXCLUDE"
    ${ARGN}
  )

  if(NOT ARG_BASE_DIR)
    set(ARG_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
  endif()

  file(
    GLOB _entries
    LIST_DIRECTORIES true
    CONFIGURE_DEPENDS "${ARG_BASE_DIR}/*"
  )

  set(_subdirs)
  foreach(_entry IN LISTS _entries)
    if(IS_DIRECTORY "${_entry}")
      get_filename_component(_dir_name "${_entry}" NAME)
      list(FIND ARG_EXCLUDE "${_dir_name}" _exclude_index)
      if(_exclude_index EQUAL -1)
        list(APPEND _subdirs "${_entry}")
      endif()
    endif()
  endforeach()

  list(SORT _subdirs)
  set(${out_var}
      "${_subdirs}"
      PARENT_SCOPE
  )
endfunction()

function(themis_add_interface_module)
  cmake_parse_arguments(
    ARG
    ""
    "NAME;BASE_DIR;FOLDER"
    "INTERFACE_INCLUDE_DIRS;INTERFACE_DEPS;INTERFACE_COMPILE_DEFINITIONS;INTERFACE_COMPILE_OPTIONS"
    ${ARGN}
  )

  if(NOT ARG_NAME)
    message(FATAL_ERROR "themis_add_interface_module() requires NAME")
  endif()

  if(NOT ARG_BASE_DIR)
    set(ARG_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
  endif()

  add_library(${ARG_NAME} INTERFACE)

  if(NOT TARGET themis::${ARG_NAME})
    add_library(themis::${ARG_NAME} ALIAS ${ARG_NAME})
  endif()

  if(TARGET themis_root_headers)
    target_link_libraries(${ARG_NAME} INTERFACE themis_root_headers)
  endif()

  if(ARG_INTERFACE_INCLUDE_DIRS)
    themis_resolve_paths(
      _interface_include_dirs
      BASE_DIR
      "${ARG_BASE_DIR}"
      PATHS
      ${ARG_INTERFACE_INCLUDE_DIRS}
    )
    target_include_directories(${ARG_NAME} INTERFACE ${_interface_include_dirs})
  endif()

  if(ARG_INTERFACE_DEPS)
    target_link_libraries(${ARG_NAME} INTERFACE ${ARG_INTERFACE_DEPS})
  endif()

  if(ARG_INTERFACE_COMPILE_DEFINITIONS)
    target_compile_definitions(${ARG_NAME} INTERFACE ${ARG_INTERFACE_COMPILE_DEFINITIONS})
  endif()

  if(ARG_INTERFACE_COMPILE_OPTIONS)
    target_compile_options(${ARG_NAME} INTERFACE ${ARG_INTERFACE_COMPILE_OPTIONS})
  endif()

  if(ARG_FOLDER)
    set_target_properties(${ARG_NAME} PROPERTIES FOLDER "${ARG_FOLDER}")
  endif()
endfunction()

function(themis_add_module)
  cmake_parse_arguments(
    ARG
    "EXCLUDE_FROM_ALL"
    "NAME;BASE_DIR;FOLDER;CONTRACT"
    "SOURCES;GLOBS;EXCLUDE;PUBLIC_INCLUDE_DIRS;PRIVATE_INCLUDE_DIRS;PUBLIC_DEPS;PRIVATE_DEPS;PUBLIC_COMPILE_DEFINITIONS;PRIVATE_COMPILE_DEFINITIONS;PUBLIC_COMPILE_OPTIONS;PRIVATE_COMPILE_OPTIONS"
    ${ARGN}
  )

  if(NOT ARG_NAME)
    message(FATAL_ERROR "themis_add_module() requires NAME")
  endif()

  if(NOT ARG_BASE_DIR)
    set(ARG_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
  endif()

  set(_sources)
  if(ARG_SOURCES)
    themis_resolve_paths(
      _explicit_sources
      BASE_DIR
      "${ARG_BASE_DIR}"
      PATHS
      ${ARG_SOURCES}
    )
    list(APPEND _sources ${_explicit_sources})
  endif()

  if(ARG_GLOBS)
    themis_collect_sources(
      _glob_sources
      BASE_DIR
      "${ARG_BASE_DIR}"
      GLOBS
      ${ARG_GLOBS}
      EXCLUDE
      ${ARG_EXCLUDE}
    )
    list(APPEND _sources ${_glob_sources})
  endif()

  list(REMOVE_DUPLICATES _sources)
  list(SORT _sources)

  if(NOT _sources)
    message(FATAL_ERROR "Themis module '${ARG_NAME}' has no sources.")
  endif()

  add_library(${ARG_NAME} STATIC ${_sources})

  if(ARG_EXCLUDE_FROM_ALL)
    set_target_properties(${ARG_NAME} PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()

  if(NOT TARGET themis::${ARG_NAME})
    add_library(themis::${ARG_NAME} ALIAS ${ARG_NAME})
  endif()

  if(TARGET themis_root_headers)
    target_link_libraries(${ARG_NAME} PUBLIC themis_root_headers)
  endif()

  if(ARG_CONTRACT)
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_CONTRACT})
  elseif(TARGET themis_build_contract)
    target_link_libraries(${ARG_NAME} PRIVATE themis_build_contract)
  endif()

  if(ARG_PUBLIC_INCLUDE_DIRS)
    themis_resolve_paths(
      _public_include_dirs
      BASE_DIR
      "${ARG_BASE_DIR}"
      PATHS
      ${ARG_PUBLIC_INCLUDE_DIRS}
    )
    target_include_directories(${ARG_NAME} PUBLIC ${_public_include_dirs})
  endif()

  if(ARG_PRIVATE_INCLUDE_DIRS)
    themis_resolve_paths(
      _private_include_dirs
      BASE_DIR
      "${ARG_BASE_DIR}"
      PATHS
      ${ARG_PRIVATE_INCLUDE_DIRS}
    )
    target_include_directories(${ARG_NAME} PRIVATE ${_private_include_dirs})
  endif()

  if(ARG_PUBLIC_DEPS)
    target_link_libraries(${ARG_NAME} PUBLIC ${ARG_PUBLIC_DEPS})
  endif()

  if(ARG_PRIVATE_DEPS)
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_PRIVATE_DEPS})
  endif()

  if(ARG_PUBLIC_COMPILE_DEFINITIONS)
    target_compile_definitions(${ARG_NAME} PUBLIC ${ARG_PUBLIC_COMPILE_DEFINITIONS})
  endif()

  if(ARG_PRIVATE_COMPILE_DEFINITIONS)
    target_compile_definitions(${ARG_NAME} PRIVATE ${ARG_PRIVATE_COMPILE_DEFINITIONS})
  endif()

  if(ARG_PUBLIC_COMPILE_OPTIONS)
    target_compile_options(${ARG_NAME} PUBLIC ${ARG_PUBLIC_COMPILE_OPTIONS})
  endif()

  if(ARG_PRIVATE_COMPILE_OPTIONS)
    target_compile_options(${ARG_NAME} PRIVATE ${ARG_PRIVATE_COMPILE_OPTIONS})
  endif()

  if(ARG_FOLDER)
    set_target_properties(${ARG_NAME} PROPERTIES FOLDER "${ARG_FOLDER}")
  endif()

  source_group(TREE "${ARG_BASE_DIR}" FILES ${_sources})
endfunction()

function(themis_add_host_test)
  cmake_parse_arguments(
    ARG
    "WRAP_SYSTEM_FAILURE"
    "NAME;BASE_DIR;FOLDER"
    "SOURCES;GLOBS;PRIVATE_DEPS;PRIVATE_INCLUDE_DIRS"
    ${ARGN}
  )

  if(NOT ARG_NAME)
    message(FATAL_ERROR "themis_add_host_test() requires NAME")
  endif()

  if(NOT ARG_BASE_DIR)
    set(ARG_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
  endif()

  set(_sources)
  if(ARG_SOURCES)
    themis_resolve_paths(
      _explicit_sources
      BASE_DIR
      "${ARG_BASE_DIR}"
      PATHS
      ${ARG_SOURCES}
    )
    list(APPEND _sources ${_explicit_sources})
  endif()

  if(ARG_GLOBS)
    themis_collect_sources(
      _glob_sources
      BASE_DIR
      "${ARG_BASE_DIR}"
      GLOBS
      ${ARG_GLOBS}
    )
    list(APPEND _sources ${_glob_sources})
  endif()

  list(REMOVE_DUPLICATES _sources)
  list(SORT _sources)

  if(NOT _sources)
    message(FATAL_ERROR "Themis host test '${ARG_NAME}' has no sources.")
  endif()

  add_executable(${ARG_NAME} ${_sources})

  if(TARGET themis_host_test_contract)
    target_link_libraries(${ARG_NAME} PRIVATE themis_host_test_contract)
  endif()

  if(ARG_PRIVATE_DEPS)
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_PRIVATE_DEPS})
  endif()

  if(ARG_PRIVATE_INCLUDE_DIRS)
    themis_resolve_paths(
      _private_include_dirs
      BASE_DIR
      "${ARG_BASE_DIR}"
      PATHS
      ${ARG_PRIVATE_INCLUDE_DIRS}
    )
    target_include_directories(${ARG_NAME} PRIVATE ${_private_include_dirs})
  endif()

  set_target_properties(
    ${ARG_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/host"
  )

  if(ARG_FOLDER)
    set_target_properties(${ARG_NAME} PROPERTIES FOLDER "${ARG_FOLDER}")
  endif()

  source_group(TREE "${ARG_BASE_DIR}" FILES ${_sources})
  if(ARG_WRAP_SYSTEM_FAILURE)
    # Keep WILL_FAIL usable for tests that intentionally terminate through a
    # platform assertion/abort. Running the executable directly can make
    # CTest classify the child termination as a system-level failure before
    # WILL_FAIL inverts the exit status.
    add_test(NAME ${ARG_NAME} COMMAND ${CMAKE_COMMAND} -E env $<TARGET_FILE:${ARG_NAME}>)
  else()
    add_test(NAME ${ARG_NAME} COMMAND ${ARG_NAME})
  endif()
endfunction()

function(themis_add_firmware)
  cmake_parse_arguments(
    ARG
    ""
    "NAME;ENTRY_SOURCE;STARTUP_SOURCE;LINKER_SCRIPT;OUTPUT_DIR;FOLDER"
    "PRIVATE_DEPS;WHOLE_ARCHIVE_DEPS;SYSTEM_LIBS"
    ${ARGN}
  )

  if(NOT ARG_NAME)
    message(FATAL_ERROR "themis_add_firmware() requires NAME")
  endif()
  if(NOT ARG_ENTRY_SOURCE)
    message(FATAL_ERROR "themis_add_firmware() requires ENTRY_SOURCE")
  endif()
  if(NOT ARG_STARTUP_SOURCE)
    message(FATAL_ERROR "themis_add_firmware() requires STARTUP_SOURCE")
  endif()
  if(NOT ARG_LINKER_SCRIPT)
    message(FATAL_ERROR "themis_add_firmware() requires LINKER_SCRIPT")
  endif()

  themis_resolve_paths(
    _firmware_sources
    BASE_DIR
    "${CMAKE_SOURCE_DIR}"
    PATHS
    "${ARG_ENTRY_SOURCE}"
    "${ARG_STARTUP_SOURCE}"
  )
  themis_resolve_paths(
    _linker_script
    BASE_DIR
    "${CMAKE_SOURCE_DIR}"
    PATHS
    "${ARG_LINKER_SCRIPT}"
  )
  list(GET _linker_script 0 _resolved_linker_script)

  if(ARG_OUTPUT_DIR)
    set(_output_dir "${ARG_OUTPUT_DIR}")
  else()
    set(_output_dir "${CMAKE_BINARY_DIR}/output")
  endif()
  file(MAKE_DIRECTORY "${_output_dir}")

  add_executable(${ARG_NAME} ${_firmware_sources})

  if(TARGET themis_build_contract)
    target_link_libraries(${ARG_NAME} PRIVATE themis_build_contract)
  endif()

  if(ARG_PRIVATE_DEPS)
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_PRIVATE_DEPS})
  endif()

  if(ARG_WHOLE_ARCHIVE_DEPS)
    target_link_libraries(
      ${ARG_NAME} PRIVATE -Wl,--whole-archive ${ARG_WHOLE_ARCHIVE_DEPS} -Wl,--no-whole-archive
    )
  endif()

  if(ARG_SYSTEM_LIBS)
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_SYSTEM_LIBS})
  endif()

  set_target_properties(
    ${ARG_NAME}
    PROPERTIES SUFFIX ".elf"
               RUNTIME_OUTPUT_DIRECTORY "${_output_dir}"
               LINK_DEPENDS "${_resolved_linker_script}"
  )

  target_link_options(
    ${ARG_NAME}
    PRIVATE
    "-T${_resolved_linker_script}"
    "-Wl,-Map=${_output_dir}/${ARG_NAME}.map"
    "-Wl,--cref"
    "-Wl,--gc-sections"
    "-Wl,--print-memory-usage"
    "-Wl,--no-warn-rwx-segments"
  )

  if(THEMIS_ENABLE_LIBC_FLOAT_PRINTF)
    target_link_options(${ARG_NAME} PRIVATE "-Wl,--undefined=_printf_float")
  endif()

  if(THEMIS_ENABLE_LIBC_FLOAT_SCANF)
    target_link_options(${ARG_NAME} PRIVATE "-Wl,--undefined=_scanf_float")
  endif()

  add_custom_command(
    TARGET ${ARG_NAME}
    POST_BUILD
    COMMENT "Generating firmware artifacts for ${ARG_NAME}"
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${ARG_NAME}> "${_output_dir}/${ARG_NAME}.hex"
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${ARG_NAME}> "${_output_dir}/${ARG_NAME}.bin"
    COMMAND ${CMAKE_SIZE} --format=sysv --radix=16 --total $<TARGET_FILE:${ARG_NAME}>
    VERBATIM
  )

  if(ARG_FOLDER)
    set_target_properties(${ARG_NAME} PROPERTIES FOLDER "${ARG_FOLDER}")
  endif()

  source_group(TREE "${CMAKE_SOURCE_DIR}" FILES ${_firmware_sources})
endfunction()
