# FalconFOC Host pipeline and Runtime

The caller serializes each pipeline instance. One cycle decodes ADC ranks using G3-NP, checks sample and command eligibility through G3-SAFE, and computes the d/q current update in a copy of the controller. G3-NP maps all three duties. G3-SAFE evaluates the complete request again at final gate time. Only Mock acceptance of that UPDATE commits the PI copy.

## Run and recover

1. Zero-initialize an instance and call `foc_pipeline_init`. The first Mock action is `INHIBIT`.
2. Publish fast and foreground progress through their separate `foc_safe_heartbeat_*` functions on the embedded Safety context. Publish the d/q command with its producer timestamp and sequence via `foc_pipeline_set_command`.
3. Pass a fresh raw ADC sample and its aligned electrical angle to `foc_pipeline_step` while disabled. The function records the sample and returns `FOC_PIPELINE_ERR_STATE` because output is inhibited. Call `foc_pipeline_arm` and then `foc_pipeline_start` with current monotonic time. START resets both PI integrators. The next step must consume a new sample.
4. A normal step returns `FOC_PIPELINE_OK` only after accepting one complete UPDATE. Use `mock.output_confirmed` and `mock.last_request.action` to identify current output. The historical `mock.last_update` is for diagnostics.
5. A rejected running step revokes the run session and attempts `INHIBIT`. Use `last_reason` and `safety_reasons` to diagnose the rejection. `FOC_PIPELINE_ERR_INHIBIT` reports that the current output is unconfirmed. Retry `foc_pipeline_request_inhibit` to obtain confirmation before rearm or fault clear.
6. A fresh but invalid ADC or angle sample inhibits immediately and counts toward the existing Safety threshold. A valid sample resets the consecutive-invalid count and still requires fresh command and sample observations followed by ARM and START. A latched fault also requires `foc_pipeline_clear_fault` with confirmed inhibition.

All timestamps share a 32-bit monotonic microsecond clock. Zero and wrap are valid, and ordering is defined within a strict half range. The pipeline retains sequence watermarks across stops and fault clear. Recovery requires command and sample producer timestamps strictly after the recovery boundary. This conservatively rejects observations stamped in the same microsecond as the stop or clear. `angle_sample_us` must equal `adc.sample_tick`, and `dt_s` is supplied by the caller in seconds. Ordinary command replay leaves the accepted timestamp intact.

## Build and exercise

```text
cmake -S runtime -B build/sw01a-runtime -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sw01a-runtime --parallel
ctest --test-dir build/sw01a-runtime --output-on-failure
```

Run `test-foc-pipeline` to exercise the serial pipeline and `test-foc-runtime` to exercise the execution domains. Both use the same fixed-rotor plant driven by accepted Mock compare values. The model stops at `INHIBIT`, and the Mock gate stays closed throughout.

## NEXT-SW01B execution domains

Initialize `foc_runtime` before enabling events. Use one monotonic microsecond clock and one owner per execution domain. Deliver H1 sensor observations and H0 ADC triggers through their corresponding entry points. Publish commands and call `foc_runtime_foreground_poll` independently of H0. For startup, first pass a matching sensor and ADC observation while disabled, then deliver ARM and START on distinct H0 events. START resets PI and the following new sample is the first eligible UPDATE.

| Data | Writer | Reader | Publication contract |
| --- | --- | --- | --- |
| Sensor slots | H1 | H0 | Fill inactive slot, then release-store the index. H0 acquire-loads the index and copies the complete slot. H1 cannot preempt H0 |
| Command slots | Foreground | H0 | Same direction and slot contract. Keep the producer timestamp and sequence for replay checks |
| PI, Safety, Mock | H0 | H0 | Only H0 mutates them after initialization. Foreground uses the published view |
| Telemetry and Fast heartbeat | H0 | Foreground | One bounded copy of atomic fields. Discard on an odd or changed generation. The copy window must contain fewer than `2^31` H0 publications |
| Foreground heartbeat | Completed Foreground supervision | H0 | Release-publish the completion stamp. H0 imports each newer observation once |
| Stop channel | Independent requests | H0 | Lock-free OR merges causes. H0 exchanges pending bits while its telemetry generation is odd. The final guard checks pending stop after Safety and before software admission |

This contract assumes a single core with `H0 > H1 > Foreground`, no domain reentry, and 32-bit lock-free word atomics. The Host test invokes higher-priority work at deterministic boundaries. In particular, the lower-priority slot protocol depends on the priority order and is unsuitable for parallel Host threads. An INHIBIT that arrives after an admitted transaction invalidates its output view and is serviced separately. Pending stop is shown as unconfirmed INHIBIT until H0 handles it. Supervision runs R2 on a private view of H0 observations and can request stop without changing H0's main state. A changed H0 generation or newly pending stop invalidates a Foreground watchdog decision before return.

Each service has a period, maximum delay and simulated budget. A due service skips old deadlines in one step and records the delay from the oldest outstanding deadline. H0 computes `dt_s` from consecutive accepted ADC source timestamps. A missed nominal tick can still produce one update if delay, budget, sample interval and Safety remain admissible. Supervision has first use of the Foreground budget. Telemetry can be dropped. Both heartbeat timestamps advance only after their respective services complete.

The original fixture uses matched `100 µs` H0 and H1 periods, `1 ms` supervision and `10 ms` telemetry. With `motion.enabled` false, the original angle contract requires exact ADC source-time matching. Simulated costs, stop acknowledgement, and compare values establish Host behavior. MCU interrupt arbitration, generated atomic instructions, execution time and physical output need separate evidence.

## NEXT-CONTROL-01B electrical-speed feedforward

`foc_pipeline_config.feedforward` is disabled by default. When enabled, the
Pipeline requires `foc_pipeline_input.electrical_feedback` to contain the
finite, signed `foc_electrical_state` and source timestamp copied from one H1
sensor slot. H0 fills this field from the same slot used for angle prediction;
it does not recompute speed, multiply by pole pairs again, or read a global
cache. The configured `max_age_us` bounds the source-to-ADC interval with the
same modular timestamp ordering used elsewhere in Runtime.

The speed is a zero-order-hold estimate from the H1 source instant to the ADC
sample. It is accepted only when the complete H1 sample is valid, the source is
not in the future, and its age is within the configured bound. A stale,
future, non-finite, or invalid snapshot returns `FOC_PIPELINE_ERR_FEEDBACK`,
submits the existing `INHIBIT` path, and leaves the staged PI state uncommitted.
The original `foc_current_step()` path and Board1 default remain unchanged when
the policy is disabled. This software contract does not measure encoder filter
delay, acceleration-model error, or hardware ISR timing.

## NEXT-CONTROL-02 trajectory planning

`foc_trajectory` is an optional H1 reference shaper placed before the existing
velocity PI. In VELOCITY mode it clamps the command and limits each reference
change by the acceleration (speeding up) or deceleration (slowing down). In
POSITION mode it runs `foc_trajectory_track_step` on the planned reference and
the speed reference is the planned velocity plus the position P on the planned
position minus the measurement, clamped to the speed limit. It does not create
another PI or replace the existing position P and velocity PI when disabled.

With a profile inertia and flux, H1 adds the planned torque
`(J * planned acceleration + B * planned velocity) / Kt` (Kt = 1.5 x pole pairs x
flux) to the velocity PI output, clamped to the outer q-current limit, so the PI
only corrects the residual.

Tuning IDs 106/107 (`accel_rad_s2`, `decel_rad_s2`, rotor-side, 0 unlimited)
derive the policy: either nonzero enables it with the position speed limit, both
zero disable it; a configuration that sets none keeps an explicit `trajectory`
policy as given.

The planner context is copied for each H1 trial. Its committed state follows
the same `accepted_outer_seq` and `motion_session` boundary as the velocity PI:
an H0/Safety/Device rejection cannot advance the formal trajectory. A new
session resets the planner, and invalid sensor or timing input cannot generate a
new reference. Normal trajectory deceleration never delays an independent
Safety INHIBIT.

## NEXT-FOC-02 sensored modes

Enable `config.motion` and supply H1 with coherent continuous mechanical position,
speed, source time, and an independent bounded `theta_elec_rad`. Set
`electrical_phase_valid` only for a same-observation phase with direction,
pole pairs and electrical offset already resolved using this Runtime's immutable
rotor configuration. Encoder producers use `foc_rotor_phase_from_counts()` and
their own Sensor CPR; ideal Host models reduce their original double coordinate
before converting to Float32. Do not derive phase from large Float32 position.

Motion-enabled H1 requires both validity flags, finite continuous position/speed,
finite electrical speed, and phase in `[0, Float32 2*pi)`. Missing/invalid phase
publishes an invalid candidate; it never falls back to accumulated position or
treats default zero as valid. Existing time/sequence rejection, mailbox and H0
ownership remain in force. Producers must construct one coherent snapshot;
H1 cannot reconstruct provenance from falsely labeled numeric fields. Appended
Sensor/Feedback fields require all binary consumers to rebuild. Motion-disabled
direct-angle input still accepts a finite angle with exact ADC-time alignment
and does not require the new phase flag.

H0 predicts phase forward to the ADC source instant when age, prediction horizon,
and extrapolation error limits all permit it. The H1 capture timestamp stays
intact. `angle_prediction_us` reports the applied interval. Continuous position
continues unwrapped to Position/Trajectory/telemetry. The acceleration bound is a
sensor/motor assumption; H0 rejects a future capture or invalid prediction.

CURRENT uses `foc_runtime_publish_command` with d/q current. VELOCITY and POSITION use `foc_runtime_publish_motion_command` with mechanical rad/s or continuous rad. H1 limits speed references, produces `Id=0` and a limited `Iq`, and publishes the result with its original command sequence/time and the H0 mode/session. H0 updates the observed source only for a new command. Reusing or recomputing a derived current target leaves the source timeout unchanged. H1 commits a staged velocity integral after observing the matching accepted target from H0. Rejected targets cannot accumulate across H1 steps.

Select a different mode using `FOC_RUNTIME_ACTION_SET_MODE` only in DISABLED with confirmed INHIBIT. New source commands and sensor captures must follow the new boundary. H1 resets its velocity state when it next observes the boundary, and H0 resets inner PI on explicit START. Before START, outer modes publish a neutral target for readiness. The normal Safety ARM/START sequence still applies.

Run `test-foc-motion-model` for direct and Runtime PMSM dynamics, including `100 µs` H0, `500 µs` H1, `1 ms` supervision, reversible speed and position steps, and load transients. Run `test-foc-motion-runtime` for fault, timeout and mode-transition assertions. The model consumes confirmed compare updates and stops at INHIBIT.

## Runtime IMPEDANCE mode

For a C API integration, enable `motion.impedance` alongside `motion.enabled`
at initialization. Supply the qualified positive motor-shaft Kt in N*m/A,
positive torque cap, nonnegative ceilings for stiffness, damping and absolute
feedforward, and a nonzero mechanical frame identity. The copied policy is
immutable. The existing `motion.iq_limit_a` is the configured q limit and must
fit the Current limit. A separate live thermal or power derating input is
outside this interface.

In DISABLED with confirmed INHIBIT, publish a lifecycle SET_MODE request for
`FOC_RUNTIME_MODE_IMPEDANCE`. After the H0 acknowledgement, read the current
Session and publish `target.impedance` through
`foc_runtime_publish_impedance_command()`. Send the original producer time
and an advancing sequence across modes and Sessions. For opt-in instances,
revocation preserves this producer watermark. While IMPEDANCE is selected,
the legacy current and cascade publication APIs cannot replace its command.
After a mode change or recovery, both source times must follow the boundary
before H0 accepts a target.

Use the same mechanical zero, sign and motor axis for command and H1 sample.
Set the sample's matching `mechanical_frame_id` only for valid same-capture
position and velocity. Both positions must lie in the inclusive
`[-1024, 1024]` rad domain. With binary32 and round-to-nearest, the conservative
absolute bound for input rounding and the subtraction together is 0.000245 rad,
exclusive of upstream position errors. The controller uses a continuous error
without angular wrap. A source frame mismatch or a position outside the domain
produces no q target. Electrical phase continues to use the existing independent
phase input.

After successful Core validation, H1 publishes zero q for ARM and START
readiness, then computes q from `foc_impedance_step()` during RUNNING. H0
checks the target's mode, Session and original command and sensor timing,
then uses the existing guarded Pipeline. UART V1 does not admit IMPEDANCE
setpoints. Zero-initialized policy leaves the mode disabled, including Board1.
The Runtime public structs have grown and binary consumers must recompile.

`motion.impedance.damping_filter_hz` is an immutable initial policy. Zero
keeps the original damping path. Values from 1 to 200 Hz enable a first-order
backward-Euler low-pass on the signed speed error used by D. Using the same
filter for the reference and feedback contributions preserves their relative
phase during motion. H1 uses elapsed source time across wrap. The first valid
RUNNING sample seeds the filter. A session change, invalid feedback, skipped
capture or a source interval beyond the H1 period plus delay allowance starts
a fresh state. STOP, FAULT and mode changes leave the filter inactive until a
new qualified impedance target. Encoder speed and phase, H0 prediction,
motion limits and output admission still use their original inputs.

Read back the immutable policy through `foc_runtime_diagnose()` in Control
Plane integrations. The cutoff is local Runtime configuration. UART V1,
the configuration storage schemas and FCAL v1 do not serialize it.

Run `test-foc-runtime-impedance` and
`test-foc-runtime-impedance-virtual` for the transaction and virtual motor
contracts. The simulation test accepts `--trace` for the six Host scenarios.

## NEXT-SYSTEM-01 full-chain Host stability

`test-foc-system` is the deterministic system-level Host lane. It enables the
optional feedforward and trajectory policies together, drives H0 at `200 us`,
H1 at `500 us`, and foreground supervision at `200 us`, then replays a
multi-rate event stream twice. The test changes velocity direction, refreshes
commands, records a stable replay fingerprint, rejects one UPDATE, verifies
that current/outer/trajectory staged state is not committed, and finally
injects a WDT inhibit. It exercises the existing Host Mock boundary only; it
does not unlock Board1 output or claim MCU timing.
