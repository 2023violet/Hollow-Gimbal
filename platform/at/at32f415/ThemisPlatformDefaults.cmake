# Themis AT32F415 platform defaults. Role: - own this prepared SoC's default target, entry,
# startup, linker, compile-definition, and CPU-class facts - keep chip-specific build metadata
# inside the platform owner instead of the repository root

set(THEMIS_PLATFORM_TARGET_DEFAULT "at32f415-hal")
set(THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT "generated/src/main.c")
set(THEMIS_PLATFORM_STARTUP_SOURCE_DEFAULT
    "vendor/cmsis/cm4/device_support/startup/gcc/startup_at32f415.s"
)
set(THEMIS_PLATFORM_LINKER_SCRIPT_DEFAULT
    "vendor/cmsis/cm4/device_support/startup/gcc/linker/AT32F415xB_FLASH.ld"
)
set(THEMIS_PLATFORM_CPU_CLASS_DEFAULT "cortex-m3-soft")
set(THEMIS_PLATFORM_COMPILE_DEFINITIONS_DEFAULT
    AT32F415KBU7_4
    USE_STDPERIPH_DRIVER
    ARM_MATH_MATRIX_CHECK
    ARM_MATH_ROUNDING
    ARM_MATH_LOOPUNROLL
    "CMSIS_device_header=<at32f415.h>"
    "THEMIS_PLATFORM_FLASH_DRIVER_HEADER=<at32f415_flash.h>"
    THEMIS_PLATFORM_FLASH_BASE_ADDR=0x08000000U
    THEMIS_PLATFORM_FLASH_SIZE_BYTES=0x00020000U
    THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES=0x00000400U
    THEMIS_PLATFORM_FLASH_SECTOR_COUNT=128U
    THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER=TMR5_GLOBAL_IRQHandler
    THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE=TMR5
)
