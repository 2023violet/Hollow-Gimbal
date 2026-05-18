# FalconFoc AT32

FalconFoc AT32 is the current FalconFoc firmware repository aligned to the
Themis layering style, but intentionally without the full `device/` and
`drivers/` runtime stack.

The target is a product-grade FOC codebase that keeps the hot path direct,
keeps the control plane thin, and stays migratable at the upper layers toward
RT-Thread and Zephyr.

## Portability | 可移植性

- `RT-Thread`
  - target direction for product orchestration, shell adaptation, and task
    structure reuse
- `Zephyr`
  - target direction for the same upper-layer architecture model
- FalconFoc rule
  - portability comes from isolating soft-task/control-plane surfaces
  - portability does not come from abstracting the whole FOC hot path

## Architecture | 架构

- `platform/at/at32f415`
  - single source of truth for SoC bring-up, startup, linker, vendor headers,
    and generated code
- `bsp/board`
  - board facts, flash backend, and minimal pre-kernel glue
- `bsp/ports`
  - thin functional ports for control-plane hardware access
- `applications`
  - FalconFoc product startup, shell, tasks, parameters, and algorithm owners
- `components`
  - reusable modules; current live protocol surface is VOFA-focused
- `subsys`
  - shared framework mechanisms used by FalconFoc-lite

Formal architecture rule:

- FOC current loop, PWM generation, ADC sampling sequence, DMA coupling, and
  similar hot paths may directly use vendor APIs or registers.
- soft-task peripherals should be collected behind thin BSP ports rather than a
  generic HAL or a full device-model stack.

## Workflows | 工作流

- build firmware:
  - `./run.sh build`
- flash firmware:
  - `./run.sh flash`
- build and flash:
  - `./run.sh all`
- memory report:
  - `./run.sh memory`
- diag/cal board witness:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/board/run_diag_carrier_witness.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32`
- host tests:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32`
- quality gate:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32 -Scope all`

The current live board-debug lane is documented around:

- debugger: `J-Link`
- console policy: `USART1 console-first`
- verified serial witness in docs: `COM54`
- verified shell probe baud: `1000000`
- formal diag/cal witness flow: `scripts/run_diag_carrier_witness.ps1`

## Documentation | 文档

- main docs entry:
  - [docs/README.md](./docs/README.md)
- no-`dev/drv` architecture decision:
  - [docs/architecture/Themis_FalconFoc无devdrv模式与FOC直控分层方案.md](./docs/architecture/Themis_FalconFoc%E6%97%A0devdrv%E6%A8%A1%E5%BC%8F%E4%B8%8EFOC%E7%9B%B4%E6%8E%A7%E5%88%86%E5%B1%82%E6%96%B9%E6%A1%88.md)
- startup chain:
  - [docs/boot/Themis_启动与初始化流程.md](./docs/boot/Themis_%E5%90%AF%E5%8A%A8%E4%B8%8E%E5%88%9D%E5%A7%8B%E5%8C%96%E6%B5%81%E7%A8%8B.md)
- shell policy:
  - [docs/governance/Themis_Shell与诊断输出规范.md](./docs/governance/Themis_Shell%E4%B8%8E%E8%AF%8A%E6%96%AD%E8%BE%93%E5%87%BA%E8%A7%84%E8%8C%83.md)
- transport and serial strategy:
  - [docs/governance/Themis_Transport与Shell硬化说明.md](./docs/governance/Themis_Transport%E4%B8%8EShell%E7%A1%AC%E5%8C%96%E8%AF%B4%E6%98%8E.md)

Repository hygiene rule:

- only FalconFoc current architecture, build, shell, and startup documents are
  allowed on the live read path
- retired experiments, superseded reviews, and unrelated product narratives do
  not belong in this repository
