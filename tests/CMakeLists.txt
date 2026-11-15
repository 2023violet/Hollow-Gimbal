# Themis host-test manifest. Role:
# - declare only the host-side verification targets that still match the
#   current FalconFoc codebase
# - keep each target explicit so ownership and coverage stay readable
# - avoid stale retired UI/resource paths that no longer exist in this product tree

set(_falconfoc_host_platform_generated_inc "${CMAKE_SOURCE_DIR}/platform/at/at32f415/generated/inc")

themis_add_host_test(
  NAME
  test_tm_export
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_tm_export.c"
  "subsys/export/tm_export.c"
  PRIVATE_INCLUDE_DIRS
  "include"
  "include/config"
  "subsys/export"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_tm_format
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_tm_format.c"
  "tests/host/subsys/test_tm_format_impl.c"
  PRIVATE_INCLUDE_DIRS
  "subsys/format"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_tm_format PRIVATE m)
endif()

themis_add_host_test(
  NAME
  test_tm_assert
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_tm_assert.c"
  "tests/host/support/host_rtos.c"
  "subsys/runtime/tm_runtime.c"
  "subsys/format/tm_format.c"
  "subsys/assert/tm_assert.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "include"
  "include/config"
  "subsys/assert"
  "subsys/format"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_tm_assert PRIVATE TM_BUILD_PROFILE_FULL_DIAG=1)

themis_add_host_test(
  NAME
  test_core_time_dwt
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_core_time_dwt.c"
  "tests/host/support/host_rtos.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "include"
  "include/config"
  "subsys/time"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_tm_runtime
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_tm_runtime.c"
  "tests/host/support/host_rtos.c"
  "subsys/runtime/tm_runtime.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "include"
  "include/config"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_bsp_system_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/bsp/test_bsp_system_contract.c"
  "tests/host/support/host_rtos.c"
  "subsys/runtime/tm_runtime.c"
  "bsp/ports/bsp_system.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "include"
  "include/config"
  "bsp"
  "bsp/ports"
  "subsys/runtime"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_bsp_console_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/bsp/test_bsp_console_contract.c"
  "tests/host/support/host_rtos.c"
  "subsys/runtime/tm_runtime.c"
  "bsp/ports/bsp_console.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "include"
  "include/config"
  "bsp"
  "bsp/ports"
  "subsys/runtime"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_tm_task_perf
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_tm_task_perf.c"
  PRIVATE_INCLUDE_DIRS
  "include"
  "include/config"
  "subsys/time"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_tm_task_perf PRIVATE TM_TASK_PERF_ENABLE=1)

themis_add_host_test(
  NAME
  test_tm_log
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_tm_log.c"
  "tests/host/support/host_rtos.c"
  "subsys/runtime/tm_runtime.c"
  "subsys/format/tm_format.c"
  "subsys/log/tm_log.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support/full_diag_config"
  "tests/host/support"
  "include"
  "include/config"
  "subsys/assert"
  "subsys/format"
  "subsys/log"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_tm_log PRIVATE TM_BUILD_PROFILE_FULL_DIAG=1)
target_compile_definitions(test_tm_log PRIVATE TM_FEATURE_VERBOSE_LOG=1)

themis_add_host_test(
  NAME
  test_tm_shell
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_tm_shell.c"
  "tests/host/support/host_rtos.c"
  "subsys/assert/tm_assert.c"
  "subsys/runtime/tm_runtime.c"
  "subsys/format/tm_format.c"
  "subsys/shell/tm_shell.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "include"
  "include/config"
  "subsys/assert"
  "subsys/format"
  "subsys/shell"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_tm_shell_task
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_tm_shell_task.c"
  "tests/host/support/host_rtos.c"
  "subsys/assert/tm_assert.c"
  "subsys/runtime/tm_runtime.c"
  "subsys/runtime/tm_task_budget_probe.c"
  "subsys/format/tm_format.c"
  "subsys/shell/tm_shell.c"
  "applications/tasks/app_shell_task.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "include"
  "include/config"
  "subsys/assert"
  "subsys/export"
  "subsys/format"
  "subsys/shell"
  "applications"
  "applications/tasks"
  "applications/config"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_tm_shell_task PRIVATE TM_SHELL_TASK_QUEUE_DEPTH=2)
target_compile_definitions(test_tm_shell_task PRIVATE TM_FEATURE_TASK_BUDGET=1)

themis_add_host_test(
  NAME
  test_tm_task_budget_probe_failfast
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/subsys/test_tm_task_budget_probe_failfast.c"
  "tests/host/support/host_rtos.c"
  "subsys/runtime/tm_task_budget_probe.c"
  "subsys/runtime/tm_runtime.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "include"
  "include/config"
  WRAP_SYSTEM_FAILURE
  FOLDER
  "themis/tests"
)
set_tests_properties(test_tm_task_budget_probe_failfast PROPERTIES WILL_FAIL TRUE)
target_compile_definitions(test_tm_task_budget_probe_failfast PRIVATE TM_FEATURE_TASK_BUDGET=1)

themis_add_host_test(
  NAME
  test_app_runtime_budget_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_app_runtime_budget_contract.c"
  PRIVATE_INCLUDE_DIRS
  "include"
  "include/config"
  "applications/config"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_foc_round_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_round_contract.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications/algo"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_round_contract PRIVATE m)
endif()

themis_add_host_test(
  NAME
  test_app_shell_common_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_app_shell_common_contract.c"
  "tests/host/support/host_rtos.c"
  "tests/host/support/runtime_host_stubs.c"
  "subsys/runtime/tm_runtime.c"
  "subsys/format/tm_format.c"
  "applications/runtime/motor_runtime.c"
  "applications/algo/traj_trap.c"
  "applications/shell/app_shell_common.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "include"
  "include/config"
  "applications"
  "applications/shell"
  "applications/runtime"
  "applications/config"
  "applications/algo"
  "applications/periph"
  "applications/config"
  "components/utils/fsm"
  "kernel/freertos/CMSIS_RTOS_V2"
  "subsys/format"
  "subsys/runtime"
  "subsys/shell"
  "subsys/assert"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_app_shell_common_contract PRIVATE TM_BUILD_PROFILE_FULL_DIAG=1)

themis_add_host_test(
  NAME
  test_app_shell_burst_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_app_shell_burst_contract.c"
  "tests/host/support/host_rtos.c"
  "tests/host/support/runtime_host_stubs.c"
  "subsys/assert/tm_assert.c"
  "subsys/runtime/tm_runtime.c"
  "subsys/format/tm_format.c"
  "subsys/shell/tm_shell.c"
  "applications/runtime/motor_runtime.c"
  "applications/algo/traj_trap.c"
  "applications/shell/app_shell_common.c"
  "applications/shell/app_shell_output_helpers.c"
  "applications/shell/app_shell_cmd_sys.c"
  "applications/shell/app_shell_cmd_diag.c"
  "applications/shell/app_shell_cmd_store.c"
  "applications/shell/app_shell_cmd_state.c"
  "applications/shell/app_shell_cmd_obs.c"
  "applications/shell/app_shell_cmd_fault.c"
  "applications/shell/app_shell_cmd_cfg.c"
  "applications/shell/app_shell_cmd_cal.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "tests/host/support/rtos"
  "tests/host/support/full_diag_config"
  "include"
  "include/config"
  "applications"
  "applications/shell"
  "applications/runtime"
  "applications/algo"
  "applications/tasks"
  "applications/config"
  "bsp/ports"
  "components/utils/fsm"
  "kernel/freertos/CMSIS_RTOS_V2"
  "subsys/format"
  "subsys/runtime"
  "subsys/shell"
  "subsys/assert"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_app_shell_burst_contract PRIVATE TM_BUILD_PROFILE_FULL_DIAG=1)
target_compile_definitions(
  test_app_shell_burst_contract PRIVATE THEMIS_HOST_RUNTIME_STUB_SKIP_CALIB_DISPATCH=1
)
target_compile_definitions(
  test_app_shell_burst_contract PRIVATE THEMIS_HOST_RUNTIME_STUB_SKIP_VOLTAGE_SET=1
)

themis_add_host_test(
  NAME
  test_motor_fsm_abort_restore
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_fsm_abort_restore.c"
  "applications/tasks/motor_fsm.c"
  "components/utils/fsm/fsm.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_motor_fsm_fault_clear_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_fsm_fault_clear_contract.c"
  "applications/tasks/motor_fsm.c"
  "components/utils/fsm/fsm.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_app_shell_state_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_app_shell_state_contract.c"
  "applications/shell/app_shell_cmd_state.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "tests/host/support/full_diag_config"
  "applications"
  "applications/shell"
  "applications/runtime"
  "applications/algo"
  "applications/periph"
  "applications/config"
  "components/utils/fsm"
  "subsys/shell"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_app_shell_state_contract PRIVATE TM_BUILD_PROFILE_FULL_DIAG=1)

themis_add_host_test(
  NAME
  test_app_shell_store_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_app_shell_store_contract.c"
  "applications/shell/app_shell_cmd_store.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "tests/host/support/full_diag_config"
  "applications"
  "applications/shell"
  "applications/runtime"
  "applications/algo"
  "applications/config"
  "components/utils/fsm"
  "subsys/shell"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_app_shell_store_contract PRIVATE TM_BUILD_PROFILE_FULL_DIAG=1)

themis_add_host_test(
  NAME
  test_app_shell_cfg_cal_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_app_shell_cfg_cal_contract.c"
  "applications/shell/app_shell_cmd_cfg.c"
  "applications/shell/app_shell_cmd_cal.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "tests/host/support/full_diag_config"
  "applications"
  "applications/shell"
  "applications/runtime"
  "applications/algo"
  "applications/config"
  "components/utils/fsm"
  "subsys/shell"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_app_shell_cfg_cal_contract PRIVATE TM_BUILD_PROFILE_FULL_DIAG=1)

themis_add_host_test(
  NAME
  test_app_shell_diag_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_app_shell_diag_contract.c"
  "applications/shell/app_shell_output_helpers.c"
  "applications/shell/app_shell_cmd_diag.c"
  "applications/periph/motor_bench_mapping.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "tests/host/support/full_diag_config"
  "applications"
  "applications/shell"
  "applications/runtime"
  "applications/algo"
  "applications/periph"
  "applications/config"
  "components/utils/fsm"
  "subsys/shell"
  FOLDER
  "themis/tests"
)
target_compile_definitions(
  test_app_shell_diag_contract PRIVATE TM_BUILD_PROFILE_FULL_DIAG=1 TM_BENCH_DIAG=1
)

themis_add_host_test(
  NAME
  test_foc_param_restore_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_param_restore_contract.c"
  "applications/runtime/motor_store.c"
  "applications/runtime/foc_restore.c"
  "applications/algo/traj_trap.c"
  "components/utils/tiny_param/tiny_param.c"
  "components/utils/crc/crc.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support/foc_restore_contract"
  "include"
  "include/config"
  "applications"
  "applications/algo"
  "applications/runtime"
  "bsp"
  "kernel/freertos/include"
  "components/utils/tiny_param"
  "components/utils/crc"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_param_restore_contract PRIVATE m)
endif()

themis_add_host_test(
  NAME
  test_motor_runtime_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_runtime_contract.c"
  "tests/host/support/host_rtos.c"
  "tests/host/support/runtime_host_stubs.c"
  "applications/periph/encoder_bench_trace.c"
  "applications/periph/motor_bench_mapping.c"
  "applications/runtime/motor_bench_watchdog.c"
  "applications/runtime/motor_runtime.c"
  "applications/algo/traj_trap.c"
  "subsys/runtime/tm_runtime.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/runtime"
  "applications/config"
  "applications/algo"
  "applications/periph"
  "components/utils/fsm"
  "include"
  "include/config"
  "kernel/freertos/CMSIS_RTOS_V2"
  "subsys/runtime"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_motor_runtime_contract PRIVATE m)
endif()
target_compile_definitions(test_motor_runtime_contract PRIVATE TM_BENCH_DIAG=1)

themis_add_host_test(
  NAME
  test_motor_fault_runtime_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_fault_runtime_contract.c"
  "applications/runtime/motor_fault_runtime.c"
  "applications/periph/encoder_bench_trace.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/runtime"
  "applications/algo"
  "applications/periph"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_motor_fault_runtime_contract PRIVATE m)
endif()
target_compile_definitions(test_motor_fault_runtime_contract PRIVATE TM_BENCH_DIAG=1)

themis_add_host_test(
  NAME
  test_motor_fault_context_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_fault_context_contract.c"
  "applications/runtime/motor_fault_runtime.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/runtime"
  "applications/algo"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_motor_bench_trace_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_bench_trace_contract.c"
  "applications/runtime/motor_fault_runtime.c"
  "applications/shell/app_shell_output_helpers.c"
  "applications/shell/app_shell_cmd_diag.c"
  "applications/periph/motor_bench_mapping.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "tests/host/support/full_diag_config"
  "include"
  "include/config"
  "applications"
  "applications/shell"
  "applications"
  "applications/runtime"
  "applications/algo"
  "applications/periph"
  "applications/config"
  "components/utils/fsm"
  "subsys/shell"
  FOLDER
  "themis/tests"
)
target_compile_definitions(
  test_motor_bench_trace_contract PRIVATE TM_BUILD_PROFILE_FULL_DIAG=1 TM_BENCH_DIAG=1
)
if(NOT MSVC)
  target_link_libraries(test_motor_bench_trace_contract PRIVATE m)
endif()

themis_add_host_test(
  NAME
  test_motor_bench_mapping_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_bench_mapping_contract.c"
  "applications/periph/motor_bench_mapping.c"
  PRIVATE_INCLUDE_DIRS
  "applications/periph"
  "applications/algo"
  "include"
  "include/config"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_encoder_bench_trace_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_encoder_bench_trace_contract.c"
  "applications/periph/encoder_bench_trace.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/periph"
  "applications/algo"
  "include"
  "include/config"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_encoder_bench_trace_contract PRIVATE TM_BENCH_DIAG=1)

themis_add_host_test(
  NAME
  test_motor_fsm_bench_pwm_boundary_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_fsm_bench_pwm_boundary_contract.c"
  "applications/tasks/motor_fsm.c"
  "components/utils/fsm/fsm.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
target_compile_definitions(
  test_motor_fsm_bench_pwm_boundary_contract PRIVATE TM_BENCH_DIAG=1 FOC_ENERGIZED_CALIB_ENABLE=0
)

themis_add_host_test(
  NAME
  test_motor_task_bench_cadence_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_task_bench_cadence_contract.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/config"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_motor_task_bench_cadence_contract PRIVATE TM_BENCH_DIAG=1)

themis_add_host_test(
  NAME
  test_motor_bench_watchdog_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_bench_watchdog_contract.c"
  "applications/runtime/motor_bench_watchdog.c"
  PRIVATE_INCLUDE_DIRS
  "applications/config"
  "applications/runtime"
  "include"
  "include/config"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_motor_bench_watchdog_contract PRIVATE TM_BENCH_DIAG=1)

if(NOT MSVC)
  target_link_libraries(test_motor_fault_context_contract PRIVATE m)
endif()

themis_add_host_test(
  NAME
  test_foc_calib_curr_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_calib_curr_contract.c"
  "applications/periph/foc_calib.c"
  "components/utils/stat_welford/stat_welford.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/periph"
  "applications/runtime"
  "components/utils/fsm"
  "components/utils/stat_welford"
  FOLDER
  "themis/tests"
)
target_compile_definitions(test_motor_fsm_abort_restore PRIVATE FOC_ENERGIZED_CALIB_ENABLE=1)

themis_add_host_test(
  NAME
  test_motor_fsm_calibration_gate_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_fsm_calibration_gate_contract.c"
  "applications/tasks/motor_fsm.c"
  "components/utils/fsm/fsm.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_calib_curr_contract PRIVATE m)
endif()
target_link_options(test_foc_calib_curr_contract PRIVATE "-Wl,--gc-sections")

themis_add_host_test(
  NAME
  test_foc_vbus_guard_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_vbus_guard_contract.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/periph"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_foc_bringup_model_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_bringup_model_contract.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/periph"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)

themis_add_host_test(
  NAME
  test_foc_voltage_openloop_model_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_voltage_openloop_model_contract.c"
  "applications/periph/foc_ctrl.c"
  "applications/algo/foc_core.c"
  "components/algorithm/pid/pid.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/periph"
  "components/algorithm/pid"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_voltage_openloop_model_contract PRIVATE m)
  target_compile_options(
    test_foc_voltage_openloop_model_contract
    PRIVATE -include "${CMAKE_SOURCE_DIR}/tests/host/support/at32f415_adc_contract.h"
  )
endif()
target_include_directories(
  test_foc_voltage_openloop_model_contract BEFORE
  PRIVATE "${CMAKE_SOURCE_DIR}/components/algorithm/pid" "${CMAKE_SOURCE_DIR}/bsp"
)

themis_add_host_test(
  NAME
  test_foc_bench_fixed_duty_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_voltage_openloop_model_contract.c"
  "applications/periph/foc_ctrl.c"
  "applications/algo/foc_core.c"
  "components/algorithm/pid/pid.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/periph"
  "components/algorithm/pid"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_bench_fixed_duty_contract PRIVATE m)
  target_compile_options(
    test_foc_bench_fixed_duty_contract
    PRIVATE -include "${CMAKE_SOURCE_DIR}/tests/host/support/at32f415_adc_contract.h"
  )
endif()
target_compile_definitions(
  test_foc_bench_fixed_duty_contract PRIVATE TM_BENCH_DIAG=1 TM_BENCH_FIXED_DUTY_TEST=1
)
target_include_directories(
  test_foc_bench_fixed_duty_contract BEFORE PRIVATE "${CMAKE_SOURCE_DIR}/components/algorithm/pid"
                                                    "${CMAKE_SOURCE_DIR}/bsp"
)

themis_add_host_test(
  NAME
  test_foc_bench_raw_sixstep_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_voltage_openloop_model_contract.c"
  "applications/periph/foc_ctrl.c"
  "applications/algo/foc_core.c"
  "components/algorithm/pid/pid.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/periph"
  "components/algorithm/pid"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_bench_raw_sixstep_contract PRIVATE m)
  target_compile_options(
    test_foc_bench_raw_sixstep_contract
    PRIVATE -include "${CMAKE_SOURCE_DIR}/tests/host/support/at32f415_adc_contract.h"
  )
endif()
target_compile_definitions(
  test_foc_bench_raw_sixstep_contract PRIVATE TM_BENCH_DIAG=1 TM_BENCH_RAW_SIXSTEP_TEST=1
)
target_include_directories(
  test_foc_bench_raw_sixstep_contract BEFORE PRIVATE "${CMAKE_SOURCE_DIR}/components/algorithm/pid"
                                                     "${CMAKE_SOURCE_DIR}/bsp"
)

themis_add_host_test(
  NAME
  test_foc_bench_raw_sine_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_voltage_openloop_model_contract.c"
  "applications/periph/foc_ctrl.c"
  "applications/algo/foc_core.c"
  "components/algorithm/pid/pid.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/periph"
  "components/algorithm/pid"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_bench_raw_sine_contract PRIVATE m)
  target_compile_options(
    test_foc_bench_raw_sine_contract
    PRIVATE -include "${CMAKE_SOURCE_DIR}/tests/host/support/at32f415_adc_contract.h"
  )
endif()
target_compile_definitions(
  test_foc_bench_raw_sine_contract PRIVATE TM_BENCH_DIAG=1 TM_BENCH_RAW_SINE_TEST=1
)
target_include_directories(
  test_foc_bench_raw_sine_contract BEFORE PRIVATE "${CMAKE_SOURCE_DIR}/components/algorithm/pid"
                                                  "${CMAKE_SOURCE_DIR}/bsp"
)

themis_add_host_test(
  NAME
  test_foc_bench_raw_sine_match_sixstep_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_voltage_openloop_model_contract.c"
  "applications/periph/foc_ctrl.c"
  "applications/algo/foc_core.c"
  "components/algorithm/pid/pid.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/periph"
  "components/algorithm/pid"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_bench_raw_sine_match_sixstep_contract PRIVATE m)
  target_compile_options(
    test_foc_bench_raw_sine_match_sixstep_contract
    PRIVATE -include "${CMAKE_SOURCE_DIR}/tests/host/support/at32f415_adc_contract.h"
  )
endif()
target_compile_definitions(
  test_foc_bench_raw_sine_match_sixstep_contract PRIVATE TM_BENCH_DIAG=1
                                                         TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST=1
)
target_include_directories(
  test_foc_bench_raw_sine_match_sixstep_contract BEFORE
  PRIVATE "${CMAKE_SOURCE_DIR}/components/algorithm/pid" "${CMAKE_SOURCE_DIR}/bsp"
)

themis_add_host_test(
  NAME
  test_foc_bench_raw_sine_20_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_voltage_openloop_model_contract.c"
  "applications/periph/foc_ctrl.c"
  "applications/algo/foc_core.c"
  "components/algorithm/pid/pid.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/algo"
  "applications/periph"
  "components/algorithm/pid"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_bench_raw_sine_20_contract PRIVATE m)
  target_compile_options(
    test_foc_bench_raw_sine_20_contract
    PRIVATE -include "${CMAKE_SOURCE_DIR}/tests/host/support/at32f415_adc_contract.h"
  )
endif()
target_compile_definitions(
  test_foc_bench_raw_sine_20_contract PRIVATE TM_BENCH_DIAG=1 TM_BENCH_RAW_SINE_20_TEST=1
)
target_include_directories(
  test_foc_bench_raw_sine_20_contract BEFORE PRIVATE "${CMAKE_SOURCE_DIR}/components/algorithm/pid"
                                                     "${CMAKE_SOURCE_DIR}/bsp"
)

themis_add_host_test(
  NAME
  test_if_to_vel_handoff_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_if_to_vel_handoff_contract.c"
  "applications/runtime/foc_motion_runtime.c"
  "applications/algo/traj_trap.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/runtime"
  "applications/algo"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_if_to_vel_handoff_contract PRIVATE m)
endif()
target_compile_definitions(test_if_to_vel_handoff_contract PRIVATE TM_BENCH_DIAG=1)

themis_add_host_test(
  NAME
  test_foc_motion_runtime_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_foc_motion_runtime_contract.c"
  "applications/runtime/foc_motion_runtime.c"
  "applications/algo/traj_trap.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/runtime"
  "applications/algo"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_foc_motion_runtime_contract PRIVATE m)
endif()
target_compile_definitions(test_foc_motion_runtime_contract PRIVATE TM_BENCH_DIAG=1)

themis_add_host_test(
  NAME
  test_motor_calibration_runtime_contract
  BASE_DIR
  ${CMAKE_SOURCE_DIR}
  SOURCES
  "tests/host/applications/test_motor_calibration_runtime_contract.c"
  "tests/host/support/runtime_host_stubs.c"
  "applications/runtime/motor_calibration_runtime.c"
  PRIVATE_INCLUDE_DIRS
  "tests/host/support"
  "applications"
  "applications/runtime"
  "applications/algo"
  "applications/periph"
  "components/utils/fsm"
  FOLDER
  "themis/tests"
)
if(NOT MSVC)
  target_link_libraries(test_motor_calibration_runtime_contract PRIVATE m)
endif()
target_compile_definitions(
  test_motor_calibration_runtime_contract PRIVATE THEMIS_HOST_RUNTIME_STUB_SKIP_GAIN_HELPERS=1
)

foreach(
  _falconfoc_app_host_target IN
  ITEMS test_foc_round_contract
        test_app_shell_common_contract
        test_app_shell_burst_contract
        test_motor_fsm_abort_restore
        test_motor_fsm_calibration_gate_contract
        test_motor_fsm_fault_clear_contract
        test_app_shell_state_contract
        test_app_shell_store_contract
        test_app_shell_cfg_cal_contract
        test_app_shell_diag_contract
        test_foc_param_restore_contract
        test_motor_runtime_contract
        test_motor_fault_runtime_contract
        test_motor_fault_context_contract
        test_foc_calib_curr_contract
        test_foc_vbus_guard_contract
        test_foc_bringup_model_contract
        test_foc_voltage_openloop_model_contract
        test_foc_motion_runtime_contract
        test_if_to_vel_handoff_contract
        test_motor_calibration_runtime_contract
)
  target_include_directories(
    ${_falconfoc_app_host_target} PRIVATE "${_falconfoc_host_platform_generated_inc}"
  )
endforeach()
