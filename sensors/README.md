# FalconFOC V2 portable magnetic encoder algorithms

`sensors/` owns the MCU-independent Float32 interpretation of a timestamped
absolute encoder frame. It converts raw codes into directed single-turn angle,
integer-backed continuous mechanical position, raw difference speed, a
variable-`dt` first-order LPF speed, an alpha-beta tracked speed, and an
explicit quality result.

The module does not access SPI, DMA, IRQ, GPIO, ADC, PWM, Runtime, Safety, or
the AT32 SDK. The caller owns storage and supplies the source capture
timestamp. No dynamic allocation or global mutable state is used, so multiple
encoder instances can run independently.

Continuity is conservative. A timestamp outside the uint32 half-range, a gap
past `max_dt_us`, an interval whose configured maximum speed could alias a
half revolution, an exactly half-revolution code jump, or an out-of-range
speed latches `DISCONTINUITY` until `foc_encoder_reset()`. Transport timeout,
frame error, duplicate timestamp, and invalid sample remain separately
classified and never advance the accepted position or filter state.

## Build and test

```powershell
cmake -S sensors -B build/sensors -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sensors --parallel 4
ctest --test-dir build/sensors --output-on-failure
```

The Host test uses deterministic synthetic static, wrapped, constant-speed,
timestamp-wrap, invalid-transport, and multi-instance sequences. Its printed
RMS values are algorithm comparisons only; they are not board noise,
electrical timing, MCU cycle, or real motor evidence.

## Integer periodic phase

An accepted observation carries `feedback.mechanical_phase_count`, the directed
integer code in `[0, context.config.counts_per_revolution)`. It equals the
mathematical nonnegative modulo of `position_counts`, including negative turns,
and comes from the same accepted observation and `captured_us` as continuous position.
CPR belongs to that Sensor instance's immutable configuration; consumers must
not substitute a hidden Board constant or apply direction a second time.

The first accepted frame has this position-only phase while `valid` and
`speed_valid` remain false. Rejected observations clear the integer phase and
retain invalid quality; zero alone cannot authorize consumption. Reset retains
configuration and requires the same first/second-frame acquisition sequence.
Changing direction or CPR requires a quiesced reinitialization, not a running
configuration write.

Continuous `mechanical_rad` keeps accumulated turns. Its Float32 absolute
resolution decreases with magnitude; the new integer phase does not promise
constant multi-turn position resolution.

## Optional periodic linearization

The ordinary `foc_encoder_init()` path leaves compensation off. To opt in,
supply a `foc_encoder_lut` and matching nonzero encoder/mount identity to
`foc_encoder_init_with_lut()`. `foc_encoder_lut_validate()` checks the table
without initializing a session. `foc_encoder_correct_code()` queries the
undirected corrected code of an initialized instance.

- Store signed `int16_t` offsets in whole counts. Knot k is at k × CPR / N
  in the raw encoder frame. Use 2 to min(CPR, 1024) knots and no repeated end
  knot. Offset magnitude is bounded by floor(CPR/8). Every segment, including
  the closing segment, limits the slope of the corrected mapping to [0.5, 1.5].
  The integer result can repeat or skip one code due to rounding.
- The caller owns the immutable array throughout the session and reset.
  Initialization copies the pointer and length, keeping no access to the
  temporary descriptor. Quiesce and reinitialize to replace or disable a
  table. Bad identity, magnitude or slope returns its own error and
  preserves the previous context.
- Update interpolates once from the raw code, wraps the result, then applies
  direction and integer unwrapping. `raw_code` stays in hardware coordinates,
  `corrected_code` is undirected, and `mechanical_phase_count` is directed.
  The same corrected path supplies continuous position and velocity.
  H1 receives the directed integer phase; H0 uses its existing prediction.
- A varying table conservatively rejects a raw half-turn or conflicting raw
  and corrected short arcs. It reserves 2.5 counts at the corrected
  half-turn for two endpoints of conventional count quantization plus output
  rounding. This can reject a physically valid near-half-turn move. Choose
  a capture cadence with margin. Constant and zero tables retain the legacy
  branch logic. Existing transport and timestamp checks continue to apply.

A valid table establishes numerical shape and configuration identity. Use
independent reference-angle data to qualify its actual error reduction.
Integer quantization and interpolation can increase low-speed speed ripple.
Electrical zero, lost frames and timing calibration remain separate
contracts.

## Board1 boundary

Board1 retains SPI transaction start/completion, source capture time, timeout,
frame status, and `foc_runtime_h1_step()`. It owns one `foc_encoder_context`
and maps valid mechanical position and LPF speed into `foc_runtime_sensor`.
The same observation's integer phase and Sensor CPR enter the Core phase helper
with that Runtime's rotor config. H1 requires explicit `electrical_phase_valid`
in motion-enabled mode; a missing phase never falls back to accumulated float
position. Safety and the Board1 output lock remain unchanged.

/* ---------------------------------------- end of file ----------------------------------------- */
