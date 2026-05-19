# Themis applications manifest. Role: - compile product-owned FOC logic and RTOS tasks - keep the
# firmware entry source outside the static library so root CMake owns the final startup path

set(_applications_globs "*.c" "algo/*.c" "periph/*.c" "tasks/*.c")
set(_applications_excludes)

if(THEMIS_FEATURE_SHELL)
  list(APPEND _applications_globs "shell/*.c")
else()
  list(APPEND _applications_excludes "tasks/app_shell_task.c" "tasks/shell_task.c")
endif()

if(NOT THEMIS_FEATURE_VOFA)
  list(APPEND _applications_excludes "tasks/vofa_task.c")
endif()

themis_add_module(
  NAME
  applications
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  GLOBS
  ${_applications_globs}
  EXCLUDE
  ${_applications_excludes}
  PUBLIC_INCLUDE_DIRS
  "."
  "algo"
  "config"
  "periph"
  "shell"
  "tasks"
  PUBLIC_DEPS
  algorithm
  bsp
  freertos
  protocol
  subsys-core
  utils
  ${THEMIS_PLATFORM_TARGET}
  FOLDER
  "themis/applications"
)
