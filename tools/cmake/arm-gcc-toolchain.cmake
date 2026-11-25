# Themis ARM bare-metal toolchain baseline. Role: - locate arm-none-eabi tools and expose MCU cache
# overrides - default to the AT32F415 core (Cortex-M4 without FPU, soft-float ABI) - establish
# compiler and linker defaults before the project configures - stay limited to toolchain facts, not
# module ownership or product policy

# Cross-compilation identity --------------------------------------------------------------------
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Prevent CMake from probing the target with a linker test. Bare-metal firmware cannot be executed
# on the host during compiler detection.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Toolchain binary discovery --------------------------------------------------------------------
if(CMAKE_HOST_WIN32)
  set(_TOOL_SUFFIX ".exe")
else()
  set(_TOOL_SUFFIX "")
endif()

find_program(
  CMAKE_C_COMPILER
  NAMES "arm-none-eabi-gcc${_TOOL_SUFFIX}" REQUIRED
  DOC "ARM GCC C compiler"
)

find_program(
  CMAKE_CXX_COMPILER
  NAMES "arm-none-eabi-g++${_TOOL_SUFFIX}" REQUIRED
  DOC "ARM GCC C++ compiler"
)

find_program(
  CMAKE_ASM_COMPILER
  NAMES "arm-none-eabi-gcc${_TOOL_SUFFIX}" REQUIRED
  DOC "ARM GCC assembler (via gcc -x assembler-with-cpp)"
)

find_program(
  CMAKE_OBJCOPY
  NAMES "arm-none-eabi-objcopy${_TOOL_SUFFIX}" REQUIRED
  DOC "ARM objcopy for binary/hex generation"
)

find_program(
  CMAKE_SIZE
  NAMES "arm-none-eabi-size${_TOOL_SUFFIX}" REQUIRED
  DOC "ARM size utility for memory-usage reporting"
)

# MCU architecture flags ------------------------------------------------------------------------
# AT32F415 is a Cortex-M4 without an FPU; firmware/at32f415 rejects any other combination.
set(MCU_CPU
    "cortex-m4"
    CACHE STRING "Target CPU core. Override only for exceptional bring-up/debug."
)
set(MCU_FPU
    "none"
    CACHE STRING "Target FPU. Leave empty or none for CPU classes without an FPU."
)
set(MCU_FLOAT
    "soft"
    CACHE STRING "Floating-point ABI (hard | soft | softfp)."
)

set(_MCU_ARCH_FLAGS "-mcpu=${MCU_CPU}" "-mthumb")
if((NOT "${MCU_FPU}" STREQUAL "") AND (NOT "${MCU_FPU}" STREQUAL "none"))
  list(APPEND _MCU_ARCH_FLAGS "-mfpu=${MCU_FPU}")
endif()
if(NOT "${MCU_FLOAT}" STREQUAL "")
  list(APPEND _MCU_ARCH_FLAGS "-mfloat-abi=${MCU_FLOAT}")
endif()
string(JOIN " " _MCU_ARCH_FLAGS_STR ${_MCU_ARCH_FLAGS})

# Compiler flags --------------------------------------------------------------------------------
# _INIT variables establish one stable baseline before the project adds layer-specific warnings or
# optimization choices. FORCE avoids silent duplicate accumulation across re-configures.
set(CMAKE_C_FLAGS_INIT
    "${_MCU_ARCH_FLAGS_STR} -fdata-sections -ffunction-sections -Wall"
    CACHE STRING "C compiler baseline flags" FORCE
)

set(CMAKE_CXX_FLAGS_INIT
    "${_MCU_ARCH_FLAGS_STR} -fdata-sections -ffunction-sections -Wall -fno-exceptions -fno-rtti"
    CACHE STRING "C++ compiler baseline flags" FORCE
)

set(CMAKE_ASM_FLAGS_INIT
    "${_MCU_ARCH_FLAGS_STR} -x assembler-with-cpp"
    CACHE STRING "Assembler baseline flags" FORCE
)

# Linker flags ----------------------------------------------------------------------------------
# --specs must live in the linker flags, not the compiler flags, or GCC 14 emits "specs used twice"
# warnings. The linker script itself is attached per target from the root build entry.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${_MCU_ARCH_FLAGS_STR} --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections"
    CACHE STRING "Linker baseline flags" FORCE
)

# CMake find_xxx() search-path policy -----------------------------------------------------------
# During cross-compilation, host tools still come from the host PATH, but target libraries/headers
# must never resolve against the host system.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
