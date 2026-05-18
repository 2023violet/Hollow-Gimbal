# AGENTS.md

FalconFoc AT32 repository rules for AI and maintainers.

This file is the live repo entry contract.

## 1. Read Order

Read these first, in order:

1. `README.md`
2. `docs/README.md`
3. `docs/architecture/README.md`
4. `docs/architecture/Themis_FalconFoc无devdrv模式与FOC直控分层方案.md`
5. `docs/architecture/Themis_BSP层定位、边界与优化路线图.md`
6. `docs/architecture/Themis_Export机制与阶段设计.md`
7. `docs/architecture/Themis_参数持久化存储设计说明.md`
8. `docs/boot/Themis_启动与初始化流程.md`
9. `docs/boot/Themis_启动链回归检查表.md`
10. `docs/governance/Themis_Transport与Shell硬化说明.md`
11. `docs/governance/Themis_Shell与诊断输出规范.md`
12. `docs/governance/Themis_工程维护日志.md`

If touching these areas, read the matching local entry too:

- `applications/`
  - `docs/application/README.md`
- `bsp/`
  - `docs/architecture/Themis_BSP层定位、边界与优化路线图.md`
- `platform/at/at32f415/`
  - `docs/boot/Themis_启动与初始化流程.md`
- `scripts/` or `tests/`
  - `scripts/README.md`
- `include/`
  - `include/README.md`
- `components/`
  - `components/README.md`

## 2. FalconFoc Boundary

This repo is FalconFoc-only and must stay focused on its current product
boundary.

Current live product facts:

- MCU line: `at32f415`
- live platform owner: `platform/at/at32f415`
- live board owner: `bsp/board`
- live thin control-plane port owner: `bsp/ports`
- live product owner: `applications/`
- live framework owner: `subsys/`
- live reusable code owner: `components/`

Current hard boundary:

- FalconFoc does not assemble Themis `device/` or `drivers/` runtime.
- FOC hot path may directly use vendor headers, generated code, and registers.
- soft-task and control-plane hardware access should be collected behind thin
  BSP ports, not a generic HAL.
- `work/` is out of scope.

## 3. Layer Rules

### `platform/at/at32f415`

Owns:

- vendor startup
- linker script
- generated peripheral bring-up
- MCU headers and chip baseline

Does not own:

- product task orchestration
- shell semantics
- parameter policy

### `bsp/board`

Owns:

- board facts
- minimal pre-kernel glue
- board-owned flash backend

Does not own:

- product tasks
- shell commands
- copied vendor driver stacks

### `bsp/ports`

Owns:

- thin functional control-plane ports such as console or system reset

Does not own:

- FOC ISR hot path
- generic future-proof abstraction layers
- product policy

### `applications`

Owns:

- app startup bridge
- motor orchestration
- state machine
- product shell commands
- parameter versioning and recovery policy

### `subsys`

Owns:

- shared framework mechanisms

Does not own:

- FalconFoc product behavior

## 4. FOC-Specific Rules

Formal split:

- FOC hot path:
  - direct control is allowed
  - vendor library or register access is allowed
  - do not force this path through `bsp/ports`
- soft-task peripherals:
  - use thin function-oriented ports
  - do not build `hal/`, vtables, device registries, or deep wrapper trees

Migration rule:

- portability comes from isolating the soft-task/control-plane surfaces
- portability does not come from abstracting the full FOC hot path

## 5. Startup Rule

Current live startup chain:

`platform generated main -> applications/app_main -> bsp_init -> export bring-up -> motor_task_init -> shell_task_init -> app_shell_task / motor_task / motor_safe_task / vofa_task`

Do not move startup ownership back into retired BSP or historical task paths.

## 6. Documentation Rule

If you change any of these, update docs in the same round:

- layer ownership
- startup chain
- shell transport policy
- parameter save/recover/version strategy
- BSP folder structure
- script or test entry points

Always append a concise entry to:

- `docs/governance/Themis_工程维护日志.md`

## 7. Verification Rule

For real code changes, verify with the current live lanes when applicable:

- firmware build:
  - `./run.sh build`
- host tests:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32`
- quality gate:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32 -Scope all`

Board facts currently documented in live docs:

- debugger: `DAPLink`
- console policy: `USART1 console-first`
- verified serial port witness in docs: `COM54`

## 8. Explicit Prohibitions

Do not:

- reintroduce full `device/` / `drivers/` runtime into FalconFoc
- pollute app-layer headers with vendor MCU headers unless the file is part of
  the FOC hardware-coupled path
- write unrelated product docs back into live FalconFoc entry points
- keep stale tests or scripts that reference removed product surfaces
- expand `bsp/board` into a second platform layer
- restore retired overview / planning / prompt / devdrv docs to the live read path

## 9. Default Decision Rule

When uncertain:

- prefer the smallest live owner
- prefer explicit ownership over convenience
- keep FOC hot path direct
- keep soft-task hardware access thin
- keep docs aligned with the real repository, not historical architecture
