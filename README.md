# FalconFOC V2

FalconFOC V2 is a pure-C, C11-compatible FOC control and Runtime library with
an independent Host Virtual Motor and an AT32F415 reference target. The Core
does not require an RTOS, MCU headers, vendor SDK, shell, transport, or board
code. The Runtime adds scheduling-domain ownership, lifecycle, Safety, and an
explicit output-submission contract around that Core.

The repository also retains the earlier FalconFoc AT32 firmware and Legacy
host-test entry points. Those are reference and compatibility surfaces; the V2
library boundary is defined by `core/`, `runtime/`, `g3/`, `sensors/`,
`calibration/`, `protocol/`, `simulation/`, and `tools/falconfoc-cli/`.

## V2 product map

- **FalconFOC Core** — Clarke/Park, SVPWM, Current PI, Velocity PI, Position P,
  and Trajectory in portable C11.
- **FalconFOC Runtime** — H0/H1/Foreground execution domains, lifecycle,
  command and sensor publication, Safety, and output submission.
- **Reference targets** — Host Virtual Motor and the locked AT32F415 Board1
  reference firmware.
- **Tools** — Python V1 CLI, virtual-device demonstrations, control-quality
  measurements, and Host-only tuning research.

## Quick start

Build and test the portable Core:

```powershell
cmake -S core -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/core --parallel 4
ctest --test-dir build/core --output-on-failure
```

Build and test the Host Virtual Motor:

```powershell
cmake -S simulation -B build/vm01a-simulation -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/vm01a-simulation --parallel 4
ctest --test-dir build/vm01a-simulation --output-on-failure
```

The external Core-consumer fixture used for release preparation is documented
at [docs/evidence/2026-11-16-next-release-prep-01.md](docs/evidence/2026-11-16-next-release-prep-01.md).

## Supported and explicitly unverified

The portable Core is intended for caller-owned, sensored FOC integrations with
explicit phase-current, electrical-angle, bus-voltage, timing, and output
contracts. AT32F415 is the first reference target, not the permanent library
boundary. The project does not currently claim support for every MCU, motor,
ADC sampling topology, or automatic motor identification flow.

The following remain unverified or locked:

- real-motor control-quality acceptance;
- physical power-stage shutdown and bridge timing;
- AT32F415 WCET, interrupt jitter, and stack high-water measurement;
- arbitrary-MCU plug-and-play portability;
- automatic end-to-end motor identification;
- product-quality acceptance for every control condition;
- real Board1 PWM/MOE enablement.

Host Virtual outputs are software-model evidence and always report
`physical_applied=false`. The Host Research Profile from the tuning study is
not written to Board1 defaults.

## Documentation

- [V2 architecture decision and change gate](docs/architecture/FalconFOC_BareMetal_Execution_ADR.md)
- [Core README](core/README.md)
- [Runtime README](runtime/README.md)
- [Simulation README](simulation/README.md)
- [Protocol README](protocol/README.md)
- [Host Virtual Quickstart](docs/code-wiki/FalconFOC_VM_virtual_quickstart.md)
- [Servo Quickstart](docs/servo/quickstart.md)
- [Release-preparation evidence](docs/evidence/2026-11-16-next-release-prep-01.md)
- [Current software handoff](docs/code-wiki/AI-handoff-2026-11-15.md)

## Current release status

The V2 software has a reproducible Host baseline, independent Core-consumer
evidence, virtual-device demonstrations, and staged research evidence. This is
an engineering preparation state. It is not a formal public release: the
repository has no selected root project license, the source provenance review
for bundled vendor/CMSIS material is incomplete, and the full `Scope all` gate
is blocked on a supported Bash environment.

---

## Legacy AT32 firmware

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
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/board/run_diag_carrier_witness.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal`
- host tests:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal`
- quality gate:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope all`

The current live board-debug lane is documented around:

- debugger: `DAP-Link` (OpenOCD V2.0.6, `tools/openocd/openocd_daplink.cfg`)
- console policy: `USART1 console-first`
- current bench serial witness: `COM3 @ 115200`
- formal board flows: `scripts/flows/board/`

## Documentation | 文档

- current handoff and software baseline:
  - [docs/code-wiki/AI-handoff-2026-11-15.md](./docs/code-wiki/AI-handoff-2026-11-15.md)
- RC0 reproducible source baseline:
  - [docs/evidence/2026-11-15-next-rc0-baseline-01.md](./docs/evidence/2026-11-15-next-rc0-baseline-01.md)

- main docs entry:
  - [docs/README.md](./docs/README.md)
- current-source Code Wiki (AT32F415):
  - [docs/code-wiki/README.md](./docs/code-wiki/README.md)
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
