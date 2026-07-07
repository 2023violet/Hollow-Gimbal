# Themis Cortex-M4F hard-float CPU-class defaults. Role: - own the generic core/FPU/ABI, FreeRTOS
# port, and ARM_MATH contract shared by M4F-hard SoCs - stay free of vendor-, SoC-, board-, or
# product-specific policy

set(THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT "cortex-m4")
set(THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT "fpv4-sp-d16")
set(THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT "hard")
set(THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT "portable/GCC/ARM_CM4F/*.c")
set(THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT "portable/GCC/ARM_CM4F")
set(THEMIS_CPU_ARM_MATH_DEFINE_DEFAULT "ARM_MATH_CM4")
