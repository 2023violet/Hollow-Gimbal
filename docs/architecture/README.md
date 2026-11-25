# FalconFOC V2 Architecture Docs

`docs/architecture/` holds the product principles and the V2 execution
decision. The Legacy Themis architecture docs (BSP, Export, shell, FreeRTOS
startup) were removed on 2026-11-24 and remain at the Git tag `legacy-final`.

1. [Project charter](./FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md) — product principles; current Git, source and evidence supersede its dated progress snapshot.
2. [Bare-metal execution ADR](./FalconFOC_BareMetal_Execution_ADR.md) — H0/H1/Foreground ownership, timing targets and the Architecture Change Gate.

Current architecture facts:

- One Board1 image built from `firmware/at32f415`; no RTOS, no shell.
- Portable C11 Core and Runtime with no MCU headers (`core/`, `runtime/`,
  `calibration/`, `sensors/`, `g3/`).
- The FOC hot path uses vendor drivers and registers directly; soft-task
  peripherals (UART, CAN, Flash, reset) sit behind thin port structs.
- Calibration runs on the MCU; parameters and persistence follow the
  [parameter contract](../superpowers/specs/2026-11-22-falconfoc-v2-parameter-contract-design.md).
