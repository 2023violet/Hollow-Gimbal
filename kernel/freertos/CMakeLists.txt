# Themis FreeRTOS kernel manifest. Role: - compile the selected FreeRTOS kernel, CMSIS-RTOS2 bridge,
# and heap backend - expose RTOS public headers to upper layers - keep heap backend selection
# explicit and centralized

set(_freertos_heap_source "portable/MemMang/${THEMIS_FREERTOS_HEAP_IMPL}.c")

if((NOT DEFINED THEMIS_CPU_FREERTOS_PORT_GLOB) OR ("${THEMIS_CPU_FREERTOS_PORT_GLOB}" STREQUAL ""))
  message(FATAL_ERROR "FreeRTOS kernel manifest requires THEMIS_CPU_FREERTOS_PORT_GLOB.")
endif()

if((NOT DEFINED THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR) OR ("${THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR}"
                                                          STREQUAL "")
)
  message(FATAL_ERROR "FreeRTOS kernel manifest requires THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR.")
endif()

themis_add_module(
  NAME
  freertos
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  GLOBS
  "croutine.c"
  "event_groups.c"
  "list.c"
  "queue.c"
  "stream_buffer.c"
  "tasks.c"
  "timers.c"
  "CMSIS_RTOS_V2/*.c"
  "CMSIS_RTOS_V2/os_systick.c"
  "${THEMIS_CPU_FREERTOS_PORT_GLOB}"
  "${_freertos_heap_source}"
  PUBLIC_INCLUDE_DIRS
  "include"
  "${THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR}"
  "CMSIS_RTOS_V2"
  PRIVATE_COMPILE_DEFINITIONS
  "_RTE_"
  PUBLIC_DEPS
  ${THEMIS_PLATFORM_TARGET}
  FOLDER
  "themis/kernel"
)
