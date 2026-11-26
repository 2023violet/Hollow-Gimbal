# AGENTS.md

FalconFOC V2 repository rules for AI and maintainers.

This file is the live repo entry contract. The earlier Themis/FreeRTOS Legacy
firmware was removed on 2026-11-24; it is preserved only at the Git tag
`legacy-final`. Do not restore it, and do not cite its paths as current.

## 1. Read Order

Read these first, in order:

1. `docs/architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md`
   - Product principles govern direction; current Git/source/evidence supersede
     its dated progress snapshot.
2. `docs/architecture/FalconFOC_BareMetal_Execution_ADR.md`
3. `README.md`
4. `docs/README.md`
5. `docs/superpowers/specs/2026-11-22-falconfoc-v2-parameter-contract-design.md`
   (UART/CAN parameters, calibration, persistence)
6. `firmware/at32f415/README.md` and `protocol/README.md`
7. `docs/governance/Themis_工程维护日志.md` (latest entries)
8. `docs/governance/Themis_代码规范指南.md`
9. `docs/governance/Themis_C与H文件布局与注释书写规范.md`
10. `docs/governance/Themis_AI高质量编码必读清单.md`

If touching these areas, read the matching local README too: `core/`,
`runtime/`, `calibration/`, `sensors/`, `g3/`, `control_plane/`, `simulation/`,
`protocol/`, `firmware/at32f415/`, `tools/falconfoc-cli/`, `scripts/`.

## 2. Product Boundary

FalconFOC V2 is a portable, sensored FOC servo for swappable PMSM motors.

- MCU: `AT32F415` (Cortex-M4 without FPU, soft-float), Board1 with DRV8311H.
- The controller never identifies a motor by itself; the user recalibrates on
  the MCU through a Host command after a motor swap.
- Calibration runs on the MCU only. Host tools drive it; they never compute it.
- Every feature needs a UART entry, a CLI command and a SIL demo scenario.
- Physical output stays locked in ARM images until a bench sign-off.
- There is no bench yet: all evidence is software (Host tests, Board1 SIL,
  fwinteg). Never present it as hardware evidence.

## 3. Layer Map

| Directory | Owns |
| --- | --- |
| `core/` | Portable C11 FOC math: Clarke/Park, SVPWM, current/velocity/position control, trajectory, impedance |
| `runtime/` | H0/H1/Foreground domains, lifecycle, Pipeline, protection, output submission |
| `calibration/` | Motor identification algorithms (R/L, flux, J/B, encoder) |
| `sensors/` | Encoder angle, velocity and quality |
| `g3/` | Device mapping (`np`) and Safety (`safe`) |
| `control_plane/` | Host parameter catalog, configuration codec and store (Host tools) |
| `protocol/` | UART V1 wire format and COBS framing |
| `firmware/at32f415/` | Board1 image: `main.c`, Board port, UART/CAN, MCU calibration, settings store, SIL |
| `simulation/` | Host Virtual Motor and virtual calibration |
| `examples/`, `tools/` | Host examples, Python CLI, replay and inspection tools |
| `platform/at/at32f415/vendor/` | Unmodified Artery CMSIS and peripheral drivers |
| `scripts/` | Quality gates, fwinteg, SDK packaging |

Every directory above with a `CMakeLists.txt` is a standalone CMake project;
there is no root CMake project.

## 4. FOC-Specific Rules

- The FOC hot path (H0 ISR, PWM, ADC, SPI encoder) may use vendor drivers and
  registers directly. Do not wrap it in a HAL.
- Soft-task hardware access (UART, CAN, Flash, reset) goes through the thin
  port structs in `firmware/at32f415`.
- Portability comes from the portable Core/Runtime, not from abstracting the hot path.
- The command lease stays 2.5 ms by default; hosts raise parameter ID 74.

## 5. Startup Rule

`firmware/at32f415/src/main.c` is the only entry: output lock → clock (crystal
PLL with HICK fallback) → peripherals → settings restore → CAN start → IRQ
enable → bare-metal H0 (ADC) / H1 / Foreground loop. See the
[execution ADR](docs/architecture/FalconFOC_BareMetal_Execution_ADR.md) and
`firmware/at32f415/LOCKED_TIMING.md`.

## 6. Documentation Rule

If you change any of these, update docs in the same round:

- layer ownership or directory structure
- startup chain or timing ownership
- UART/CAN protocol, parameters, persistence format or calibration stages
- script, gate or test entry points

Parameters have one source: edit `firmware/at32f415/parameters.json`, run
`python -B scripts/gen/gen_parameters.py` and commit the generated files; never
hand-edit `foc_parameter_table.{h,c}`, `falconfoc_params.py` or `docs/parameters.md`.

Always append a concise entry to `docs/governance/Themis_工程维护日志.md`.

## 7. Verification Rule

For real code changes, run the lanes that apply:

- Host lanes: each V2 project builds and tests on its own, for example
  `cmake -S firmware/at32f415 -B build/m0-board -G Ninja -DCMAKE_C_COMPILER=clang`,
  then `cmake --build` and `ctest --test-dir build/m0-board --output-on-failure`.
- ARM image:
  `cmake -S firmware/at32f415 -B build/m2-arm-default -G Ninja -DCMAKE_TOOLCHAIN_FILE=<repo>/tools/cmake/arm-gcc-toolchain.cmake`
  (add `-DFALCONFOC_BOARD_PERSISTENCE=ON` for the persistent-settings image).
- Board1 SIL: `ctest --test-dir build/m0-board -R '^test-foc-board1-sil' -j 4` (one CTest
  per `test_*` method of `tools/falconfoc-cli/test_board1_sil.py`), or
  `FOC_BOARD1_SIL=<abs path to falconfoc-board1-sil> python -B -m unittest test_board1_sil`
  in `tools/falconfoc-cli`. A new SIL `test_*` method is a new CTest: add it to
  `scripts/flows/fwinteg/expected_tests.tsv`.
- Quality gate:
  `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_falconfoc_quality_gate.ps1 -RepoRoot <repo> -RequireCMakeFormatter`
- Full integration (run alone, it is slow): `bash scripts/flows/fwinteg/run_fwinteg.sh build/fwinteg-<name>/run1`

Board facts: debugger `DAP-Link` (OpenOCD V2.0.6, `tools/openocd/openocd_daplink.cfg`);
UART V1 on USART1; CAN1 on PA11/PA12 through a SIT3051 (S pin grounded, 120 Ω on board).

## 8. Explicit Prohibitions

Do not:

- restore Legacy (Themis, FreeRTOS, shell, `applications/`, `bsp/`, `subsys/`,
  `components/`) code or docs to the live tree
- put vendor MCU headers into `core/`, `runtime/`, `calibration/`, `sensors/` or `g3/`
- unlock physical output in ARM images without a recorded bench sign-off
- keep stale tests or scripts that reference removed surfaces
- run Host calibration math in place of the MCU path
- use `double` in image code outside the Foreground calibration estimators listed in
  `scripts/gates/check_image_double.py`

## 9. Default Decision Rule

For major V2 architecture changes, apply the Architecture Change Gate in
`docs/architecture/FalconFOC_BareMetal_Execution_ADR.md` before implementation,
and record any superseding decision with acceptance and rollback criteria.

When uncertain:

- prefer the smallest live owner
- prefer explicit ownership over convenience
- keep the FOC hot path direct
- keep soft-task hardware access thin
- keep docs aligned with the real repository, not historical architecture
