# FalconFoc Prepared Platform And Timing Closure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make future `AT32F415` / `AT32F423` trials a platform-local operation by adding prepared inactive platform directories and moving remaining SoC timing facts out of `applications/algo/foc_config.h`.

**Architecture:** Phase 1 keeps `AT32F403A` as the only live baseline while introducing a platform-owned FOC timing seam that upper-layer algorithm code consumes without knowing the specific SoC. Phase 2 adds prepared `at32f415` and `at32f423` platform directories with their own metadata, generated code, vendor startup/linker facts, and timing headers, then verifies that switching SoC only needs `-DTHEMIS_PLATFORM_SOC=<soc>`.

**Tech Stack:** CMake, vendor AT32 platform packs, existing FalconFoc firmware presets, existing PowerShell gates, host tests, firmware builds, optional board witness when matching hardware exists.

---

### Task 1: Move FOC timer-clock truth back to the platform owner

**Files:**
- Create: `platform/at/at32f403a/generated/inc/tm_platform_foc_timing.h`
- Modify: `applications/algo/foc_config.h`
- Modify: `docs/architecture/Themis_FalconFoc平台换芯与SoC迁移指南.md`
- Modify: `docs/governance/Themis_工程维护日志.md`

- [ ] **Step 1: Introduce a platform timing header for the live 403A baseline**

```c
#pragma once

#define TM_PLATFORM_TMR_CLK_HZ        (240000000U)
#define TM_PLATFORM_TMR_CLK_MHZ       (240U)
#define TM_PLATFORM_PWM_FREQ_HZ       (20000U)
#define TM_PLATFORM_VEL_LOOP_FREQ_HZ  (5000U)
#define TM_PLATFORM_POS_LOOP_FREQ_HZ  (1000U)
#define TM_PLATFORM_TRAJ_TASK_FREQ_HZ (1000U)
```

- [ ] **Step 2: Rebind `foc_config.h` to the platform timing seam**

```c
#include "tm_platform_foc_timing.h"

#define TMR_CLK_HZ        TM_PLATFORM_TMR_CLK_HZ
#define TMR_CLK_MHZ       TM_PLATFORM_TMR_CLK_MHZ
#define PWM_FREQ_HZ       TM_PLATFORM_PWM_FREQ_HZ
#define VEL_LOOP_FREQ_HZ  TM_PLATFORM_VEL_LOOP_FREQ_HZ
#define POS_LOOP_FREQ_HZ  TM_PLATFORM_POS_LOOP_FREQ_HZ
#define TRAJ_TASK_FREQ_HZ TM_PLATFORM_TRAJ_TASK_FREQ_HZ
```

- [ ] **Step 3: Verify the app layer no longer owns `240 MHz` SoC truth**

Run: `rg -n "240000000|AT32F403A: PLL|TMR_CLK_HZ             \\(240000000U\\)" applications/algo applications/periph applications/runtime applications/tasks`

Expected: no hits in `applications/`.

- [ ] **Step 4: Document the new owner split**

Document that:
- timer/PWM cadence truth is now platform-owned
- `applications/algo/foc_config.h` keeps product defaults and formulas only
- future SoC swaps must edit the platform timing header, not the app config

- [ ] **Step 5: Commit**

```bash
git add platform/at/at32f403a/generated/inc/tm_platform_foc_timing.h applications/algo/foc_config.h docs/architecture/Themis_FalconFoc平台换芯与SoC迁移指南.md docs/governance/Themis_工程维护日志.md
git commit -m "refactor: move foc timing facts to platform owner"
```

### Task 2: Prepare an inactive `AT32F415` platform directory

**Files:**
- Create: `platform/at/at32f415/CMakeLists.txt`
- Create: `platform/at/at32f415/ThemisPlatformDefaults.cmake`
- Create: `platform/at/at32f415/generated/inc/tm_platform_foc_timing.h`
- Create: `platform/at/at32f415/generated/**`
- Create: `platform/at/at32f415/vendor/**`
- Create: `platform/at/at32f415/AT32F415xx*.svd`
- Modify: `scripts/gates/check_platform_backend_contract.ps1`

- [ ] **Step 1: Clone the current 403A platform layout as the template**

Copy the full directory structure pattern from:
- `platform/at/at32f403a/CMakeLists.txt`
- `platform/at/at32f403a/generated/`
- `platform/at/at32f403a/vendor/`
- `platform/at/at32f403a/ThemisPlatformDefaults.cmake`

Adjust names and source lists so they match the real `AT32F415` vendor package instead of `403a_407`.

- [ ] **Step 2: Define the 415 metadata contract locally**

```cmake
set(THEMIS_PLATFORM_TARGET_DEFAULT "at32f415-hal")
set(THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT "generated/src/main.c")
set(THEMIS_PLATFORM_STARTUP_SOURCE_DEFAULT "vendor/cmsis/cm4/device_support/startup/gcc/startup_at32f415_xxx.s")
set(THEMIS_PLATFORM_LINKER_SCRIPT_DEFAULT "vendor/cmsis/cm4/device_support/startup/gcc/linker/AT32F415xX_FLASH.ld")
set(THEMIS_PLATFORM_COMPILE_DEFINITIONS_DEFAULT "AT32F415xxxx;USE_STDPERIPH_DRIVER;ARM_MATH_CM4;ARM_MATH_MATRIX_CHECK;ARM_MATH_ROUNDING;ARM_MATH_LOOPUNROLL")
```

- [ ] **Step 3: Give the 415 platform its own timing facts**

```c
#define TM_PLATFORM_TMR_CLK_HZ        (150000000U)
#define TM_PLATFORM_TMR_CLK_MHZ       (150U)
#define TM_PLATFORM_PWM_FREQ_HZ       (20000U)
#define TM_PLATFORM_VEL_LOOP_FREQ_HZ  (5000U)
#define TM_PLATFORM_POS_LOOP_FREQ_HZ  (1000U)
#define TM_PLATFORM_TRAJ_TASK_FREQ_HZ (1000U)
```

- [ ] **Step 4: Verify prepared-platform configure/build without changing the live baseline**

Run:
- `cmake -S . -B build/fw-at32f415-smoke -G Ninja -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-gcc-toolchain.cmake -DTHEMIS_BUILD_FIRMWARE=ON -DTHEMIS_BUILD_HOST_TESTS=OFF -DTHEMIS_DEVDRV_PROFILE=full-diag -DTHEMIS_PRODUCT_FLAVOR=carrier-demo -DTHEMIS_PLATFORM_VENDOR=at -DTHEMIS_PLATFORM_SOC=at32f415`
- `cmake --build build/fw-at32f415-smoke --parallel`

Expected:
- configure passes without any startup/linker/target/compile-definition override
- root still defaults to `at32f403a` when `THEMIS_PLATFORM_SOC` is not specified

- [ ] **Step 5: Commit**

```bash
git add platform/at/at32f415 scripts/gates/check_platform_backend_contract.ps1
git commit -m "feat: add prepared at32f415 platform"
```

### Task 3: Prepare an inactive `AT32F423` platform directory

**Files:**
- Create: `platform/at/at32f423/CMakeLists.txt`
- Create: `platform/at/at32f423/ThemisPlatformDefaults.cmake`
- Create: `platform/at/at32f423/generated/inc/tm_platform_foc_timing.h`
- Create: `platform/at/at32f423/generated/**`
- Create: `platform/at/at32f423/vendor/**`
- Create: `platform/at/at32f423/AT32F423xx*.svd`
- Modify: `scripts/gates/check_platform_backend_contract.ps1`

- [ ] **Step 1: Mirror the 415 task structure for 423 using the actual vendor package**

Keep the same owner pattern:
- platform directory owns startup/linker/part macro
- generated tree owns SoC-specific bring-up
- timing header owns cadence facts

- [ ] **Step 2: Define the 423 metadata contract locally**

```cmake
set(THEMIS_PLATFORM_TARGET_DEFAULT "at32f423-hal")
set(THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT "generated/src/main.c")
set(THEMIS_PLATFORM_STARTUP_SOURCE_DEFAULT "vendor/cmsis/cm4/device_support/startup/gcc/startup_at32f423_xxx.s")
set(THEMIS_PLATFORM_LINKER_SCRIPT_DEFAULT "vendor/cmsis/cm4/device_support/startup/gcc/linker/AT32F423xX_FLASH.ld")
set(THEMIS_PLATFORM_COMPILE_DEFINITIONS_DEFAULT "AT32F423xxxx;USE_STDPERIPH_DRIVER;ARM_MATH_CM4;ARM_MATH_MATRIX_CHECK;ARM_MATH_ROUNDING;ARM_MATH_LOOPUNROLL")
```

- [ ] **Step 3: Give the 423 platform its own timing facts**

```c
#define TM_PLATFORM_TMR_CLK_HZ        (150000000U)
#define TM_PLATFORM_TMR_CLK_MHZ       (150U)
#define TM_PLATFORM_PWM_FREQ_HZ       (20000U)
#define TM_PLATFORM_VEL_LOOP_FREQ_HZ  (5000U)
#define TM_PLATFORM_POS_LOOP_FREQ_HZ  (1000U)
#define TM_PLATFORM_TRAJ_TASK_FREQ_HZ (1000U)
```

- [ ] **Step 4: Verify prepared-platform configure/build**

Run:
- `cmake -S . -B build/fw-at32f423-smoke -G Ninja -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-gcc-toolchain.cmake -DTHEMIS_BUILD_FIRMWARE=ON -DTHEMIS_BUILD_HOST_TESTS=OFF -DTHEMIS_DEVDRV_PROFILE=full-diag -DTHEMIS_PRODUCT_FLAVOR=carrier-demo -DTHEMIS_PLATFORM_VENDOR=at -DTHEMIS_PLATFORM_SOC=at32f423`
- `cmake --build build/fw-at32f423-smoke --parallel`

Expected: configure/build passes with only `THEMIS_PLATFORM_SOC=at32f423` changed.

- [ ] **Step 5: Commit**

```bash
git add platform/at/at32f423 scripts/gates/check_platform_backend_contract.ps1
git commit -m "feat: add prepared at32f423 platform"
```

### Task 4: Tighten gates and docs around the new prepared-platform contract

**Files:**
- Modify: `tests/script/test_falconfoc_themis_lite_contract.ps1`
- Modify: `scripts/gates/check_cmake_quality.ps1`
- Modify: `docs/architecture/Themis_CMake构建架构说明.md`
- Modify: `docs/architecture/Themis_FalconFoc平台换芯与SoC迁移指南.md`
- Modify: `docs/governance/Themis_工程维护日志.md`

- [ ] **Step 1: Make the gate check the new platform timing seam**

Add assertions that each real prepared platform contains:
- `ThemisPlatformDefaults.cmake`
- `generated/inc/tm_platform_foc_timing.h`

- [ ] **Step 2: Keep the live-baseline rule frozen**

The contract test must still enforce:
- default SoC is `at32f403a`
- `at32f415` / `at32f423` do not auto-become presets
- root `CMakeLists.txt` still contains no SoC-specific startup/linker/part literals

- [ ] **Step 3: Update docs to the new operating model**

Document that:
- stage-1 infrastructure is complete
- stage-2 prepared platforms exist but are inactive
- the standard smoke path is now “copy platform pack, fill defaults, fill timing header, switch `THEMIS_PLATFORM_SOC`”

- [ ] **Step 4: Run the gate suite**

Run:
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_platform_backend_contract.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32`
- `powershell -NoProfile -ExecutionPolicy Bypass -File tests/script/test_falconfoc_themis_lite_contract.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32`
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32 -Scope cmake`

Expected: all pass while `AT32F403A` remains the only live baseline.

- [ ] **Step 5: Commit**

```bash
git add tests/script/test_falconfoc_themis_lite_contract.ps1 scripts/gates/check_cmake_quality.ps1 docs/architecture/Themis_CMake构建架构说明.md docs/architecture/Themis_FalconFoc平台换芯与SoC迁移指南.md docs/governance/Themis_工程维护日志.md
git commit -m "docs: formalize prepared platform contract"
```

### Task 5: Finish with a full smoke matrix and a clean handoff

**Files:**
- Verify only: `build/host-debug/**`, `build/fw-full-diag-carrier-debug/**`, `build/fw-standard-carrier-minsizerel/**`, `build/fw-tiny-carrier-minsizerel/**`
- Optional doc note: `docs/architecture/Themis_FalconFoc平台换芯与SoC迁移指南.md`

- [ ] **Step 1: Re-run the live 403A matrix**

Run:
- `cmake --preset host-debug`
- `cmake --build --preset build-host-debug --parallel`
- `ctest --preset test-host-debug`
- `cmake --preset fw-full-diag-carrier-debug`
- `cmake --build --preset build-fw-full-diag-carrier-debug --parallel`
- `cmake --preset fw-standard-carrier-minsizerel`
- `cmake --build --preset build-fw-standard-carrier-minsizerel --parallel`
- `cmake --preset fw-tiny-carrier-minsizerel`
- `cmake --build --preset build-fw-tiny-carrier-minsizerel --parallel`

Expected: the live 403A baseline stays green after prepared-platform additions.

- [ ] **Step 2: Record the manual witness rule**

If matching hardware exists, do one minimal board witness per prepared SoC:
- flash
- `help`
- `sys status`
- `store status`

If hardware does not exist yet, explicitly record “build-only prepared platform”.

- [ ] **Step 3: Clean the handoff**

Before handing off:
- keep generated `build/` churn out of the review summary
- separate “live baseline” results from “prepared platform” results
- call out any remaining direct vendor-header use in `applications/periph/` as intentional hot-path coupling, not migration debt

- [ ] **Step 4: Commit**

```bash
git add docs/architecture/Themis_FalconFoc平台换芯与SoC迁移指南.md
git commit -m "chore: record prepared platform smoke matrix"
```
