# FalconFOC Core

`core/` is the standalone control core for FalconFOC V2. It is a host-buildable C11 static library with no dependency on the Legacy firmware, MCU headers, vendor SDK, RTOS, shell, transport, flash, DMA, or board code.

## Scope

The G2 slice contains the voltage-open-loop path.

- amplitude-invariant Clarke transform
- Park and inverse Park transforms using the Legacy sign convention
- electrical-angle wrapping to `[0, 2π)`
- centered common-mode SVPWM with six-sector reporting
- configured voltage and duty limits
- explicit `dt_s` timing
- deterministic rejection of null, non-finite, invalid VBUS, invalid timing, and invalid configuration inputs
- disabled output with centered duties on every failed `foc_step`

NEXT-FOC-01 adds a separate d/q current-feedback controller. Sensor acquisition, calibration, communication, scheduling, and physical output supervision belong to the caller.

## Public contract

The public declarations are in `include/foc_core.h`. Physical quantities use SI units.

- voltage fields use volts
- current fields use amperes
- electrical angles use radians
- electrical speed uses radians per second
- timing uses seconds
- duty values are normalized to `[0, 1]`

`struct foc_validated_motion_command` is the single G2 command type. The `foc_validated_motion_command_t` alias is provided for documentation and integration code that uses a `_t` naming convention.

`foc_step` only supports `FOC_CONTROL_VOLTAGE_OPEN_LOOP`. A successful enabled step advances the electrical angle by `electrical_speed_rad_s * dt_s`, transforms the d/q voltage command, applies the configured voltage envelope, and returns a complete output snapshot.

The later Runtime API may expose command publication and a fast-tick adapter. G2 keeps those ownership and scheduling concerns out of the Core and uses `foc_step` as its explicit, host-testable entry point.

## Optional trajectory planner

`include/foc_trajectory.h` provides a small Float32 reference generator for
the existing velocity PI loop. It can limit a velocity target by
`max_accel_rad_s2 * dt_s` and `max_velocity_rad_s`, or generate a point-to-point
velocity target from continuous mechanical position using
`sqrt(2 * max_accel * abs(target - measured))`. It never wraps multi-turn
mechanical position and it does not own sensors, clocks, Safety, or output.

The context is caller-owned and copyable for a trial. Runtime commits the trial
only after the corresponding downstream UPDATE is accepted. Invalid input
clears the output and leaves the planned velocity unchanged. The planner is
optional; existing Runtime configurations leave it disabled.

The minimum caller sequence is:

```c
struct foc_context context;
struct foc_output output;

foc_init(&context, &config);
foc_step(&context, &command, &measurement, &timing, &output);
if (output.output_enable != 0U) {
    /* A later Device layer owns safety checks and hardware mapping. */
}
```

## Error and safety behavior

Every public operation validates pointers before dereferencing them. Transform and SVPWM functions calculate into local values and publish the result only after validation, so their input structures are not modified.

`foc_step` clears the output to a centered, disabled state before validation. It commits the new angle only after the complete inverse-Park and SVPWM path succeeds. Invalid `dt_s`, VBUS, command values, mode, or configuration leave the context angle unchanged.

The configured `voltage_limit_v` is applied before SVPWM. SVPWM also enforces the normalized two-thirds vector envelope and clamps each final duty to `duty_min` and `duty_max`. The `saturated` flag records vector or duty limiting.

## d/q current loop

`include/foc_current.h` defines `foc_current_init`, `foc_current_reset`, and `foc_current_step`. Zero-initialize a context before use. Each instance keeps its own settings and two voltage integrators. Supply signed, coherent phase currents in the Core phase convention, the electrical angle at the current sample instant, the measured bus voltage, and the actual elapsed `dt_s`.

```c
struct foc_current_context current = {0};
struct foc_current_output output;

int status = foc_current_init(&current, &current_config);
if (status == FOC_STATUS_OK) {
    status = foc_current_step(&current, &sample_and_target, &output);
    if (status == FOC_STATUS_OK && output.output_enable != 0U) {
        /* Device and Safety evaluate output.svpwm for a normal update. */
    } else {
        /* Request Device/Safety INHIBIT. */
    }
}
```

Clarke and Park produce the feedback current. The target is limited as a d/q vector before calculating the error on each axis. The PI uses `I_trial = I_previous + Ki * dt_s * error` and `V_trial = Kp * error + I_trial`. Gains are in V/A and V/(A·s). The voltage request is limited as a vector to the smaller of the configured voltage limit and `VBUS / sqrt(3)`, the circular envelope available at any electrical angle with unrestricted duties. The existing SVPWM duty bounds can reduce the applied voltage further. For current control those bounds must straddle `0.5`.

The controller reconstructs the ideal equivalent voltage from the final duties and VBUS. On saturation it admits only the portion of each trial integral increment consistent with that voltage and permits increments that unwind the saturation. It recomputes modulation once when the admitted increment changes. The output contains the measured d/q current, reconstructed voltage, final duties, and limit flags.

Failed steps clear the entire output and leave the context unchanged. A cleared duty value is a placeholder. The downstream Device/Safety layer owns `INHIBIT`. A caller can step a value copy of the context and commit it only after the final output guard accepts the result. Reset clears both integrators while retaining settings.

## Sensored motion loops

`include/foc_motion.h` provides mechanical-to-electrical conversion, bounded angle prediction, velocity PI, and stateless position P. Supply continuous mechanical position in rad and coherent speed in rad/s. The producer owns the mechanical zero, direction normalization, and any single-turn unwrapping. The electrical angle is `wrap(pole_pairs * mechanical_position + electrical_offset)` and electrical speed is `pole_pairs * mechanical_speed`. The caller aligns encoder sign and phase polarity so positive q current produces positive mechanical torque. Position error keeps all mechanical turns. Only electrical phase is normalized.

`foc_rotor_predict` takes an already validated forward interval in microseconds. It limits both the interval and the extrapolation contribution `0.5 * max_electrical_acceleration * dt_s^2`. Source-angle and speed uncertainty belong to the caller's sensor quality contract. `foc_velocity_step` uses actual `dt_s`, admits only the supported integral increment, and bounds the integral when the q current limit drops. A disabled step freezes state. Position P limits its velocity reference and has no state to reset. A new motion session resets the velocity PI.

## Optional current feedforward

`foc_current_feedforward_init()` validates an optional PMSM model in SI units:
`Ld` and `Lq` in H, and `psi_f` in Wb. The enabled equations are
`vd_ff = -omega_e * Lq * iq_target` and
`vq_ff = omega_e * (Ld * id_target + psi_f)`. The feedforward voltage is added
before the existing SVPWM limit and voltage reconstruction; anti-windup admits
the integral against the PI proportional term plus feedforward, so feedforward
voltage is not incorrectly credited to the PI integrator.

The ordinary `foc_current_step()` path remains unchanged when feedforward is
not configured. `foc_current_step_with_feedforward()` requires the caller to
provide a finite electrical speed valid at the same control instant. This Core
API does not synthesize speed from an angle and does not wire the optional path
into Board1 or Runtime automatically.

## Host build and tests

From the repository root:

```text
cmake -S core -B build/core-g2 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/core-g2 --parallel
ctest --test-dir build/core-g2 --output-on-failure
```

Five test executables cover transforms, SVPWM, voltage-open-loop stepping, d/q current feedback, and the motion primitives. The current test uses a fixed-rotor R/L model driven by final duties. The Runtime test project exercises the cascaded controller with an electrical and mechanical PMSM model.

## Integration boundary

The G2 context owns voltage-open-loop configuration and electrical angle. The separate current context owns its configuration and two integrators. An adapter owns sample timing, coherent snapshots, and hardware output submission. Host validation covers the software boundary. Target timing and physical output authorization are separate acceptance steps.
