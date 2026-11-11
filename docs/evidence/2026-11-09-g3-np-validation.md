# FalconFOC V2 G3-NP Validation

Date: 2026-11-09

## Scope

This record covers the no-power G3-01 Core/Device contract and G3-02 offline vertical-chain simulator. It does not modify the Legacy ADC/PWM ISR chain and does not connect to the root firmware build.

The implementation is under `g3/np/` and uses standard C plus the existing G2 Core source. No AT32 header, vendor SDK, FreeRTOS, Shell, DMA, ADC register, TMR register, debugger, serial command, board flash, PWM enable, power, or motor operation was used.

## Board1 facts represented

- ADC rank 1 is VBUS.
- ADC ranks 2, 3, and 4 carry the three current-sense channels.
- VBUS conversion is `raw * 3.3 / 4095 * 11`.
- Current conversion is `(raw - offset) * 3.3 / (4095 * 0.5)`.
- ABC labels ranks 2, 3, and 4 as phases A, B, and C.
- ACB swaps the B/C labels.
- ABC maps phase A/B/C duties to TMR1 CH3/CH2/CH1.
- ACB maps phase A/B/C duties to TMR1 CH3/CH1/CH2.
- Duty-to-compare conversion follows the existing truncating `duty * period_counts` rule.

These are source-derived Board1 facts. They are represented in a host contract and are not yet a production BSP implementation.

## Safety semantics exercised

The Device request distinguishes `INHIBIT`, `UPDATE`, `ZERO_DUTY`, and `ACTIVE_BRAKE`. The simulator's bridge gate is permanently closed. A valid Core output can update mock compare registers and produce an update request, while the mock bridge remains disabled. Zero-duty and active-brake requests are represented as separate actions and are not inferred from a zero compare value.

Fault priority is explicit in the simulator. A latched fault wins over state, sample, timing, Core, and output decisions. A disabled simulator state inhibits before any sample is consumed. Invalid, future, repeated, or stale samples inhibit without reusing old data. Invalid timing inhibits without committing a new Core angle. A disabled Core output remains an inhibit request and is not converted into a zero-duty update.

The current host contract treats sample sequence and timestamp values as monotonic within the simulated run. Counter wraparound policy is deliberately deferred to the production Device timing contract.

## Test inventory

`test_foc_device_contract` verifies ADC scale conversion, phase offsets, ABC and ACB rank mapping, ABC and ACB PWM channel mapping, compare conversion, explicit inhibit semantics, invalid sample rejection, and invalid output rejection with safe request fields.

`test_foc_device_sim` verifies the closed bridge gate, valid `Sample -> Core -> Output` flow, disabled-state inhibit, stale and invalid sample rejection, transient recovery, invalid timing, disabled Core command, latched fault priority, non-automatic fault recovery, and deterministic register snapshots.

## Results

The independent G3-NP CMake project was clean-built with Clang 22.1.8. Both CTest targets passed.

The root firmware build and Legacy host regression were not altered by this stage. G2 remains independently accepted in the local workspace with its environment-limited Bash quality-gate item documented separately.

The source-style and gate coverage review is recorded in `docs/evidence/2026-11-09-g2-g3-source-style-acceptance.md`. The dedicated `falconfoc` quality scope includes the new C/H, CMake, and README contract checks.

## Boundary

G3-NP proves only the host-side data contract and offline control-chain semantics. It does not prove ADC trigger freshness on AT32F415, sample-to-PWM latency, timer shadow-register behavior, actual bridge disable behavior, current polarity under the driver, hardware break protection, watchdog reset behavior, WCET, or powered motor safety.

G3-SAFE and G3-PWR remain blocked. The next review must decide whether this provisional contract is ready to be mapped into thin BSP/Device owners without touching the Legacy control path.
