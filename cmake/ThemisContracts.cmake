# Themis contract-target helper. Role: - create root/build/platform/CPU/host
# contract targets - keep shared compile/link policy out of the root assembly
# narrative - make leaf manifests consume stable contract owners only

include_guard(GLOBAL)

function(themis_define_contract_targets repo_root)
  add_library(themis_root_headers INTERFACE)
  target_include_directories(
    themis_root_headers INTERFACE "${repo_root}/include" "${repo_root}/include/config"
  )
  set_target_properties(themis_root_headers PROPERTIES FOLDER "themis/contracts")

  add_library(themis_cpu_class_contract INTERFACE)
  target_link_libraries(themis_cpu_class_contract INTERFACE themis_root_headers)
  if(THEMIS_BUILD_FIRMWARE AND THEMIS_CPU_ARM_MATH_DEFINE)
    target_compile_definitions(themis_cpu_class_contract INTERFACE ${THEMIS_CPU_ARM_MATH_DEFINE})
  endif()
  set_target_properties(themis_cpu_class_contract PROPERTIES FOLDER "themis/contracts")

  add_library(themis_build_contract INTERFACE)
  target_compile_definitions(
    themis_build_contract
    INTERFACE $<$<CONFIG:Debug>:DEBUG>
              $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:NDEBUG>
              ${THEMIS_DEVDRV_PROFILE_COMPILE_DEFINITION}
              ${THEMIS_PRODUCT_FLAVOR_COMPILE_DEFINITION}
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
  target_link_libraries(
    themis_platform_contract INTERFACE themis_root_headers themis_cpu_class_contract
  )
  if(THEMIS_PLATFORM_COMPILE_DEFINITIONS)
    target_compile_definitions(
      themis_platform_contract INTERFACE ${THEMIS_PLATFORM_COMPILE_DEFINITIONS}
    )
  endif()
  set_target_properties(themis_platform_contract PROPERTIES FOLDER "themis/contracts")

  add_library(themis_host_test_contract INTERFACE)
  target_link_libraries(
    themis_host_test_contract INTERFACE themis_root_headers themis_build_contract
  )
  target_include_directories(
    themis_host_test_contract
    INTERFACE "${repo_root}/tests/host/support/rtos"
              "${repo_root}/subsys/assert"
              "${repo_root}/subsys/runtime"
              "${repo_root}/subsys/format"
              "${repo_root}/kernel/freertos/include"
              "${repo_root}/kernel/freertos/CMSIS_RTOS_V2"
  )
  target_compile_definitions(themis_host_test_contract INTERFACE THEMIS_HOST_TEST=1)
  set_target_properties(themis_host_test_contract PROPERTIES FOLDER "themis/contracts")
endfunction()
