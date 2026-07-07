# Themis at32f415 platform manifest. Role: - compile vendor/platform sources that define the
# active SoC baseline - export platform include directories and compile-time contracts - keep
# generated platform facts isolated from upper-layer policy

themis_add_module(
  NAME
  ${THEMIS_PLATFORM_TARGET}
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  SOURCES
  "generated/src/at32f415_int.c"
  "generated/src/at32f415_wk_config.c"
  "generated/src/syscalls.c"
  "generated/src/sysmem.c"
  "generated/src/wk_adc.c"
  "generated/src/wk_debug.c"
  "generated/src/wk_dma.c"
  "generated/src/wk_gpio.c"
  "generated/src/wk_spi.c"
  "generated/src/wk_system.c"
  "generated/src/wk_tmr.c"
  "generated/src/wk_usart.c"
  "vendor/drivers/src/at32f415_adc.c"
  "vendor/drivers/src/at32f415_crc.c"
  "vendor/drivers/src/at32f415_crm.c"
  "vendor/drivers/src/at32f415_debug.c"
  "vendor/drivers/src/at32f415_dma.c"
  "vendor/drivers/src/at32f415_exint.c"
  "vendor/drivers/src/at32f415_flash.c"
  "vendor/drivers/src/at32f415_gpio.c"
  "vendor/drivers/src/at32f415_misc.c"
  "vendor/drivers/src/at32f415_spi.c"
  "vendor/drivers/src/at32f415_tmr.c"
  "vendor/drivers/src/at32f415_usart.c"
  "vendor/cmsis/cm4/device_support/system_at32f415.c"
  "vendor/cmsis/dsp/Source/CommonTables/CommonTables.c"
  "vendor/cmsis/dsp/Source/FastMathFunctions/arm_cos_f32.c"
  "vendor/cmsis/dsp/Source/FastMathFunctions/arm_sin_f32.c"
  "vendor/cmsis/dsp/Source/MatrixFunctions/arm_mat_add_f32.c"
  "vendor/cmsis/dsp/Source/MatrixFunctions/arm_mat_init_f32.c"
  "vendor/cmsis/dsp/Source/MatrixFunctions/arm_mat_inverse_f32.c"
  "vendor/cmsis/dsp/Source/MatrixFunctions/arm_mat_mult_f32.c"
  "vendor/cmsis/dsp/Source/MatrixFunctions/arm_mat_sub_f32.c"
  "vendor/cmsis/dsp/Source/MatrixFunctions/arm_mat_trans_f32.c"
  EXCLUDE
  "generated/src/main.c"
  PUBLIC_INCLUDE_DIRS
  "generated/inc"
  "vendor/drivers/inc"
  "vendor/cmsis/cm4/device_support"
  "vendor/cmsis/cm4/core_support"
  "vendor/cmsis/dsp/include"
  "vendor/cmsis/dsp/include/dsp"
  "vendor/cmsis/dsp/PrivateInclude"
  FOLDER
  "themis/platform"
)

target_link_libraries(${THEMIS_PLATFORM_TARGET} PUBLIC themis_platform_contract)
target_compile_options(
  ${THEMIS_PLATFORM_TARGET} PRIVATE $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wno-unused-variable>
)

if((THEMIS_PLATFORM_TARGET STREQUAL "at32f415-hal") AND (NOT TARGET at32f415_hal))
  add_library(at32f415_hal ALIAS at32f415-hal)
endif()
