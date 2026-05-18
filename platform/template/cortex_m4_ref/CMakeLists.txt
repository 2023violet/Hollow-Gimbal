# Themis template platform skeleton. Role: - declare the exported target shape for a future second
# platform port - freeze the generated/vendor ownership boundary before real sources land - stay
# validate-only until startup, linker, and generated facts are ready

themis_add_interface_module(
  NAME
  template-cortex-m4-ref-hal
  INTERFACE_DEPS
  themis_platform_contract
  INTERFACE_COMPILE_DEFINITIONS
  TM_PLATFORM_TEMPLATE=1
  FOLDER
  "themis/template"
)

if(NOT TARGET template_cortex_m4_ref_hal)
  add_library(template_cortex_m4_ref_hal ALIAS template-cortex-m4-ref-hal)
endif()
