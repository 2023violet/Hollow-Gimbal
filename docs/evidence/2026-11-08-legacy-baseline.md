# Legacy Baseline

- Baseline date: 2026-11-08 America/Los_Angeles
- Scope: G0 read-only baseline freeze
- Powered-board action: none
- Debugger or flash action: none
- UART command action: none

## Repository identity

- Repository: `D:/study/Software/Hollow-Gimbal`
- Branch: `main`
- Baseline `HEAD`: `1f542521189d5e0ef25e64e6530608ece8d6e137`
- `origin/main` at audit start: `a03ee4b758ab0c14de4d93a88ca6e0623aefccbd`
- Working tree at audit start contained proposal-document changes and untracked G0/G1 plan and Runtime ADR files. No Legacy source, CMake, generated platform, or build configuration was changed by G0.

## Toolchain and build commands

Host tools observed:

- CMake `4.4.2`
- Ninja `1.13.2`
- Arm GNU Toolchain `14.3.Rel1`, `arm-none-eabi-gcc 14.3.1`

Commands executed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
cmake --preset fw-full-diag-carrier-bench-debug
cmake --build --preset build-fw-full-diag-carrier-bench-debug --parallel
```

No `run.sh flash`, OpenOCD, GDB, serial command, PWM enable, or motor command was run.

## Host test result

- Flow: `host-debug -> build-host-debug -> test-host-debug`
- Result: `45/45 PASS`
- CTest total time on this audit: `2.88 sec`
- Host build had no source work after configuration. Ninja reported no work to do.

This result covers host contracts and models. It does not prove physical PWM, independent phase current, temperature, driver protection, or continuous motor acceptance.

## Firmware artifact identity

Preset:

- `fw-full-diag-carrier-bench-debug`
- Platform: `at/at32f415`
- CPU class: `cortex-m3-soft`
- Linker: `AT32F415xB_FLASH.ld`

Artifacts:

| Artifact | Size |
| --- | ---: |
| `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.bin` | 92,144 bytes |
| `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf` | 1,476,344 bytes |
| `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.hex` | 259,262 bytes |
| `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.map` | 1,564,366 bytes |

ELF SHA-256:

```text
7E061224F3FA3D06A0CF0E2B91892FDEE5B0111B939CCC8228BB74B947041B12
```

Previous build output reported by the build system:

- FLASH: 92,144 bytes / 128 KiB, 70.30 percent
- RAM: 27 KiB / 32 KiB, 84.38 percent

## Current platform and board facts

- Live platform owner: `platform/at/at32f415`
- Current Legacy CPU class: `cortex-m3-soft`
- Current FreeRTOS port family: `portable/GCC/ARM_CM3`
- Current external control witness: USART1, 115200 8N1, Board1 UART path
- Current FOC platform target: `at32f415-hal`
- Current linker memory: 128 KiB Flash, 32 KiB RAM
- Current generated timer chain: TMR1, TMR3, TMR5
- TMR2 is not in the current generated startup chain

The current CPU class and FreeRTOS port remain Legacy facts. G0 does not classify them as defects and does not change them.

## Historical real-board evidence

The repository records historical bench evidence for:

- ADC trigger dependence and sampling-window behavior
- AVDD supply collapse and later board-level repair evidence
- raw PWM and I-f isolation runs
- measured injected ADC frequency near 13.35 kHz
- measured bench FOC step near 3.34 kHz while the compile-time bench model used 5 kHz
- encoder calibration and closed-loop handoff attempts
- current software limits and G0/G1 safety boundaries

These records are historical evidence tied to particular firmware, hardware, power, motor, and parameter identities. They are not current remeasurements and do not constitute product-level continuous-run acceptance.

## Known stale or invalid checks

- Some boot and governance documents still contain historical AT32F403A, J-Link, or COM54 references.
- `tests/script/test_falconfoc_themis_lite_contract.ps1` still references retired AT32F403A paths and missing timing artifacts.
- `scripts/flows/host/check_foc_peripheral_contract.ps1` still expects the retired hardware ADC trigger and `CCR4=2700`, while the current source uses software trigger and `CCR4=3740`.
- The quality gate cannot complete in the current PowerShell environment when its Bash-dependent firmware CLI contract is reached.

These are migration or tooling issues. They are recorded here and not repaired during G0.

## Evidence limits

The following are established by this audit:

- Current host contracts pass 45/45.
- Current AT32F415 bench firmware configures and builds with the existing Legacy configuration.
- The current build artifacts are identified by path, size, and ELF SHA-256.
- No physical board action was performed.

The following remain unproved by this audit:

- Actual ADC/PWM/control timing on the powered board
- Independent phase current
- PWM, DRV input, and phase output waveforms
- Hardware break or driver fault visibility
- Thermal behavior
- Continuous motor operation
- UART Protocol v1 behavior, because it is not implemented yet
- G3-SAFE readiness for future powered V2 tests

## G0 exit decision

G0 baseline capture is complete for the local host and firmware lanes. G1 remains required for timer/DMA ownership, timing/blocking analysis, UART continuous-stream behavior, H0/H1 state ownership, and G3-SAFE acceptance.
