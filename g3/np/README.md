# FalconFOC V2 G3-NP

`g3/np/` is a host-only, no-power validation slice for the first Core/Device boundary. It is intentionally separate from the root firmware build and does not call the Legacy ADC ISR, PWM helpers, generated platform code, vendor headers, FreeRTOS, or any hardware register.

## G3-01 Device contract

The Device layer in `include/foc_device.h` and `src/foc_device.c` represents Board1 facts as data and pure functions.

- ADC rank 1 is VBUS.
- ADC ranks 2 through 4 are the three phase-current sense values.
- VBUS conversion uses the Board1 divider scale `3.3 / 4095 * 11`.
- Current conversion uses `3.3 / (4095 * 0.5)` after the physical phase offset.
- ABC maps ranks 2, 3, 4 to physical phases A, B, C.
- ACB keeps A on rank 2 and swaps B and C.
- ABC maps phase duties A, B, C to TMR1 CH3, CH2, CH1.
- ACB keeps A on CH3 and swaps B and C.

The PWM request has an explicit action. `FOC_DEVICE_OUTPUT_INHIBIT`, `FOC_DEVICE_OUTPUT_UPDATE`, `FOC_DEVICE_OUTPUT_ZERO_DUTY`, and `FOC_DEVICE_OUTPUT_ACTIVE_BRAKE` are distinct. G3-NP can construct all four request semantics, while the actual zero-duty and active-brake hardware behavior remains deferred to the Device/BSP safety contract. Zero compare values in an inhibit request are mock metadata for the closed bridge gate. They do not mean zero-duty drive or active braking.

## G3-02 offline chain

The simulator executes the following host-only chain.

```text
raw ADC sample
    -> sample validity and freshness checks
    -> Board1 physical conversion
    -> G2 foc_step
    -> phase-to-timer mapping
    -> mock compare registers
```

The mock bridge gate is permanently closed. A valid update can produce a PWM request and mapped compare values, while the mock register state still reports `bridge_enabled = 0`.

The minimum simulator sequence is:

```c
foc_device_sim_init(&sim, &config);
foc_device_sim_set_running(&sim);
foc_device_sim_step(&sim, &input, &result);
```

The caller owns `sim`, `input`, and `result` storage. A successful result is still a host request because the closed mock gate prevents physical bridge enable.

The simulator checks disabled state, latched fault priority, missing and stale samples, invalid timing, Core failure, disabled Core output, transient recovery, and ABC/ACB mappings. A failed step inhibits the request and keeps the Core angle unchanged.

## Build and test

From the repository root:

```text
cmake -S g3/np -B build/g3-np -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/g3-np --clean-first --parallel
ctest --test-dir build/g3-np --output-on-failure
```

This project is not added to the root firmware CMake graph. Production BSP ownership, ISR integration, ADC trigger changes, watchdog policy, G3-SAFE, and powered tests remain deferred.
