# FalconFOC V1 CLI

The codec, JSON CLI, shared-vector tests and simulated connection use Python's
standard library. A selected physical port uses the existing pyserial package.
No project dependency manifest is changed.

The [offline configuration inspector](../falconfoc-config-inspector/README.md)
opens Host storage images. UART V1 continues to expose its existing observable
device state and fixed parameter set.

Run from the repository root.

```powershell
cmake -S firmware/at32f415 -B build/sw04-host -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/sw04-host
python -m unittest discover -s tools/falconfoc-cli -p test_falconfoc_cli.py -v
python tools/falconfoc-cli/falconfoc_cli.py encode hello --sequence 0
python tools/falconfoc-cli/falconfoc_cli.py decode --hex 06464601010101010101010101010101038ce700
python tools/falconfoc-cli/falconfoc_cli.py vectors
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/sw04-host/foc-serial-sim.exe demo
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/sw04-host/foc-serial-sim.exe request param-read --id 1
```

For the Host Virtual product workflow, build the simulation target and run the
same process through Current, Velocity, Position, load and sensor-fault demos:

```powershell
cmake -S simulation -B build/vm01a-simulation -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/vm01a-simulation --parallel 4
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/vm01a-simulation/falconfoc-virtual-device.exe virtual-demo --virtual-scenario current --csv build/vm01a-simulation/current.csv --json build/vm01a-simulation/current.json
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/vm01a-simulation/falconfoc-virtual-device.exe virtual-demo --virtual-scenario velocity
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/vm01a-simulation/falconfoc-virtual-device.exe virtual-demo --virtual-scenario position --advance-us 500 --duration-us 1200000
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/vm01a-simulation/falconfoc-virtual-device.exe virtual-demo --virtual-scenario load
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/vm01a-simulation/falconfoc-virtual-device.exe virtual-demo --virtual-scenario fault
```

The virtual device is a Host-only process. It uses the existing V1 COBS/CRC
adapter over binary stdin/stdout and reports lifecycle, virtual-time, metrics
and physical-semantics metadata on stderr. `physical_applied` remains false;
virtual UPDATE acceptance is software-model evidence only. The demos need no
board, debugger, motor, or Python packages beyond the standard library.

The current profile is `GENERIC_RESEARCH_MOTOR`: 12 V, four pole pairs, H0
200 us, H1 500 us, a double-precision average-value PMSM RK4 plant, quantized
ADC ranks, and a virtual magnetic encoder. It omits MOSFET switching transients,
dead time, thermal drift, real power-stage protection and MCU WCET. CSV and JSON
quality fields are fixed-window research measurements; a missing settling time
is emitted as `null`, and `EXPECTED_DEGRADED` remains distinct from
`CONTROL_QUALITY_PASS`.

## Control-tuning research

The Host-only tuning study is reproducible from the repository root:

```powershell
python tools/falconfoc-cli/control_tuning.py `
  --simulator build/vm01a-simulation/falconfoc-virtual-device.exe `
  --output-dir build/control-tuning-01
```

The runner performs a small, fixed and explainable grid search in the order
Current → Velocity → Position, then freezes the chosen copied profile for
independent holdout and long-horizon confirmation runs. Candidate JSON/CSV
files, dominated trials, and the aggregate `study.json` are retained. The
profile override options are consumed only by the Host virtual-device
initialization path; they do not change production PI defaults, Board1
calibration, UART V1, Runtime Safety, Feedforward admission, or output policy.

The final study reports `FEASIBLE`/`INFEASIBLE` constraints separately from
`CONTROL_QUALITY_PASS`, `CONTROL_QUALITY_FAIL`, and `EXPECTED_DEGRADED` metrics.
An improved Host candidate therefore cannot be read as a physical-motor or
AT32F415 acceptance result.

The JSON now carries `metrics_schema_version=2`, historical full-window values,
state-qualified RUNNING and tail windows, transient/steady-state metrics and an
explicit measurement-window description. `--horizon long` runs the independent
100 ms velocity and 2 s position observations. Fault results separate software
INHIBIT latency from FAULT_LATCHED latency and export event-level UPDATE
counters; `-1` timestamps in CSV mean that an event was not observed.

The Host test suite runs a paired Load A/B counterfactual with identical
commands and virtual time. This is separate from the historical `speed_drop`
field, which remains available for audit comparison.

Every response prints a JSON object. The simulator exchanges binary pipe bytes
with the actual C protocol, UART buffers and Runtime adapter. Its clock is fixed
at zero and it has no valid sensor measurements. Real-time timeout scenarios
are covered by the C tests with advancing fake time.

On a separately authorized hardware session, `--port COM3 request status`
selects the physical transport, automatically performs HELLO and sends the
requested operation. That command opens the port. This implementation has only
been exercised with the C simulator.

Command construction examples

```powershell
python tools/falconfoc-cli/falconfoc_cli.py encode setpoint --session 1 --sequence 3 --first 0.1 --second 0.2
python tools/falconfoc-cli/falconfoc_cli.py encode setpoint --session 1 --sequence 4 --mode velocity --first 2
python tools/falconfoc-cli/falconfoc_cli.py encode param-write --session 1 --sequence 5 --id 2 --type f32 --value 1
```

The device rejects ARM/START with OUTPUT_LOCKED. A setpoint acceptance reports
its Runtime publication identity. Query status for H0 observation, faults and
the software result. Physical applied is always false in the locked image.
V1 mode selection is constrained to the current Runtime mode. Parameter IDs,
response layouts and session semantics are specified in the
[protocol documentation](../../protocol/README.md).
