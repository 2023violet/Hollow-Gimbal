# FalconFOC V2 G2 Minimal Core Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task after plan approval. Steps use checkbox syntax for tracking.

**Goal:** Build and verify an independently compilable FalconFOC Core that provides validated voltage-open-loop control, Clarke/Park transforms, inverse Park, SVPWM, explicit timing, numeric rejection, and host tests without touching Legacy firmware.

**Architecture:** The new Core lives under core/ as a standalone CMake project. Public data structures are plain C types. Pure math functions are separate from stateful voltage-open-loop stepping. The Core receives a foc_validated_motion_command_t, measurements, and explicit timing, then returns a complete output snapshot and status. Legacy code is never linked into the Core target.

**Tech Stack:** C11, CMake, CTest, standard math.h, host compiler, no CMSIS-DSP, no FreeRTOS, no vendor SDK.

**Spec:** docs/superpowers/specs/2026-11-07-falconfoc-v2-platform-design.md and docs/superpowers/specs/2026-11-08-falconfoc-v2-runtime-adr-r1.md

## Global Constraints

- Do not modify Legacy applications, platform, bsp, subsys, components, root CMakeLists.txt, or Legacy tests.
- Do not link or include Legacy foc_core.c, foc_math.h, foc_config.h, CMSIS-DSP, FreeRTOS, Shell, Flash, or MCU headers.
- Use explicit SI units in public fields: volts, amps, radians, radians/second, seconds.
- Core must return deterministic error status and safe output for invalid inputs.
- G2 supports voltage open loop only. No current loop, observer, encoder, Hall, calibration, protocol, transport, or board code.
- Output computation must not mutate input vectors or command objects.
- No dynamic memory.
- No powered-board, debugger, flash, UART command, PWM enable, or motor action.
- Legacy host tests and firmware build must remain passing after G2.

## Public Interfaces

The public header core/include/foc_core.h defines plain-C types for:

- foc_status
- foc_control_mode
- foc_vector_abc
- foc_vector_alpha_beta
- foc_vector_dq
- foc_svpwm_output
- foc_config
- foc_validated_motion_command
- foc_measurement
- foc_timing
- foc_output
- foc_context

The public functions are:

- foc_config_validate
- foc_clarke
- foc_park
- foc_inverse_park
- foc_svpwm
- foc_init
- foc_reset
- foc_step

The exact signatures are fixed in the public header before implementation. foc_step receives a validated motion command, measurement, and timing, and returns a complete output snapshot.

## Review Focus

- Invalid numeric input. Every public function rejects NaN and infinity without producing a non-finite output.
- Invalid timing. Zero, negative, non-finite, and over-limit dt return ERR_INVALID_DT and disable output.
- Invalid bus voltage. Zero, negative, non-finite, and below-minimum VBUS return ERR_INVALID_VBUS and disable output.
- Input mutation. Transform and SVPWM functions leave input structs unchanged.
- Saturation. High voltage vectors produce bounded duty values and set a saturation flag.
- Disabled mode. Disabled or invalid command returns a safe disabled output with deterministic duty values.

---

### Task 1: Create standalone Core build and red tests

Files:

- Create core/CMakeLists.txt
- Create core/include/foc_core.h
- Create core/src/foc_core.c
- Create core/tests/test_foc_core.c
- Create core/tests/test_foc_math.c
- Create core/tests/test_foc_svpwm.c

Steps:

- [x] Write a self-contained CMake project with static library target falconfoc_core, three host test executables, enable_testing, C11, and warning flags. Write tests for the Review Focus behaviors. Leave production functions absent or returning failure for the RED stage.
- [x] Run cmake -S core -B build/core-g2 -G Ninja -DCMAKE_BUILD_TYPE=Debug, cmake --build build/core-g2 --parallel, and ctest --test-dir build/core-g2 --output-on-failure. Expected: tests fail because behavior is missing, not because of a test typo.
- [x] Add the exact public types and declarations. Keep implementation functions returning documented failure until later tasks.
- [x] Re-run the same commands. Expected: tests compile and fail on behavioral assertions.

---

### Task 2: Implement pure transforms and angle handling

Files:

- Modify core/src/foc_core.c
- Test core/tests/test_foc_math.c

Steps:

- [x] Test Clarke, Park, inverse Park, angle cases including negative and greater-than-2pi input, null pointers, non-finite inputs, and input immutability. Confirm RED.
- [x] Implement amplitude-invariant Clarke with alpha equal to a and beta equal to (b-c)/sqrt(3). Implement the existing Park sign convention and the inverse. Use standard sinf/cosf and a private wrap-to-0-2pi helper. Reject non-finite inputs before calculating.
- [x] Run ctest -R test_foc_math and confirm PASS.
- [x] Run the entire standalone Core suite and confirm only unimplemented targets remain RED.

---

### Task 3: Implement SVPWM and limits

Files:

- Modify core/src/foc_core.c
- Test core/tests/test_foc_svpwm.c

Steps:

- [x] Test zero vector, alpha and beta axes, all six sectors, high-vector saturation, duty bounds, invalid VBUS, non-finite vectors, and input immutability. Confirm RED.
- [x] Implement inverse Clarke plus centered common-mode injection. Normalize against VBUS, scale vectors above the configured voltage envelope, set saturated, and clamp final duty to duty_min and duty_max. Do not mutate input.
- [x] Run ctest -R test_foc_svpwm and confirm PASS.
- [x] Run the complete standalone Core suite and confirm math and SVPWM targets pass.

---

### Task 4: Implement configuration validation and voltage-open-loop stepping

Files:

- Modify core/src/foc_core.c
- Test core/tests/test_foc_core.c

Steps:

- [x] Test valid and invalid config, disabled command, angle progression by electrical_speed_rad_s times dt_s, angle wrapping, valid VBUS and dt, invalid dt, invalid VBUS, non-finite commands, deterministic replay, and invalid-step state preservation. Confirm RED.
- [x] Validate finite config values, positive vbus_min_v, positive voltage_limit_v, duty bounds, duty_min less than duty_max, and positive max_dt_s. Initialize angle to zero and outputs to disabled centered values.
- [x] Implement foc_step. Validate pointers and finite values, validate dt and VBUS, handle disabled mode without advancing angle, support only voltage-open-loop mode, advance and wrap angle, clamp d/q voltage, run inverse Park and SVPWM, and return deterministic disabled output on every error.
- [x] Run ctest -R test_foc_core and confirm PASS.
- [x] Run ctest --test-dir build/core-g2 --output-on-failure and confirm all standalone Core tests pass.

---

### Task 5: Add Legacy comparison vectors and validation record

Files:

- Modify core/tests/test_foc_math.c
- Modify core/tests/test_foc_svpwm.c
- Create core/README.md
- Create docs/evidence/2026-11-09-g2-core-validation.md

Steps:

- [x] Add independent expected-value vectors for transforms, SVPWM axes, saturation, angle progression, and invalid inputs.
- [x] Add explicit float tolerances and record differences caused by non-mutating API, input validation, or limit policy. Do not link Legacy implementation into Core tests.
- [x] Write core/README.md covering scope, API, units, errors, build, tests, no-dependency rule, G2 non-goals, and future Device/Port boundary.
- [x] Write docs/evidence/2026-11-09-g2-core-validation.md with source identity, API, test inventory, tolerances, Legacy comparison, negative behavior, regressions, G2 limits, and G3 blockers.
- [x] Run Legacy host tests and the Legacy bench firmware build. Expected: 45/45 PASS and firmware build PASS.
- [x] Run standalone Core configure, build, CTest, and git diff --check. Expected: Core tests pass and no Legacy source, platform, root CMake, or board behavior changes are present.

## G2 Exit Criteria

- Core builds as an independent static library.
- Core tests pass independently.
- Core uses standard C and math.h only.
- Core has no MCU, RTOS, Shell, Flash, CMSIS-DSP, or board dependency.
- Clarke, Park, inverse Park, angle wrapping, SVPWM, voltage open loop, explicit dt, and numeric rejection are tested.
- Invalid dt, VBUS, NaN, Inf, and out-of-range configuration produce deterministic safe output.
- Input structures are not mutated.
- Legacy host tests remain 45/45 PASS.
- Legacy bench firmware still builds.
- No PWM, flash, debugger, UART command, or powered-board action occurred.
- G3 remains blocked until G3-SAFE is separately reviewed and implemented.

## Handoff to G3

G3 may begin only after the G2 validation record is reviewed. G3 integrates the Core with the AT32F415 port through an ADC-to-Core-to-PWM vertical slice. G3 cannot reuse Legacy motor_dev as the V2 Core context and cannot enter powered testing before G3-SAFE passes.
