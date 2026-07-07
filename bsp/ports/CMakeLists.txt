# Themis BSP port manifest. Role: - compile thin BSP-owned functional ports
# that expose control-plane hardware hooks without reintroducing dev/drv

themis_add_module(
  NAME
  bsp-ports
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  SOURCES
  "bsp_console.c"
  "bsp_system.c"
  PUBLIC_INCLUDE_DIRS
  ".."
  "."
  PUBLIC_DEPS
  ${THEMIS_PLATFORM_TARGET}
  PRIVATE_DEPS
  subsys-core
  FOLDER
  "themis/bsp"
)
