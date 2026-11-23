# FalconFOC V2 calibration candidates

`calibration/` is a portable, offline C11 module for producing calibration
candidates. It estimates three phase current zero offsets, identifies direction
and pole-pair candidates from explicit mechanical/electrical reference pairs,
and estimates electrical angle offset with circular residuals.

The module separates calculation from application. It does not decide whether
a power stage is safe, does not assume that ADC values near 2048 prove zero
current, does not access Runtime/Safety/Board/Device, does not write Flash, and
does not control PWM. The caller owns the physical precondition and must match
the result identity before any later, separately authorized application step.

The intended Board1 mapping is explicit and deferred: ADC result offsets map to
`foc_device_adc_config.phase_a/b/c_offset_count`, current gain metadata must
match `current_a_per_count`, and rotor results map to the existing positive
pole-pair Core through an explicit coordinate conversion. The admission helper
`foc_calibration_admit()` checks all three complete candidates and commits a
single `foc_calibration_snapshot`; it never partially updates a running
configuration. The fitted rotor sign is multiplied by the raw-code-to-source
mechanical sign to produce the Runtime Sensor direction, so CAL-01 `direction`
is not applied as a second blind inversion. Board1 consumes that snapshot only
through its explicit pre-initialization entry point; the nominal initializer,
Flash policy, and output lock remain unchanged.

All sessions use caller-owned storage, fixed bounded sample arrays or online
statistics, no dynamic allocation, and explicit failure states. Rotor and
electrical-offset fitting require timestamped, ordered, valid paired references;
passive mechanical angle alone is intentionally reported as unidentifiable.

The optional [unified calibration workflow](WORKFLOW.md) orchestrates these
estimators and supports explicit, reviewed initialization of a new Host
Runtime through the Control Plane application adapter.

[Measurement Trace v1](MEASUREMENT_TRACE.md) validates recorded observations before
offline replay through the same workflow. The Host CLI loads and hashes the pack.

## Standstill motor R/L and PI design

Include `foc_motor_id.h` and link `falconfoc-calibration`. A capture belongs to
one caller-owned context, one axis, one acquisition origin, and one caller
capture ID. The same instance can be reset for another capture.

1. Initialize a d or q capture with expected time intervals, signal and
   parameter ranges, excitation floors, and a residual bound. Require
   max_interval_s*max_Rs/min_L <= 0.3 (the fastest admitted RL pole).
2. Submit a capture ID, nonzero setup ID and acquisition origin on every
   endpoint. The setup ID binds the motor, fixed frame and acquisition
   calibration. Allocate a fresh capture ID for each acquisition, including
   after reset. Use a shared monotonic microsecond clock. The acquisition
   owner rules out clock resets and gaps of an entire counter period.
3. Each endpoint supplies signed current at the ADC aperture. For later
   endpoints, the terminal voltage in V is the time average over the actual
   [previous ADC aperture, current ADC aperture) interval. Supply both bounds
   and confirmation of actual output for that interval. The configured class
   must be a Virtual Motor input observation, a measured terminal voltage, or
   an independently calibrated terminal-voltage reconstruction. A PI request,
   modulation limiter result, duty register, or PWM submit acknowledgment by
   itself has unverified terminal voltage. The first endpoint has no preceding
   voltage interval. A controlling firmware may instead declare
   `FOC_MOTOR_ID_VOLTAGE_COMMANDED` (measured origin only): the interval voltage
   reconstructed from its own applied duties and measured bus voltage, without
   independent terminal validation. The class travels with the result; on
   hardware it carries dead-time and switch-drop error (Board1 on-MCU CAL-3).
4. `foc_motor_id_finish()` requires complete non-overlapping windows, with
   at least two intervals per window, at least three windows, and at most
   65,535 endpoints. Each row is mean(v) = Rs * mean(i) + L * delta(i)/T.
   Current uses trapezoidal quadrature over each actual interval. Double
   accumulators retain six products without storing the capture. RMS floors,
   normalized column independence, physical bounds and residual determine
   usability. The residual is evaluated on the published Float32 parameters.
   All admission checks precede accumulation. Changed identity reports
   INPUT_CHANGED, mispaired time bounds report TIME_ORDER, and missing
   voltage, output, frame or standstill evidence reports PRECONDITION
   when the sample values themselves are valid. Results keep setup, origin,
   configured voltage class, axis, capture ID, bounds, counts and diagnostics.
   Failed captures yield zero parameters.
5. Pair usable d and q fits with common setup, origin and voltage class and
   different capture IDs through `foc_motor_id_make_model()`. It bounds
   their relative Rs disagreement and averages the two estimates. Retain
   both fits as provenance alongside the numerical model. Ld and Lq come
   from their respective captures.

The model is the zero-speed reduction of the PMSM d/q equations. Correct
voltage/current scaling, electrical alignment, sufficient excitation and
sampling resolution are acquisition requirements. Assert the common, fixed
d/q frame for I and V and standstill for each endpoint and the entire interval.
For measured captures, associate the setup ID with validated electrical zero
and signs, rotor restraint, and voltage and clock calibration evidence. The
estimator checks supplied assertions and zero observed speed. Zero encoder
change alone cannot establish standstill. A permanent-magnet rotor under q current may
move. Real acquisition must establish restraint and measure or validate
interval-average terminal voltage, including inverter drop and deadtime.
The estimator assumes a held or appropriately averaged voltage per interval
and current smooth enough for endpoint quadrature. Arbitrary within-interval
waveforms can violate this even with a correct voltage mean. At fixed-step
zero-order hold, the 0.3 pole limit caps the trapezoidal L bias at about
0.75 percent. Jitter, noise, conditioning and waveform errors still need
separate evidence. A small residual does not establish the acquisition assumptions.

`foc_motor_pi_design()` accepts trusted model parameters (known model inputs
or a successful merge) and returns candidate d/q Kp and common Ki in V/A
and V/(A s). For a requested bandwidth fc, it uses wc = 2*pi*fc,
Kp_d/q = Ld/q*wc and Ki = Rs*wc. The policy rejects the following requests.

- wc*Ts > 0.3 or Rs*Ts/min(Ld,Lq) > 0.3.
- wc*(delay + Ts/2) > pi/6. Ts includes worst admitted sample jitter.
  delay is sample to voltage application and excludes the hold term.
- A first-update demand above the declared incremental voltage headroom,
  or gains and Ki*Ts outside the normal positive Float32 range at the
  design sample period.

The demand uses (max(Kp_d,Kp_q) + Ki*Ts)*initial_error, matching Current's
same-cycle integral contribution from a zero integral perturbation. Actual
steady voltage and anti-windup recovery depend on the plant, existing bias,
duty constraints and control schedule. The bounds define design policy for
the ideal model. Apply candidates explicitly to an offline configuration and
validate the closed-loop response before using them in another context.

## Magnet flux and derived Kt

`foc_flux_id.h` is a standalone C11 estimator for adjacent current apertures
and co-timed interval averages of actual terminal Vq and electrical speed.
The caller supplies a qualified R/L model, pole pairs, current/voltage/speed
chain identities, valid d/q frame, common clock and dependency revisions.
One interval has an exact declared duration. Samples are contiguous, with
matching shared current endpoints and 32-bit modular time. The capture must
span less than half the clock range. An interval needs confirmed effective
output, a terminal-voltage witness and unsaturated observations.

The estimator accepts model terminal voltage for virtual data, independently
sensed terminal voltage for measured data and independently validated
terminal reconstruction for measured data. The latter needs a calibration
and uncertainty basis for the same interval. Qualification flags and chain
IDs identify the caller's evidence. Physical accuracy still depends on that
evidence. A measured `FOC_MOTOR_ID_VOLTAGE_COMMANDED` capture declares its
voltage unwitnessed (firmware duty and bus reconstruction) and keeps that class
in the result.

`foc_flux_id_break()` ends the current contiguous segment: the open partial
window is discarded and the next interval may start at any later time and
sequence. A physical motor that reverses or changes speed captures each settled
level as its own segment instead of a fixture's instant speed steps.

For each same-sign window, the estimator averages Vq − Rs·Iq − ωe·Ld·Id
and subtracts Lq·ΔIq/T using the paired current endpoints. It fits the
remaining voltage to mean ωe through the origin and reports separate positive
and negative fits, residual, agreement, windows and signed operating speed.
Current endpoints are integrated with the trapezoidal rule. The interval
average of speed times the Id midpoint requires adequate resolution of the
ωe·Id product. The default Host fixture uses 50 µs intervals and signed
10/20 electrical rad/s levels.

The project uses peak phase currents and amplitude-invariant Clarke/Park.
Flux is in Wb. With p pole pairs, Ke = p·flux is open-circuit phase-to-neutral
peak V per mechanical rad/s. Kt = 1.5·p·flux is N·m per peak q ampere at Id=0.
Nonzero Id in a salient motor adds the torque term
1.5·p·(Ld−Lq)·Id·Iq. The estimator labels its outputs
`FLUX_FROM_MEASUREMENT` and `KT_DERIVED_FROM_FLUX`. Independent torque
calibration has its own source, `KT_FROM_INDEPENDENT_TORQUE`.
[Measurement and acceptance evidence](../docs/evidence/2026-11-20-next-cal-flux-01.md)

## Equivalent mechanical inertia and viscous damping

Include `foc_mechanical_id.h` and link `falconfoc-calibration`. Initialize a
caller-owned capture with `foc_mechanical_id_init()`, feed endpoints with
`foc_mechanical_id_push()`, and obtain a candidate with
`foc_mechanical_id_finish()`. Use `foc_mechanical_id_reset()` with a fresh
capture ID. An invalid session rejects subsequent samples and exports zero
coefficients. The returned setup, torque model, acquisition and load classes
scope the candidate to the qualified mechanical assembly.

All signals share the signed motor-shaft mechanical frame and one source clock.
Qualify the measured dq current and independent Kt over the admitted Id range,
or supply an independently qualified full electromagnetic torque observation.
Use actual observation apertures and confirm that the speed, torque and any
signed opposing load represent each interval. Time labels alone cannot qualify
LPF lag, acquisition delay, or the output and current association. Confirm a
negligible load or supply measured load. Undeclared friction, unobserved load
and incorrect Kt can bias the model. The acquisition owner must keep speed and
torque sufficiently resolved across every interval, including variation faster
than the mechanical time constant. The fitted pair must satisfy
`B * max_interval_s / J <= 0.3` (RANGE otherwise); the check runs on the fit,
not on the configured bounds, so wide bounds still admit every plant the
interval resolves. For fixed-step trapezoidal quadrature on the linear model
this limits the integration error associated with that pole. It does not guarantee accuracy for arbitrary intersample
waveforms or timing errors.

Complete windows use
`mean(tau_e - tau_load) = J * delta(omega)/T + B * mean(omega)`.
Trapezoidal integration uses actual intervals. Six Double statistics support
an online two-column fit with bounded work and fixed storage. Both
acceleration signs, speed and acceleration RMS floors, column independence,
physical parameter limits and a residual upper bound on the published
Float32 coefficients gate a usable result. The damping term represents
viscous damping within the acquisition conditions. Each usable J/B pair
can be supplied explicitly to `foc_impedance_tuning_design()` using the same
frame and Kt assumptions. Confirm the design against an independent closed
loop before use. No capture, mode switch, gain application or persistence
runs automatically.

## Offline encoder LUT generation

The encoder calibrator lives in `foc_encoder_calibration.h`. It fits the
inverse mapping from raw hardware code to measured mechanical reference.
The caller supplies a real Sensor policy, fixed setup and reference identity,
and a known mechanical reference zero in radians. Each pair must carry the
same raw aperture and reference timestamp on a shared microsecond clock.
The acquisition owner verifies reference independence, time synchronization,
direction and zero alignment. Calibration checks the supplied identities,
validity, timestamps, continuous sequence and reference qualification.

`foc_encoder_cal_init()` begins training; `foc_encoder_cal_push()` accepts
pairs. Each segment must have the configured count, samples in all four
quarters and sufficient position variance during training. Use 4–64 equally
spaced knots and at least four raw codes per segment. Local linear fits in
raw coordinates estimate their two endpoints. The two estimates meeting
at a knot are averaged, including the periodic seam. The signed knot mean
is checked as a zero-bias diagnostic and removed before rounding. The
unrounded nodal mean is zero; integer quantization can leave up to half a
count of mean. The reference and electrical zero settings remain external
to the generated table. The training diagnostic measures the interpolant
of the integer knots before the Sensor's per-sample integer rounding and
includes a scale-dependent allowance for roundoff. Its per-segment gate
retains sensitivity to local residuals despite uneven global coverage.

`foc_encoder_cal_freeze()` checks training diagnostics, quantizes knots and
runs the existing Sensor LUT admission. Start a distinct, later holdout
capture with `foc_encoder_cal_begin_holdout()`. Its full coverage is
required, and its errors use `foc_encoder_correct_code()` on the frozen
table. `foc_encoder_cal_finish()` checks RMS, peak and non-degradation
against the raw angle. A rejected holdout cannot modify the fit and still
be accepted in that session. Host datasets should be distinct in capture
and sample trajectory. Chronology and IDs support provenance, and the
caller is responsible for ensuring observations are actually independent.

`foc_encoder_cal_export()` copies a valid result into caller-owned storage.
Keep the result and its acquisition policy for traceability. Borrow the
read-only Sensor descriptor with `foc_encoder_cal_make_lut()` and install
it explicitly at Sensor initialization. The result must outlive that
Sensor use. Reset retains policy and starts a new capture with a fresh ID.
Invalid operations preserve independently exported results. This module
does not enter Runtime or the current reference firmware build.

The Simulation periodic-chain test accepts `--encoder-calibration-study`
for quality and control-chain summaries and `--encoder-calibration-dataset`
to also emit the underlying training, holdout and negative-case pairs.

## Build and test

```powershell
cmake -S calibration -B build/calibration -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/calibration --parallel 4
ctest --test-dir build/calibration --output-on-failure
```

The deterministic Host test covers current-zero noise/outliers/drift and
precondition rejection, direction/pole-pair fitting and ambiguity, circular
offset at the 0/2π boundary, identity mismatch, invalid input, and independent
instances. Its values are synthetic algorithm evidence, not real motor or
power-stage calibration.

/* ---------------------------------------- end of file ----------------------------------------- */
