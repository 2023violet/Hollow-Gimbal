# FalconFOC Host Virtual Motor

`simulation/` is a Host-only VM-01a project. It connects the existing Runtime,
Device, Safety, Current PI, Encoder, and a deterministic average-value PMSM
plant without changing the AT32F415 Board1 target.

The default profile is `GENERIC_RESEARCH_MOTOR`: 12 V VBUS, four pole pairs,
the synthetic R/L/flux/inertia values used by the Host research model, H0 at
200 us, and H1 at 500 us. It is not Board1 calibration data and does not prove
MCU timing or physical output behavior.

## Build and run

```powershell
cmake -S simulation -B build/vm01a-simulation -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/vm01a-simulation --parallel 4
ctest --test-dir build/vm01a-simulation --output-on-failure
build/vm01a-simulation/falconfoc-virtual-current.exe --csv build/vm01a-simulation/vm01a.csv
```

The `falconfoc-virtual-current.exe` executable runs five deterministic Host
scenarios: Current step, Current reverse, rejected UPDATE, STOP, and replay.
The VM-01b `falconfoc-virtual-motion.exe` executable reuses the same harness
for velocity and position control, load disturbance, reverse command, mode
transition, command timeout, and sensor fault scenarios:

```powershell
build/vm01a-simulation/falconfoc-virtual-motion.exe --scenario all
build/vm01a-simulation/falconfoc-virtual-motion.exe --scenario V01-velocity-step
build/vm01a-simulation/falconfoc-virtual-motion.exe --csv build/vm01a-simulation/vm01b.csv
```

The `falconfoc-virtual-device.exe` target connects the same VM-01b Runtime to
the production V1 COBS/CRC/Session/Sequence adapter over binary stdin/stdout.
It is explicitly initialized with `VIRTUAL_ONLY`; its stdout remains protocol
bytes, while lifecycle results and virtual/physical telemetry separation are
reported as JSON metadata on stderr. The Python CLI keeps the process alive for
the complete workflow:

```powershell
python tools/falconfoc-cli/falconfoc_cli.py `
  --simulator build/vm01a-simulation/falconfoc-virtual-device.exe `
  virtual-demo --virtual-scenario current
python tools/falconfoc-cli/falconfoc_cli.py `
  --simulator build/vm01a-simulation/falconfoc-virtual-device.exe `
  virtual-demo --virtual-scenario velocity
python tools/falconfoc-cli/falconfoc_cli.py `
  --simulator build/vm01a-simulation/falconfoc-virtual-device.exe `
  virtual-demo --virtual-scenario position
```

The complete Host acceptance set uses five scenario names:

```powershell
# Current, short control loop with CSV/JSON exports
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/vm01a-simulation/falconfoc-virtual-device.exe virtual-demo --virtual-scenario current --duration-us 10000 --csv build/vm01a-simulation/demo-current.csv --json build/vm01a-simulation/demo-current.json

# Velocity with a fixed mechanical load on/off timeline
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/vm01a-simulation/falconfoc-virtual-device.exe virtual-demo --virtual-scenario load --advance-us 500 --duration-us 30000 --csv build/vm01a-simulation/demo-load.csv --json build/vm01a-simulation/demo-load.json

# Velocity sensor transport failure at a fixed virtual time
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/vm01a-simulation/falconfoc-virtual-device.exe virtual-demo --virtual-scenario fault --advance-us 500 --duration-us 20000 --csv build/vm01a-simulation/demo-fault.csv --json build/vm01a-simulation/demo-fault.json

# Position long run; the 500 us step preserves H0/H1 deadline margins while
# the duration crosses one continuous mechanical revolution.
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/vm01a-simulation/falconfoc-virtual-device.exe virtual-demo --virtual-scenario position --advance-us 500 --duration-us 1200000 --csv build/vm01a-simulation/demo-position.csv --json build/vm01a-simulation/demo-position.json
```

`load` applies `0.03 N·m` from `5000 us` through `15000 us`; the parameters can
be overridden with `--load-on-us`, `--load-off-us`, and `--load-torque-nm`.
`fault` injects one real Encoder transport failure at `5000 us` via
`--sensor-fault-us`. Both timelines act on the Plant or Sensor Adapter; they do
not write speed, position, or Safety state directly.

The CSV contains virtual time, mode/session, measured current, target and
measured motion values, bus voltage, saturation, update counts, faults, load
torque and event-level fault timestamps. The JSON contains the same scenario
identity plus the historical full-window RMSE, state-qualified RUNNING RMSE and
tail RMSE, transient peak, steady-state error, settling time, saturation
duration, integration status, control-quality status and replay hash.
`metrics_schema_version=2` identifies the new fields; historical `rmse` and
`tail_rmse` remain for audit comparison. `EXPECTED_DEGRADED` remains a research
result and is never renamed to a control-quality pass.

The default velocity and position commands are short-horizon demonstrations.
Run `--horizon long` for the independent 100 ms velocity and 2 s position
observations; these do not replace the historical short-horizon artifacts. The
Python Host tests also run a paired Load counterfactual with identical virtual
time and commands and require a measurable Plant delta. A Load no-op mutation
must fail that oracle.

`ARM` and `START` still travel as existing V1 messages. Mode choice and virtual
time advance are process arguments, not wire messages. The metadata distinguishes
`request_accepted`, `request_applied_by_h0`, `virtual_update_accepted`,
`simulated_current_valid`, `simulated_sensor_valid`, and
`physical_applied=false`; existing V1 STATUS/TELEMETRY fields are unchanged.

Both executables use the production Runtime H0/H1 paths. The output adapter
accepts only a validated Host UPDATE. An accepted UPDATE becomes the next
zero-order-held average voltage applied to the Plant. INHIBIT switches to the
documented high-impedance approximation: stator voltage is removed while
inertia, damping, and external load continue to evolve. It is not a physical
bridge shutdown or active-brake model.

Motion metrics are research measurements, not pass/fail product limits. A
scenario whose final error remains outside the stated tolerance is marked
`EXPECTED_DEGRADED`; that marker records a physically or configuration-limited
case and does not turn poor tracking into an acceptance claim.

The Plant state is converted to raw ADC ranks and encoder codes. ADC samples go
through the existing Device decoder; encoder codes go through
`foc_encoder_update()` before H1 publication. The Runtime owns H0/H1/Safety and
Current PI state, so a rejected UPDATE cannot commit a staged PI state.

## Independent periodic phase

The motion-enabled Virtual producer takes directed integer phase count and CPR
from the same accepted Encoder observation as continuous position. Core applies
the **controller Runtime's** pole pairs and offset, rather than substituting
the Plant's true motor parameters. H1 consumes the bounded phase only when its
explicit phase flag and existing source admission both hold. No accumulated
Float32 mechanical position is used to recover phase.

Motion-disabled mode preserves its original direct-angle producer and exact
ADC-time alignment contract; it does not require a configured motion rotor or
the new phase flag. Its legacy angle source keeps the prior precision limits.

Host first-frame compatibility is unchanged: an accepted absolute position can
be used with the existing zero-speed assumption before Encoder difference speed
is established. Encoder `valid`/`speed_valid` are not forged. Normal startup
first accepts UPDATE at 600 us; ARM queued before run_until(0) and START before
200 us can still produce UPDATE at 400 us. This Host-only source policy does
not change Board1's full Feedback admission or its locked output policy.

`test-foc-periodic-chain` traces real signed Encoder updates through Core,
H1/H0/Current/SVPWM and the production virtual adapter at long-count milestones,
including timestamp wrap. It independently checks phase/dq error, continuous
position, raw source identity and startup STOP/FAULT behavior. The virtual
Current tests also execute positive and negative multi-turn trajectories.
The `1e-5 rad` phase budget is a declared Host numerical test budget, not a
hardware accuracy or real-time guarantee.

## System angle error acceptance

The existing `test-foc-periodic-chain` entry also runs `NEXT-SYSTEM-ANGLE-01`.
It reuses the Virtual context/output adapter and real Encoder, H1, H0,
Pipeline and Current implementation. Prescribed Double mechanical trajectories
drive the real virtual encoder-code generator; neither H1 speed nor H0 phase
is overwritten in these acceptance cases. This is kinematic sensor/control
integration with an exactly representable 1 A alpha-axis current, not a motor
response or hardware-quality test.

The test-only `tests/foc_system_angle_oracle.h` supplies analytic motion,
nearest-count quantization and a separate Double difference/LPF reference.
Thirteen deterministic cases cover static/low/constant speed, capture delay,
acceleration/reversal, capture-stamp bias/jitter, ADC sampling jitter,
execution jitter, counter/timestamp wrap, combined disturbances and both four
and seven pole pairs. H0/H1 remain 200/500 us. The test profile selects Board's
650 us prediction policy; production Virtual and Board settings are unchanged.
The 20 ms history establishes Encoder speed before the measured run; the
existing startup/readiness cases remain separate and unchanged.
`assumed_transport_complete_us` declares test input availability at capture+5 us
and checks that H1 runs afterwards. It is not a transport simulation or a
measurement of SPI completion. Capture, H1 publication, ADC sample and H0
execution times remain separate columns.

Error accounting separates quantization, phase mapping, raw-speed estimation,
LPF lag, capture-stamp bias, constant-speed model error and Float32 prediction
error. The independent speed-reference comparison has a 0.001 mechanical
rad/s arithmetic tolerance; it does not promise that the estimated speed is
within 0.001 rad/s of physical truth. The `2e-6 rad` prediction tolerance
likewise covers calculation only. Conditional triangle bounds and actual
combined errors are reported separately; no product total-angle limit or
statistical independence is assumed. Current/Park measurements are checked
against Double rotations and `2 * |I_alpha_beta| * |sin(phase_error/2)|`.
Paired PI calculations use the same pre-step state, currents and target.

Export every accepted measurement, its component errors and timestamps:

```powershell
build/vm01a-simulation/test-foc-periodic-chain.exe --system-angle-csv build/vm01a-simulation/system-angle.csv
```

The normal CTest invocation retains the complete old periodic-chain group and
adds this group; the CSV option runs only the new group. Rejected Encoder
transport/code feedback and an expired accepted source cannot create a new
UPDATE. H1 rejection of a future/duplicate/expired **candidate** retains the
previous accepted snapshot: H0 may still UPDATE while that previous source is
fresh. The tests verify that source identity explicitly. This is not a promise
that every rejected publication immediately latches a fault.
Rejected candidates carry deliberately different phase/speed. Retained logical
payload and Current dq must remain consistent, and the original source expiry
must still inhibit after each rejection.

## Host control-tuning research

`tools/falconfoc-cli/control_tuning.py` runs the deterministic
`NEXT-CONTROL-TUNING-01` study. It reuses the V1 virtual-device workflow and
passes copied Host profile values into `foc_virtual_current_init()` before the
Runtime is initialized. The production defaults, Board1 parameters, UART V1,
Safety, and output lock are not changed.

```powershell
python tools/falconfoc-cli/control_tuning.py `
  --simulator build/vm01a-simulation/falconfoc-virtual-device.exe `
  --output-dir build/control-tuning-01
```

The runner searches Current first, then Velocity, then Position, and stores
every candidate under `optimization/`. It freezes the selected Host profile
for `holdout/` and exact long-horizon `confirmation/` runs. Each trial has
JSON and CSV output plus stdout/stderr logs. `profile_id`, research
parameters, safety feasibility, and metrics are collected in `study.json`.

The supported simulator overrides are Host-only: Current Kp/Ki, Velocity
Kp/Ki, Position Kp, current/voltage/VBUS limits, and selected synthetic motor
parameters. They are initialization-time copies; the Runtime controller
integrators are never edited while a trial is running. A selected profile is
a research candidate, not a production firmware configuration or a real-motor
tuning result.

This project does not call `foc_board_init()`, does not alter
`foc_board_output_submit()`, and cannot unlock real Board1 PWM.

/* ---------------------------------------- end of file ----------------------------------------- */

## Model identification and PI comparison

The Simulation suite includes `test-foc-motor-id-virtual`. It feeds standstill
voltage and current observations from the existing RK4 plant into the portable
calibration API, then runs fixed gains, model-truth gains and fitted-model gains
through the same Runtime-backed Current/Virtual Motor research profile.
Run the test executable with `--trace` to emit per-H0 observations for the
existing Host quality metrics. The q-axis standstill fixture isolates the
electrical axis with zero magnet flux and zero d current. A separate free-rotor
fixture retains magnet flux and verifies rejection. Use `--quality` to
emit voltage, timing and frame qualification rows. The voltage fixture
applies a test-only transfer gain at the RK4 input and shares the resulting
current endpoints across requested- and terminal-voltage fits. The angle
fixture superposes independent d/q trajectories at fixed rotor angle and
rotates their observations. The quality rows report the configured voltage
class. The request-rejected arm submits an unverified class and admits zero
voltage intervals. Virtual Motor defaults and the original `--trace`
experiment retain their behavior. See the
[calibration assumptions](../calibration/README.md#standstill-motor-rl-and-pi-design).

## Impedance diagnostics and Host damping study

The production Core feasibility API has a formal Runtime IMPEDANCE witness
under CTest. Run `test-foc-runtime-impedance-virtual --static-hold --trace`
to export the hold, exact-capacity, overload, tolerance and feedforward data.

`test-foc-impedance-robustness --damping-noise-holdout --trace` exports the
frozen A/B/C comparison. It exercises the production Runtime with a
Host-only damping candidate. It uses a separate experiment entry so the
original 01C and 02A modes remain reproducible. The executable reports
metrics and asserts transaction contracts; the release-quality verdict is
calculated against the frozen gates in the evidence archive.

## Motion-model feedforward experiments

`test-foc-impedance-robustness --motion-feedforward-holdout --trace` compares
one continuous trajectory under identical constraints, with no model term,
qualified reference J/B, and the independent 02A identified model. It uses
the existing IMPEDANCE command and includes STOP, adapter rejection and
sample age witnesses. Use `--motion-feedforward-train` to reproduce training
or `--motion-feedforward-legacy` for the fixed 02B cubic tracking diagnostic.
The legacy probe has a separate result stream. Historical CTest and frozen
quality verdicts retain their original scope.

## Joint motion feedforward and damping experiment

`test-foc-impedance-robustness --joint-control-characterization --trace`
exports the four arms on the frozen cubic reference. Use
`--joint-control-holdout --trace` for the independently frozen cases.
The model term uses the public motion feedforward API. The isolated 48 Hz
operator affects the tested damping term through the existing Host fixture.
The runtime acceptance and output paths still run for every arm. CTest
checks execution and transaction assertions; the evidence tools evaluate
noise and dynamic admission separately.
