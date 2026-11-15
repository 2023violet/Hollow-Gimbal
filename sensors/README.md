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

## Board1 boundary

Board1 retains SPI transaction start/completion, source capture time, timeout,
frame status, and `foc_runtime_h1_step()`. It owns one `foc_encoder_context`
and maps the module's valid mechanical position and selected LPF speed into
the existing `foc_runtime_sensor` structure. Runtime, Safety, and output lock
contracts remain unchanged.

/* ---------------------------------------- end of file ----------------------------------------- */
