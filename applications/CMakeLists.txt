# Themis applications manifest. Role: - compile product-owned FOC logic and RTOS tasks - keep the
# firmware entry source outside the static library so root CMake owns the final startup path

themis_add_module(
  NAME
  applications
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  GLOBS
  "*.c"
  "algo/*.c"
  "periph/*.c"
  "runtime/*.c"
  "shell/*.c"
  "tasks/*.c"
  PUBLIC_INCLUDE_DIRS
  "."
  "algo"
  "config"
  "periph"
  "runtime"
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
