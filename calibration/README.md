# FalconFOC V2 calibration candidates

`calibration/` is a portable, offline Float32 module for producing calibration
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
