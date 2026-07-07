# Themis Cortex-M3 soft-float CPU-class defaults. Role: - own the generic core/FPU/ABI, FreeRTOS
# port, and ARM_MATH contract shared by M3-soft SoCs - stay free of vendor-, SoC-, board-, or
# product-specific policy

set(THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT "cortex-m3")
set(THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT "none")
set(THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT "soft")
set(THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT "portable/GCC/ARM_CM3/*.c")
set(THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT "portable/GCC/ARM_CM3")
set(THEMIS_CPU_ARM_MATH_DEFINE_DEFAULT "ARM_MATH_CM3")
