# Themis BSP board manifest. Role: - compile only board facts and pre-kernel
# glue - keep soft-task functional ports outside the board owner

themis_add_module(
  NAME
  bsp
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  SOURCES
  "bsp_flash.c"
  "bsp_init.c"
  PUBLIC_INCLUDE_DIRS
  ".."
  "."
  PUBLIC_DEPS
  ${THEMIS_PLATFORM_TARGET}
  bsp-ports
  PRIVATE_DEPS
  subsys-core
  FOLDER
  "themis/bsp"
)
