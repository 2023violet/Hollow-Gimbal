# Themis algorithm component manifest. Role: - compile reusable control and filter logic - expose
# only the explicit algorithm public headers - keep this file limited to sources, includes, and
# dependency truth

themis_add_module(
  NAME
  algorithm
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  GLOBS
  "pid/*.c"
  "filter/*.c"
  PUBLIC_INCLUDE_DIRS
  "filter"
  "pid"
  # `pid.h` exposes `core_time.h`, and the public algorithm headers use `arm_math.h`, so both
  # dependencies are real consumer-facing contracts.
  PUBLIC_DEPS
  subsys-core
  ${THEMIS_PLATFORM_TARGET}
  FOLDER
  "themis/components"
)
