# Themis framework subsystem manifests. Role: - split framework core contracts from
# board-facing shared mechanisms - keep subsystem source ownership explicit while
# breaking BSP/framework cycles - expose one optional umbrella target for upper
# layers without hiding the real contracts

set(_subsys_core_sources
    "assert/tm_assert.c"
    "daemon/tm_daemon.c"
    "export/tm_export.c"
    "format/tm_format.c"
    "log/tm_log.c"
    "runtime/tm_runtime.c"
    "time/core_time.c"
)

if(THEMIS_FEATURE_IPC)
  list(APPEND _subsys_core_sources "ipc/tm_ipc.c")
endif()

if(THEMIS_FEATURE_TASK_BUDGET)
  list(APPEND _subsys_core_sources "runtime/tm_task_budget_probe.c")
endif()

if(THEMIS_FEATURE_SHELL)
  list(APPEND _subsys_core_sources "shell/tm_shell.c")
endif()

set(_subsys_core_public_include_dirs
    "assert"
    "daemon"
    "export"
    "format"
    "ipc"
    "log"
    "runtime"
    "shell"
    "time"
)

themis_add_module(
  NAME
  subsys-core
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  SOURCES
  ${_subsys_core_sources}
  PUBLIC_INCLUDE_DIRS
  ${_subsys_core_public_include_dirs}
  PUBLIC_DEPS
  freertos
  utils
  FOLDER
  "themis/subsys"
)

themis_add_interface_module(
  NAME
  subsys
  INTERFACE_DEPS
  subsys-core
  FOLDER
  "themis/subsys"
)
