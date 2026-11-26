# FalconFOC Host Virtual Motor Quick Start

This page is the independent developer entry point for the VM-01a through
VM-01d Host reference product. It does not require a board, debugger, motor,
power supply, or real serial port.

## Environment

- Windows PowerShell.
- CMake 3.25 or newer.
- Ninja.
- A C compiler supported by the repository CMake files; the verified Host
  workflow uses Clang 22.
- Python 3.13 in the verified run. The CLI uses the Python standard library;
  no Pandas, Matplotlib, or external Python package is required.

Build the standalone project from the repository root:

```powershell
cmake -S simulation -B build/vm01a-simulation -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/vm01a-simulation --parallel 4
ctest --test-dir build/vm01a-simulation --output-on-failure
```

The simulation uses the production Runtime, Device, Safety, Current PI,
Velocity PI, Position P, Encoder and V1 Serial Adapter. The plant is a
double-precision average-value PMSM with RK4 integration; ADC ranks and a
virtual magnetic encoder are quantized before Runtime consumption. H0 is
200 us, H1 is 500 us, and the default profile is
`GENERIC_RESEARCH_MOTOR` (12 V, four pole pairs, research R/L/flux/inertia
values).

The model does not represent MOSFET switching transients, dead time, real
thermal drift, power-stage protection, real ADC noise, MCU WCET or physical
bridge shutdown. INHIBIT is modeled as a high-impedance average-value output;
`physical_applied` is always false.

## Run the five acceptance scenarios

```powershell
$device = "build/vm01a-simulation/falconfoc-virtual-device.exe"
python tools/falconfoc-cli/falconfoc_cli.py --simulator $device virtual-demo --virtual-scenario current --duration-us 10000 --csv build/vm01a-simulation/current.csv --json build/vm01a-simulation/current.json
python tools/falconfoc-cli/falconfoc_cli.py --simulator $device virtual-demo --virtual-scenario velocity --duration-us 10000
python tools/falconfoc-cli/falconfoc_cli.py --simulator $device virtual-demo --virtual-scenario position --advance-us 500 --duration-us 1200000 --csv build/vm01a-simulation/position.csv --json build/vm01a-simulation/position.json
python tools/falconfoc-cli/falconfoc_cli.py --simulator $device virtual-demo --virtual-scenario load --duration-us 30000 --csv build/vm01a-simulation/load.csv --json build/vm01a-simulation/load.json
python tools/falconfoc-cli/falconfoc_cli.py --simulator $device virtual-demo --virtual-scenario fault --duration-us 20000 --csv build/vm01a-simulation/fault.csv --json build/vm01a-simulation/fault.json
```

The process keeps one V1 session alive through HELLO, SETPOINT, ARM, START,
telemetry refresh, and STOP. Mode selection, load schedule, sensor-fault time
and virtual-time step are Host process controls; they are not new UART V1
messages.

The short velocity and position commands preserve the historical VM-01d
demonstrations. Use `--horizon long` for independent 100 ms velocity and 2 s
position observations; long results are separate artifacts and do not replace
the short-horizon evidence.

`load` uses `0.03 N·m` from `5000 us` to `15000 us`. Override it with
`--load-on-us`, `--load-off-us`, and `--load-torque-nm`. `fault` injects one
Encoder transport failure at `5000 us`; override it with `--sensor-fault-us`.

## Interpret the result files

CSV rows contain virtual time, mode, Safety state, command sequence, Motion
Session, current and motion targets/measurements, bus voltage, saturation,
accepted/rejected UPDATE counts, fault bits, output action, load torque and
event-level fault timestamps. JSON adds the historical full-window RMSE plus
state-qualified RUNNING RMSE/Tail RMSE, transient peak, steady-state error,
Settling Time, saturation duration, integration status, control-quality status,
final state and replay hash. `metrics_schema_version=2` identifies the new
measurement-window fields; old `rmse` and `tail_rmse` remain for comparison.

The result categories are intentionally separate:

- `INTEGRATION_PASS`: the real Runtime/Plant chain ran and accepted output.
- `CONTROL_QUALITY_PASS`: fixed research tolerance and settling rule passed.
- `CONTROL_QUALITY_FAIL`: the measured tracking result missed the fixed rule.
- `EXPECTED_DEGRADED`: a documented research-profile or finite-envelope result
  retained from VM-01b; it is not a product-quality pass.
- `NOT_MEASURED`: a fault or safety termination ended the tracking window.

Settling time is `null` when the error never enters and remains inside the
fixed tolerance. The fault demo additionally reports the injection time,
software fault response delay, and whether accepted UPDATE count stopped after
INHIBIT.

The separate `falconfoc-virtual-current.exe` and
`falconfoc-virtual-motion.exe` programs remain available for focused VM-01a and
VM-01b replay tests. The locked Board1 simulator remains available through
`build/sw04-host/foc-serial-sim.exe`; it must continue to return
`OUTPUT_LOCKED` for ARM/START and `physical_applied=false`.

## Host tuning study

Run the fixed, explainable Current → Velocity → Position search with:

```powershell
python tools/falconfoc-cli/control_tuning.py `
  --simulator build/vm01a-simulation/falconfoc-virtual-device.exe `
  --output-dir build/control-tuning-01
```

The study keeps all candidate JSON/CSV/log files, freezes one copied Host
profile, and runs independent Holdout plus exact 100 ms/2 s confirmation
scenarios. The selected values are research evidence only; they are never
written to production defaults or Board1 calibration.

/* ---------------------------------------- end of file ----------------------------------------- */
