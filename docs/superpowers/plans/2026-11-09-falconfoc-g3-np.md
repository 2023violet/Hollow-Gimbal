# FalconFOC V2 G3-NP Interface and Offline Chain Plan

> For agentic workers: use the execution workflow task-by-task after this plan is accepted. This plan is limited to no-power source contracts and host simulation.

**Goal:** Define and verify a thin Core/Device boundary for Board1 facts and build an offline `Sample -> Core -> Output` vertical-chain simulator without modifying the Legacy ISR, ADC/PWM runtime, root firmware CMake, or any board state.

**Architecture:** The pure G2 Core remains unchanged as the mathematical owner. A new `g3/np/` host-only project owns the provisional Device mapping and mock register simulator. The Device layer converts raw ADC rank data into physical measurements and converts Core phase duties into Board1 TMR1 channel compare requests. The simulator enforces sample freshness, explicit timing, output action semantics, and a closed power gate.

**Board1 facts represented by tests:** ADC rank 1 is VBUS, ranks 2 through 4 are the three phase-current sense channels. The current Legacy mapping labels rank 2 as phase A, rank 3 as phase B, and rank 4 as phase C for `PHASE_ORDER_ABC`, with B/C swapped for `PHASE_ORDER_ACB`. VBUS uses `raw * 3.3 / 4095 * 11`. Current uses `(raw - offset) * 3.3 / (4095 * 0.5)`. Legacy PWM maps phase A to TMR1 CH3, phase B to CH2, and phase C to CH1 for ABC. ACB swaps B and C while A remains CH3.

## Hard boundaries

- Do not modify `applications/`, `platform/`, `bsp/`, `subsys/`, `components/`, the root `CMakeLists.txt`, Legacy tests, or generated firmware sources.
- Do not include AT32 headers, FreeRTOS, Shell, DMA, ADC registers, TMR registers, or vendor SDK code in `g3/np/`.
- Do not call `ADC1_IRQHandler`, `TMR1_CH_IRQHandler`, `adc_read_phases`, `pwm_out_set_duty`, `pwm_out_enable`, or `pwm_out_disable`.
- Do not flash, debug, send UART commands, enable PWM, power the board, or run a motor.
- Keep the mock bridge gate closed even when the simulator produces a valid PWM request.
- Treat the Device mapping as a provisional G3 contract. Production BSP ownership and G3-SAFE policy remain later decisions.

## G3-01 contract

Create `g3/np/include/foc_device.h` and `g3/np/src/foc_device.c` with:

- raw ADC sample containing VBUS, rank 2, rank 3, rank 4, sample sequence, sample timestamp, and validity bits
- ADC conversion configuration containing scale factors, phase-current offsets, and phase order
- decoded measurement containing VBUS, phase currents, sample sequence, and sample timestamp
- PWM configuration containing timer period counts and phase order
- PWM request containing an explicit action, normalized phase duties, and mapped CH1/CH2/CH3 compare values
- `FOC_DEVICE_OUTPUT_INHIBIT`, `FOC_DEVICE_OUTPUT_UPDATE`, `FOC_DEVICE_OUTPUT_ZERO_DUTY`, and `FOC_DEVICE_OUTPUT_ACTIVE_BRAKE` as distinct actions
- validation that rejects invalid configuration, missing samples, non-finite scales, invalid phase order, and out-of-range duties
- deterministic raw-to-physical conversion with no mutation of raw input
- deterministic phase-to-timer mapping for ABC and ACB

The Device contract must never infer bridge enable from a duty value. An inhibit request remains an inhibit request even if its safe compare values are zero. Zero-duty and active-brake requests remain explicit actions whose hardware behavior is deferred to the later BSP safety contract.

## G3-02 offline chain

Create `g3/np/include/foc_device_sim.h` and `g3/np/src/foc_device_sim.c` with:

- a simulator context containing a G2 Core context, Device configuration, run state, last accepted sample sequence, fault bits, and mock PWM registers
- a step input containing a validated G2 command, raw ADC sample, current timestamp, and explicit `dt_s`
- a step result containing status, fault bits, Core output, Device PWM request, and mock register snapshot
- explicit states for disabled, running, and fault-latched operation
- sample freshness checks based on strictly increasing sequence and bounded timestamp age
- fault-priority order where disabled or latched inhibit wins before sample, timing, Core, or output decisions
- Core error conversion to an inhibit request without committing Core state
- a closed mock bridge gate that records requested compare writes but never reports physical bridge enable

The simulator must test the following paths.

1. Valid raw sample and running state produce a valid Core output request and correct mapped compare values while the mock bridge remains disabled.
2. ABC and ACB phase mappings produce the expected channel ordering.
3. Missing, invalid, repeated, and stale samples produce inhibit requests and do not reuse stale data.
4. Invalid `dt_s` produces inhibit and preserves the Core angle.
5. Core-disabled command produces inhibit and does not become zero-duty drive.
6. A latched fault overrides an otherwise valid sample and command.
7. A valid sequence after a transient invalid sample can recover only when the simulator is not latched.
8. Every simulator result has finite, deterministic fields and no vendor or hardware dependency.

## Host build

`g3/np/CMakeLists.txt` is an independent C11 project. It links the existing G2 Core source, the new Device mapping, and the simulator. It does not modify or become a subdirectory of the root firmware build.

## Exit criteria

- G2 local source acceptance record exists and all G2 tests pass.
- G3-01 conversion and mapping tests pass with fixed Board1 facts.
- G3-02 offline chain tests pass with closed bridge gate.
- Invalid and stale sample behavior is observable and deterministic.
- Core state is unchanged on failed chain steps.
- No Legacy source, firmware CMake, generated platform source, or board behavior changes exist.
- No powered-board operation occurs.

## Deliberately deferred

- production `bsp/board` or `bsp/ports` implementation
- actual ADC/PWM register access
- ADC trigger ownership changes
- ISR integration
- H0/H1 mailbox and scheduler integration
- watchdog selection and G3-SAFE latch policy
- zero-duty, active-brake, and bridge-disable hardware semantics beyond the explicit host action contract
- any G3-PWR or powered motor test
