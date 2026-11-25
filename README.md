# FalconFOC V2

FalconFOC V2 is a pure-C, C11-compatible FOC control and Runtime library with
an independent Host Virtual Motor and an AT32F415 reference target. The Core
does not require an RTOS, MCU headers, vendor SDK, shell, transport, or board
code. The Runtime adds scheduling-domain ownership, lifecycle, Safety, and an
explicit output-submission contract around that Core.

The earlier Themis/FreeRTOS Legacy firmware was removed on 2026-11-24 and is
kept only at the Git tag `legacy-final`. The repository is V2 only: `core/`,
`runtime/`, `g3/`, `sensors/`, `calibration/`, `control_plane/`, `protocol/`,
`simulation/`, `firmware/at32f415/`, `examples/` and `tools/`. Each of these
is a standalone CMake project; there is no root CMake project.

## V2 product map

- **FalconFOC Core** — Clarke/Park, SVPWM, Current PI, Velocity PI, Position P,
  Trajectory and impedance in portable C11.
- **FalconFOC Runtime** — H0/H1/Foreground execution domains, lifecycle,
  command and sensor publication, Safety, and output submission.
- **Reference targets** — Host Virtual Motor and the locked AT32F415 Board1
  reference firmware: UART V1, CANSimple and CAN MIT, on-MCU calibration (ADC
  zero, rotor, R/L, flux, PI, inertia, cogging, encoder linearization), motor and
  thermal protection, and persistent settings
  ([Board1 README](firmware/at32f415/README.md)).
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
- [Board1 firmware README](firmware/at32f415/README.md)
- [Parameter contract](docs/superpowers/specs/2026-11-22-falconfoc-v2-parameter-contract-design.md)
- [Docs index](docs/README.md)

## Current release status

The V2 software has a reproducible Host baseline, independent Core-consumer
evidence, virtual-device demonstrations, and staged research evidence. This is
an engineering preparation state. It is not a formal public release: the
repository has no selected root project license and the source provenance review
for bundled vendor/CMSIS material is incomplete.

## Verification

- Quality gate: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_falconfoc_quality_gate.ps1 -RepoRoot <repo> -RequireCMakeFormatter`
- Full integration (Host matrix, ARM images, static checks, SIL UART, gate): `bash scripts/flows/fwinteg/run_fwinteg.sh build/fwinteg-<name>/run1`
- Entry contract for AI and maintainers: [AGENTS.md](AGENTS.md); script map: [scripts/README.md](scripts/README.md)
