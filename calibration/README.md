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
   voltage interval.
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
