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

## Periodic electrical phase

`foc_rotor_phase_from_counts()` in `include/foc_motion.h` converts a directed
integer mechanical cycle count to electrical phase. Supply the source Sensor's
CPR in `[2,65536]`, count in `[0,CPR)`, nonzero uint16 pole pairs, and finite
electrical offset. The count/poles product fits uint32 and is reduced modulo
CPR before Float32 scaling; direction is already applied by the Sensor.
Offset is applied once, from the controller's immutable rotor configuration.

The helper clears its output on error and grants no Sensor validity or output
authority. Large finite offsets retain `foc_angle_normalize()`'s documented
precision limits. The independent Host phase test uses a `1e-5 rad` circular
error budget over its declared CPR/poles/count/offset matrix; this is not a
physical encoder or MCU accuracy guarantee.

Keep continuous mechanical position for Position, Trajectory and telemetry.
Do not recover periodic phase from a large Float32 accumulated position. The
legacy `foc_rotor_electrical()` API remains available with its existing behavior
and the same large-input precision limitations.

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

## Single-axis mechanical impedance

`include/foc_impedance.h` exposes the stateless `foc_impedance_step()` for
motor-shaft requests. Supply signed, continuous reference-minus-measurement
position error computed in a common coordinate system before Float32 rounding.
The calculation accepts multi-turn errors as supplied. Reference and measured
speed are in rad/s. Stiffness is in N*m/rad, damping in N*m*s/rad, and
feedforward and torque requests in N*m.

Supply positive motor-shaft `Kt` in N*m/A and the live q current limit in A
on every call. With the Core phase convention, positive q current produces
positive motor torque. The ideal PMSM model at `Id = 0` uses
`Kt = 1.5 * pole_pairs * flux_linkage_wb`. R/L candidates do not determine Kt.
The tighter torque or current limit determines the torque cap. Both caps allow
zero, which gives zero torque and current. The final q current is capped again
after Float32 conversion to enforce the original live current bound.
`limited_torque_nm` is the torque request before q conversion. Multiplying
the final Float32 q target by `Kt` can differ slightly due to rounding and the
final current clamp.

Use the return status and `output.valid` before handing the request downstream.
Errors clear the output. The caller handles sample freshness and downstream
Current, voltage, PWM and Safety admission. Run the standalone
`test-foc-impedance` and Simulation's `test-foc-impedance-virtual` through CTest.
The latter exercises the existing Runtime Current entry with an external Host
controller and has a `--trace` option for per-sample CSV records.

### Model-based gain candidates

`foc_impedance_tuning_design()` in `include/foc_impedance_tuning.h` computes
`K = J * omega_n^2` and `D = 2 * zeta * J * omega_n - B` for an unsaturated
motor-shaft model. Supply a nonzero model identity, qualified inertia and Kt,
explicit viscous damping or the assumed-zero marker, target natural angular
frequency and damping ratio, configured gain ceilings, and current and timing
budgets. The API rejects negative D, gains beyond the supplied ceilings,
nonrepresentable arithmetic, and requested or predicted
`omega_n * (period + extra_delay) > 0.25`.

On success, `valid` covers the ideal model design and static gain bounds. The
ideal frequencies are predicted from the published Float32 gains. The reported
`Kt * Iq_limit` is a current-derived torque budget. Runtime still owns its
independent torque cap and all command admission. Sampled stability, noise
limits, and saturated behavior need closed-loop validation. Review a candidate
before explicitly transferring the gains to an impedance command. The tuning
function has no Runtime side effects and is intended for offline or cold-path use.

Use `test-foc-impedance-tuning` for the standalone numerical contract.
The Simulation test `test-foc-impedance-robustness` runs the frozen holdout
and reports separate per-scenario quality verdicts. Use `--train` for the
design matrix and `--trace` to export individual samples.

### Static actuator feasibility

The cold-path `foc_impedance_feasibility_evaluate()` in
`include/foc_impedance_feasibility.h` uses the same motor-axis impedance
configuration. Supply the admitted q-current limit, qualified Kt and signed
net static load identities, feedforward, and the allowed position error.
Confirm a fixed target, accounted static support torques, and qualified
current tracking. Unknown provenance returns UNKNOWN. A moving target has
its own verdict. Neither case produces a torque budget or position estimate.

The symmetric capacity is `min(torque_limit, Kt * Iq_limit)`. Capacity and
signed reserve are evaluated and returned in Double so a Float32 round-up
cannot create a feasible balance. For an interior load and positive K, the
equilibrium error is `(load - feedforward) / K`. At the capacity boundary,
saturation may allow a range of equilibrium errors with zero reserve. The
result gives its nearest error and marks position as unconstrained. With
K=0, limited feedforward must balance the load, and the model gives no
position-restoring term. D is validated as a controller setting and has zero
contribution at rest.

This static model does not certify convergence, dynamic stability, trajectory
tracking, or hardware safety. The diagnostic has no Runtime side effects.
Run `test-foc-impedance-feasibility` for the numerical contract and
`test-foc-impedance-static-hold` in Simulation for formal Runtime closed-loop
witnesses.

### Model motion feedforward

`foc_impedance_feedforward_evaluate()` in `include/foc_impedance_feedforward.h`
computes the signed motor-shaft term `J * acceleration + B * velocity`.
The caller supplies a qualified mechanical model with matching reference
and model frame IDs, coherent velocity and acceleration from one point of a
continuous, bounded trajectory, and a symmetric feedforward torque budget.
The result is a complete candidate with model and frame provenance. Rejection
clears the result, including when the model term exceeds its budget.

Pass a usable candidate explicitly through the existing IMPEDANCE command
feedforward member. The term accounts for inertia and viscous motion. Any
separately confirmed external load remains a caller contribution. Allocate
budget for feedback and any load compensation, then respect the existing
Runtime torque and current limits. The calculation owns no controller state.
Run `test-foc-impedance-feedforward` for the numerical contract.

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

`foc_velocity_step` uses actual `dt_s`, admits only the supported integral increment, and bounds the integral when the q current limit drops. All validation/calculation errors leave the context (configuration, initialization flag and integral) unchanged and clear every output field. A valid disabled step freezes state and clears output; inputs are still validated first. Position P limits its velocity reference and has no state to reset. A new motion session resets the velocity PI.

### Prediction admission, numerical accuracy and system error

`foc_rotor_predict` retains constant-speed prediction, Float32 arithmetic and the existing normalization. Its contracts have three separate scopes:

1. **Input admission.** The caller establishes a forward age with its modular clock check. The API checks pointers, finite sample/settings, nonnegative acceleration/error bounds, a horizon below the 32-bit clock half range, age within that horizon, and `0.5 * max_electrical_acceleration * dt_s^2 <= max_error_rad`. Normalization rejects a non-finite computed phase. A writable destination is cleared on failure. `FOC_STATUS_OK` means these existing checks passed; finite inputs alone do not grant arbitrary-input numerical accuracy.
2. **Nominal calculation regression.** `test-foc-motion-reference` compares against independent Double time conversion, phase accumulation and mathematical `2*pi` reduction, starting from the actual stored Float32 input values. The test envelope is input phase `[0, Float32 2*pi)`, electrical speed `|omega_e| <= 7000 rad/s`, integer prediction ages `0..650 us`, and the default Board policy `{650 us, 2000 rad/s^2, 0.00065 rad}`. The acceptance budget is `2e-6 rad` measured by shortest periodic angle distance. Boundary matrices cover every integer age; extra samples use fixed seed `0x0c012026`. The tests assume binary32 float, at least binary64 double, normal round-to-nearest arithmetic and the project's standard build flags without fast-math. This is repeatable sampled regression, not a proof for every representable input or target MCU math implementation.
3. **System total error.** Float32 calculation error is separate from acceleration-model error, encoder phase quantization, velocity-estimation error, sensor acquisition latency and timestamp uncertainty. Input representation error before storage is also outside the calculation Oracle. `max_error_rad` limits only the assumed acceleration-model contribution; it is not a total electrical-angle error guarantee. The acceleration assumption itself needs an application or measurement basis. At the default 650 us horizon, the assumed model contribution is `0.0004225 rad` before adding those other terms.

The reference test retains an observation at `omega_e = 65535000 rad/s`, age `400 us` and phase `0.35820722579956055f` with an admitting policy. It prints its circular calculation error without imposing the nominal budget or requiring the error to remain large. Large speeds, large unbounded phases/offsets, longer horizons and nonstandard floating-point modes have no precision guarantee from this regression. API admission and success/failure behavior are unchanged.

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

The Host test executables cover transforms, SVPWM, voltage-open-loop stepping, d/q current feedback, and the motion primitives. The current test uses a fixed-rotor R/L model driven by final duties. The Runtime test project exercises the cascaded controller with an electrical and mechanical PMSM model.

The existing `test-foc-motion` CTest entry includes positive/negative integral-trial and proportional-request overflow, poisoned-output clearing, complete logical context preservation, disabled/invalid inputs and same-instance recovery. `test-foc-motion-reference` adds the independent prediction Oracle described above to its existing rotor, PI, Position and Trajectory references. These checks extend the existing entries; they do not add a new test framework or certify physical control quality.

## Integration boundary

The G2 context owns voltage-open-loop configuration and electrical angle. The separate current context owns its configuration and two integrators. An adapter owns sample timing, coherent snapshots, and hardware output submission. Host validation covers the software boundary. Target timing and physical output authorization are separate acceptance steps.
