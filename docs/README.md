# FalconFOC V2 Docs

`docs/` is the documentation root for FalconFOC V2. The Themis/FreeRTOS Legacy
firmware and its docs were removed on 2026-11-24 and remain only at the Git tag
`legacy-final`.

## Live entries

1. [Project charter](./architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md) — product principles
2. [Bare-metal execution ADR](./architecture/FalconFOC_BareMetal_Execution_ADR.md) — H0/H1/Foreground, timing targets, change gate
3. [Parameter contract](./superpowers/specs/2026-11-22-falconfoc-v2-parameter-contract-design.md) — UART/CAN parameters, MCU calibration stages, persistence formats
4. [Board1 firmware](../firmware/at32f415/README.md) — pins, build, SIL, locked output
   - [Parameter table](./parameters.md) — every UART/CAN parameter, generated from `firmware/at32f415/parameters.json`
5. [Wire protocols](../protocol/README.md) — UART V1 and CANSimple
6. [Python CLI](../tools/falconfoc-cli/README.md) — commands and SIL scenarios
7. [Servo quickstart](./servo/quickstart.md) and [Host Virtual quickstart](./code-wiki/FalconFOC_VM_virtual_quickstart.md)
8. [Control-plane docs](./control-plane/quickstart.md) — Host configuration store and diagnostics
9. [Maintenance log](./governance/Themis_工程维护日志.md) — every change, newest at the end

Coding rules: [code style](./governance/Themis_代码规范指南.md),
[C/H layout and comments](./governance/Themis_C与H文件布局与注释书写规范.md),
[AI coding checklist](./governance/Themis_AI高质量编码必读清单.md).

## Historical material

These record how the project got here. They are not current-state pointers and
may cite removed Legacy paths.

- `code-wiki/` — Legacy-era source maps, Board1 bring-up and the 2026-08/09
  open-loop bench debugging (the only powered-motor evidence so far). The Board1
  pinout in `hardware-and-drv8311h.md` predates the new pinout (`d4aafff`); the
  firmware README is authoritative.
- `evidence/` — dated acceptance reports and raw evidence.
- `superpowers/plans/` and `superpowers/specs/` — dated plans and designs.
- `release/` — SDK 0.1.0-rc.1 release-preparation records.
- `pcb docs/` — Board1 schematics and datasheets.
