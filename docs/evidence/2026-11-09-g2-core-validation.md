# FalconFOC V2 G2 Core Validation

Date: 2026-11-09

## Scope and source identity

This record covers the G2 standalone Core only. The Legacy application, platform, board, root CMake assembly, FreeRTOS port, ADC path, PWM path, encoder path, and control ISR were not modified.

The validation was performed in the working tree at commit `1f542521189d5e0ef25e64e6530608ece8d6e137`. The Core is built through `core/CMakeLists.txt` as an independent C11 static library.

The Core source uses only `foc_core.h` and the standard C `math.h` implementation. A repository search found no MCU, RTOS, Shell, Flash, CMSIS-DSP, vendor SDK, or Legacy application include in `core/`.

## Implemented contract

The G2 API provides:

- Clarke, Park, and inverse Park transforms
- electrical-angle wrapping for negative and greater-than-one-turn angles
- centered common-mode SVPWM
- six-sector reporting, normalized duty output, vector limiting, and duty limiting
- voltage-open-loop stepping with explicit `dt_s`
- configuration, numeric, timing, and VBUS validation
- deterministic disabled output on failed stateful steps
- non-mutating transform and SVPWM inputs

The Core supports voltage open loop only. Current loops, observers, sensors, startup, communication, scheduling, safety supervision, and hardware integration remain outside G2.

The platform design's future `foc_set_command` and `foc_fast_tick` names are not implemented as a second stateful mailbox in G2. `foc_step` is the explicit host-test primitive for this slice. A later Runtime adapter can map H0/H1 snapshots into that primitive after command ownership and fast-loop timing contracts are implemented.

## Test inventory

`test_foc_math` verifies the Legacy Clarke/Park sign conventions, inverse-Park round trip, known Park values, negative and wrapped angles, null pointers, non-finite inputs, and input immutability.

`test_foc_svpwm` verifies the zero vector, alpha and beta axis expected duties, all six sectors, sector-boundary continuity, common-mode output expectations, high-vector saturation, configured duty bounds, invalid VBUS, non-finite vectors, null pointers, and input immutability.

`test_foc_core` verifies valid and invalid configurations, initialization and reset, explicit angle progression, angle wrapping, disabled output, deterministic replay, equal elapsed time across different legal `dt_s` values, complete safe output on errors, invalid `dt_s`, invalid VBUS, non-finite commands, unsupported modes, state preservation on errors, and null pointers.

Expected-value tolerances are `1e-5` for transform and duty values and `1e-6` for deterministic replay and zero-vector checks.

## Legacy comparison

The new Core uses the same verified physical conventions as Legacy.

- Clarke uses `alpha = phase_a` and `beta = (phase_b - phase_c) / sqrt(3)`.
- Park uses `d = alpha*cos(theta) + beta*sin(theta)` and `q = beta*cos(theta) - alpha*sin(theta)`.
- Inverse Park uses the corresponding Legacy inverse equations.
- SVPWM uses the same inverse Clarke equations, centered common-mode injection, normalized two-thirds vector envelope, and duty-envelope concept.

The Core is intentionally not linked to `applications/algo/foc_core.c`. The comparison is made with independent expected vectors. For a 12 V bus and a 2 V alpha-axis command, both conventions produce duties of approximately `0.625`, `0.375`, and `0.375` before any tighter configured duty clamp. The new API keeps the source vector read-only and makes the VBUS and limit policy explicit.

## Results

Standalone Core configure, build, and CTest completed with all three tests passing. The resulting library and executables are host artifacts only.

The existing Legacy host flow completed with `45/45` tests passing. The AT32F415 `fw-full-diag-carrier-bench-debug` configure and build completed successfully and reported the existing `cortex-m3-soft` CPU class. The repository `git diff --check` completed successfully.

The repository-wide quality gate passed its CMake quality, CMake preset, platform backend, and CMake smoke layers. Its firmware CLI contract layer could not run because this Windows environment has no Bash executable. That is an environment prerequisite failure in an existing repository script, not a Core test failure, and it remains explicitly open for a future gate rerun on a machine with Bash.

## G2 boundary

Passing this record establishes that the independent software Core can be compiled and behaviorally tested on the host. It does not establish WCET on AT32F415, ADC sampling correctness, PWM register correctness, encoder timing, watchdog capability, runtime scheduling, or G3-SAFE readiness.

No board was flashed, no debugger session was started, no UART command was sent, no PWM was enabled, and no powered motor test was performed for G2.
