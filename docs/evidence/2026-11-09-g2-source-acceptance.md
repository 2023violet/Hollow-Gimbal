# FalconFOC V2 G2 Independent Source Acceptance

Date: 2026-11-09

## Acceptance scope

This review examined the local working-tree versions of `core/include/foc_core.h`, `core/src/foc_core.c`, all three Core test files, `core/CMakeLists.txt`, `core/README.md`, and the G2 validation record. The review is based on commit `1f542521189d5e0ef25e64e6530608ece8d6e137` plus the uncommitted `core/` and evidence files in the shared workspace. It does not rely on the older GitHub `main` snapshot.

No source under `applications/`, `platform/`, `bsp/`, `subsys/`, `components/`, the root `CMakeLists.txt`, or the Legacy test tree was changed by G2.

## Review findings

### Public contract

- The public header contains only plain C data structures and status-returning functions.
- Units are explicit in field names for volts, radians, radians per second, and seconds.
- The G2 entry point is `foc_step`. It receives command, measurement, timing, and output snapshots without introducing a command mailbox or Runtime ownership model.
- The context owns only configuration and electrical angle state.

### Numerical and transform behavior

- Clarke, Park, and inverse Park use local result values and publish only after input and result validation.
- Angles are finite-checked and wrapped before trigonometric evaluation.
- Transform arithmetic that overflows to a non-finite result is rejected without publishing a partial result.
- SVPWM validates configuration, VBUS, vector arithmetic, and final duty finiteness before publishing.

### SVPWM behavior

- Inverse Clarke and centered common-mode injection match the Legacy physical convention.
- The normalized vector envelope is limited to the configured voltage limit and the two-thirds SVPWM envelope.
- Duty limits are applied after common-mode injection and set the saturation flag when they alter a result.
- Sector reporting is based on the input vector direction and covers all six sectors.
- The tests cover axis expected values, all sector midpoints, boundary continuity on both sides of each sector boundary, finite outputs, input immutability, duty bounds, and high-vector saturation.

### Stateful failure behavior

- `foc_step` initializes a complete centered disabled output before validation.
- Invalid pointers, configuration, command values, timing, VBUS, unsupported mode, and arithmetic overflow cannot commit a new electrical angle.
- Voltage-vector limiting is recorded in the output saturation flag.
- Disabled commands return a complete safe output without advancing the angle.
- Tests verify all safe-output fields and finite values on each exercised failure path.

### Time behavior

- `dt_s` is supplied by the caller and checked against the configured maximum.
- Angle integration uses `electrical_speed_rad_s * dt_s` and wraps after each successful step.
- Tests cover zero and over-limit periods, one-turn wrapping, deterministic replay, and equal elapsed time represented by different legal periods.

## Verification

The standalone Core was clean-built with Clang 22.1.8 through the independent CMake project. All three Core CTest targets passed after the additional acceptance assertions.

The existing Legacy host flow remains `45/45 PASS`. The AT32F415 bench firmware continues to configure and build with `cortex-m3-soft`. `git diff --check` passes for tracked changes and a separate scan found no trailing whitespace in new files.

The repository-wide quality gate remains partially open because its existing firmware CLI contract script requires a Bash executable that is absent from the current Windows environment. CMake quality, preset, platform backend, and CMake smoke checks pass.

The separate source-style acceptance record is `docs/evidence/2026-11-09-g2-g3-source-style-acceptance.md`. It records the file-header, public-contract, EOF, formatter coverage, CMake target, and governance-entry remediation for both G2 and G3-NP.

## Decision

Independent G2 source acceptance is **PASS with an environment-limited repository gate**.

This decision certifies the local source and evidence snapshot. It does not certify G3 Device mapping, AT32F415 timing, ADC freshness, PWM register behavior, watchdog behavior, or any powered-board operation. G3-NP remains the next authorized scope. G3-PWR and G3-SAFE remain blocked until separately accepted.
