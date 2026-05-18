# Themis protocol component manifest. Role: - compile reusable protocol-side parsing and framing
# code - export only the reviewed protocol public headers - keep product-feature slicing out of
# component source files - keep the module declaration focused on ownership facts only

themis_add_module(
  NAME
  protocol
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  SOURCES
  "vofa/vofa.c"
  PUBLIC_INCLUDE_DIRS
  "vofa"
  PRIVATE_DEPS
  utils
  FOLDER
  "themis/components"
)
