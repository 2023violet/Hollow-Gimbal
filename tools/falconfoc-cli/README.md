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

For the Board1 SIL (production Board1 firmware path against a virtual motor):

```powershell
cmake --build build/sw04-host --target falconfoc-board1-sil
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/sw04-host/falconfoc-board1-sil.exe sil-demo
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/sw04-host/falconfoc-board1-sil.exe sil-demo --sil-lockstep-us 500 --json build/sw04-host/sil.json
```

Scenarios (`--sil-scenario`): `current` (default), `velocity` and `position`
(SET_MODE then tracking), `tuning` (controller writes while DISABLED, refused
while RUNNING, readback and revision, then a tuned run), `recover` (lease
expiry, FAULT_CLEAR, re-run) and `persist` (tune, `save`, power cycle on the same
`--sil-flash` file, restored values and a tuned run, a damaged record refused,
`erase-config`). Single requests use the same names:

```powershell
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/sw04-host/falconfoc-board1-sil.exe request set-mode --mode velocity
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/sw04-host/falconfoc-board1-sil.exe request param-read --param current-kp-q
```

`sil-demo` connects, reads the boot configuration (IDs 32, 33, 45), sends ARM
and START and waits for each to apply at H0, streams a current target
(`--reference`, default 0.4 A, `--duration-us` of virtual time), then STOPs. It
compares the UART-reported Iq with the target and with the plant current at
the ADC aperture, and checks ADC slots, encoder frames and the drive after
STOP. It prints `SIL_PASS` or exits with code 3. The device free-runs at
`--sil-speed` (default 0.02 of wall time) unless `--sil-lockstep-us` is given.

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


## Host motor calibration and FOC

Build the Virtual Motor project from the repository root. The calibration
command uses the C11 product runner in the same project as the V1 virtual
device. It operates on Host profiles and uses Python's standard library.

```powershell
cmake -S simulation -B build/virtual-cal -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build/virtual-cal --target falconfoc-virtual-calibrate
$exe = 'build/virtual-cal/falconfoc-virtual-calibrate.exe'
$cli = 'tools/falconfoc-cli/falconfoc_cli.py'

# Capture and preview measured parameters and gains.
python -B $cli calibrate --simulator $exe --profile-id GENERIC_RESEARCH_MOTOR --cogging

# Review the same fixture and policy, apply afresh, run the control trials, and save.
$reviewId = 'copy-the-review-id-from-CANDIDATE_PREVIEW'
python -B $cli calibrate --simulator $exe --profile-id GENERIC_RESEARCH_MOTOR --approve-candidate $reviewId --cogging --save-config motor.fcal --trace rl.csv --json calibration.json

# Reload against the same profile, then repeat readback and verification.
python -B $cli calibrate --simulator $exe --profile-id GENERIC_RESEARCH_MOTOR --load-config motor.fcal
python -B $cli calibrate --simulator $exe --profile-id GENERIC_RESEARCH_MOTOR --approve-candidate $reviewId --load-config motor.fcal --json restored.json
```

Executable naming depends on the host. `CALIBRATION_SALIENT_MOTOR` is the
second fixture with separate Motor/Encoder identities, opposite raw encoder
direction, ACB phase order and different electrical and mechanical dynamics.
Preview emits `PARAMETERS_IDENTIFIED`, `CALIBRATION_QUALIFIED`,
`CANDIDATE_PREVIEW` and `REVIEW_REQUIRED`. Captured stage records are printed with the result. Use the reported
`review_id` with `--approve-candidate` to authorize the same measured values,
gains, identity and LUTs on a deterministic rerun. A mismatch holds the
Runtime inhibited. Apply uses new Runtime and Encoder storage. The report includes
configuration readback, ± Current/Velocity/Position/Impedance comparisons,
timeout recovery, STOP and `CONTROL_QUALITY_PASS/FAIL`. The reports always
carry `PHYSICAL_CALIBRATION_NOT_VERIFIED`.

`--cogging` adds the optional Host fixture's separate bidirectional capture,
holdout and H1 feedforward. `--trace` exports the separately identified d/q
terminal-voltage intervals and ADC-current endpoints; this CSV is distinct
from Measurement Trace v1. `--cancel-after ELECTRICAL_D` exercises cancellation
without applying or saving a configuration. Rejections carry a machine-readable
`error`, `stage` and `native_status`.

Save creates a new Host FCAL v1 file only after quality verification.
Readback on reload identifies the stored model and applied gains and compares
them against the selected profile's baseline again. FCAL v1 uses little-endian
words, binary32 parameters and CRC32, with profile motor, encoder, frame and
acquisition identity checks. Restore requires explicit approval, validates
parameter ranges and LUT admission, recomputes every gain from the stored
model and local tuning policy, and binds a fresh local output owner. CRC32
checks corruption and supplies the review fingerprint. The fingerprint
identifies a payload for explicit review. UART V1, Schema 2 and MCU Flash use their existing
separate contracts. JSON, CSV and package outputs require new file names.

Run the product CLI acceptance test after a normal Simulation build with
`ctest --test-dir build/virtual-cal -R '^test-foc-virtual-calibration-cli$' --output-on-failure`.
The test requires Python 3. The C runner also accepts the options directly.


The current Host calibration uses a 130 Hz Current PI design request within
its 360 µs sample and 260 µs update-delay budgets. The prepared values
still pass the existing voltage and current admission. The recommendation
also checks bidirectional active-window Current RMS against the default
configuration. Archived FCAL v1 files from the 36 Hz policy may be loaded
with their original review ID and emit their own benefit result. The
historical `virtual-demo` verdict retains its original metrics and adds
state-aware `tracking_error` diagnostics. See [Cycle-level A/B](../../simulation/README.md#control-quality-counterfactuals).

## Host capability and reachability diagnostics

The Host wrapper exposes the same versioned operations as the native calibration
runner. Discover supported virtual profiles, modes, FCAL version and software
limits with `--capabilities`. Add `--startup-trace startup.csv` to an approved
calibration run to capture the existing Current Startup V1 trace.

```powershell
python -B $cli calibrate --simulator $exe --capabilities
python -B $cli calibrate --simulator $exe --profile-id CALIBRATION_SALIENT_MOTOR --load-config salient.fcal --reachability 6.9 0 2.2 12
python -B $cli calibrate --simulator $exe --profile-id GENERIC_RESEARCH_MOTOR --load-config motor.fcal --reachability-torque 0 0 0.30 12
```

The four arguments are signed mechanical speed in rad/s, Id in peak amperes,
signed Iq in peak amperes or shaft torque in N·m, and measured or assumed VBUS
in volts. The torque query uses the Impedance limit and accepts zero Id,
matching the Runtime command contract. The Current query applies the Current
vector cap and reports the modeled torque. A query validates profile identity,
CRC and parameter ranges. It reads the saved model and emits one
`REACHABILITY` V1 event without initializing an active controller. Use a
separate approved load to run the control trials.

The result reports parameter provenance, component voltages, all-angle and
best-angle voltage bounds, named constraints and the conservative Iq and torque
interval at the supplied Id and speed. Use the interval only when
`range_valid` is true. `MODEL_FEASIBLE`, `MODEL_INFEASIBLE` and
`INDETERMINATE` are steady-state verdicts for the supplied parameters.
A successful query can report an infeasible point. The event includes the
control mode to distinguish its current and torque policies. ADC bus admission
and the configured software voltage or duty cap still apply.

The loaded parameters estimate the virtual fixture. A different motor,
parameter drift or load can change the result. The steady-state model uses
zero d/q current derivatives. It leaves step timing, settling, real commutation,
output permissions and physical calibration to their respective evidence.
Runner start failure, timeout and malformed runner output have stable
`HOST_RUNNER` diagnostics. FCAL, runtime control and UART V1 messages retain
their existing formats.

## Host parameter review and reuse

`--stages rs` and `--stages jb` produce `PARAMETER_PREVIEW` with a review ID,
source and capture. Repeat the command with `--approve-candidate <review-id>`
to approve that result. These two modes report `runtime_applicable:false`.

```powershell
python -B $cli calibrate --simulator $exe --stages jb
python -B $cli calibrate --simulator $exe --reuse-session --profile-id GENERIC_RESEARCH_MOTOR
```

The reuse session reads one command per line and flushes a JSON-lines response
ending in `REUSE_COMMAND_RESULT`. Start with `capture`. Use the
`REUSE_PREVIEW.review_id` in `review <review-id>`, then send `apply`. The
`approve <review-id>` command performs both steps. Apply publishes a fresh
Host Runtime after readback and closed-loop verification.

| Command | Behavior |
| --- | --- |
| `plan same` | Show eligible records and their original source, session, capture and revision. |
| `plan encoder`, `plan load`, `plan motor`, `plan adc` | Select a virtual device change and see which records require a new capture. |
| `plan phase`, `plan home`, `plan offset`, `plan lut`, `plan kt`, `plan limits` | Inspect a wiring, coordinate, reference or policy change. |
| `plan voltage`, `plan angle`, `plan torque`, `plan temperature` | Inspect an unsupported reference or operating point. `measure` returns `NEW_REFERENCE_REQUIRED`. |
| `measure` | Capture the required stages and preview the composed configuration. |
| `reject <review-id>` | Reject the current preview and revoke its application authorization. |
| `invalidate <parameter>` | Revoke one record and any pending review. Use names from `REUSE_PARAMETER`. |
| `stop`, `fault`, `recover` | Exercise software INHIBIT, command-timeout fault and restricted recovery. A pending review is revoked. |
| `save <path>` | Exclusively create an FCAL v1 numerical package from a still-qualified applied configuration. |
| `load <path>`, `load <path> <fcal-review-id>` | Inspect or verify a v1 package against the selected fixture, with its original explicit review. Dependencies remain process-local. |
| `quit` | End the session. |

The two profiles are virtual fixtures, including the zero-speed mechanical
origin and their local motion-limit policy. A source change that requires
external data cannot be fulfilled by those fixtures. FCAL v1 does not encode
dependency provenance or local Runtime policy changes. Such policy changes can
be reviewed and applied in memory, with FCAL export refused when local policy
would change the configuration on reload. See the [acceptance evidence](../../docs/evidence/2026-11-20-next-cal-arch-02.md).

## Host flux evidence

The full calibration preview and the reuse session show one
`FLUX_CANDIDATE` event with measured flux, derived Kt and Ke, signed fit
residuals, applied-terminal source, revisions and electrical speed range.
Use `--flux-trace` with a new path to export the accepted terminals and
paired aperture data from a full Host capture.

```powershell
python -B $cli calibrate --simulator $exe --profile-id CALIBRATION_SALIENT_MOTOR --flux-trace salient-flux.csv
```

The reuse review binds the flux evidence to the composed configuration.
FCAL v1 reload retains the numeric compatibility path. Direction quality,
source and dependency metadata remain part of the Host capture or live
reuse session. [FLUX-01 acceptance evidence](../../docs/evidence/2026-11-20-next-cal-flux-01.md)


## Host commutation bootstrap

Use `inspect` for prerequisite status, `known` with an identity-matched
FCAL v1, `offset` with the selected fixture's declared pole count and encoder
direction, or `unknown` to infer all three commutation parameters from
observations. Offset and unknown require an open-circuit scan with enough
shaft travel and terminal signal. The preview reports the coarse reference,
the final post-LUT offset, and any rejected prerequisite or capture.

```powershell
python -B $cli calibrate --simulator $exe --bootstrap inspect
python -B $cli calibrate --simulator $exe --bootstrap unknown --boot-trace bootstrap.csv
python -B $cli calibrate --simulator $exe --bootstrap unknown --approve-candidate <review-id> --save-config motor.fcal
python -B $cli calibrate --simulator $exe --bootstrap known --load-config motor.fcal
python -B $cli calibrate --simulator $exe --bootstrap known --load-config motor.fcal --approve-candidate <fcal-review-id>
python -B $cli calibrate --simulator $exe --reuse-session --bootstrap unknown
```

Use `CANDIDATE_PREVIEW.review_id` from the corresponding preview. A fresh
bootstrap review binds the scan and measured Flux/Kt to the full candidate.
FCAL v1 keeps its numerical fingerprint and requires its own review on reload.
`--boot-trace` creates a new CSV with synchronized scan observations and
identity. Use `--profile-id CALIBRATION_SALIENT_MOTOR` to select the other
fixture. In reuse sessions `capture` uses the measured path, `plan same`
and `measure` retain compatible records, and encoder changes invalidate
the matched phase-dependent fit. The measured bootstrap modes use the
non-cogging calibration lane.

[Bootstrap acceptance and reproduction](../../docs/evidence/2026-11-20-next-cal-boot-01.md)

## Host calibration product modes

The named modes use the existing native runner and explicit review IDs.
Use a new output path when saving a package, CSV or JSON report.

| Mode | Use |
| --- | --- |
| Quick | Validate an identity-matched FCAL v1 and the current Host ADC and encoder chain. After review, apply the numeric configuration and verify all four control modes. |
| Full | Measure all supported parameters using a new open-circuit Bootstrap and the existing stages. Preview before approval, then apply and verify a fresh Host Runtime. |
| Selective | Keep reviewed provenance in the current process. Use `plan encoder`, `plan adc`, `plan motor`, `plan load` or `plan kt` to see the dependency effect. Then measure, review and verify the composed candidate. |

```powershell
python -B $cli calibrate --simulator $exe --calibration-mode full --boot-trace motor-boot.csv
$bootReview = 'copy-the-full-preview-review-id'
python -B $cli calibrate --simulator $exe --calibration-mode full --approve-candidate $bootReview --save-config motor.fcal

python -B $cli calibrate --simulator $exe --calibration-mode quick --load-config motor.fcal
$fcalReview = 'copy-the-quick-preview-review-id'
python -B $cli calibrate --simulator $exe --calibration-mode quick --load-config motor.fcal --approve-candidate $fcalReview

python -B $cli calibrate --simulator $exe --calibration-mode selective
```

In Selective, enter `capture` and `approve <review-id>` to establish qualified
history. Enter `plan <change>`, `measure` and `approve <new-review-id>`
to replace the affected configuration. `plan same` preserves all compatible
measurements. A STOP, fault or invalidated dependency revokes pending review.
After a recoverable fault, use `recover`, plan again and review the new preview.
Use `quit` to end the session. FCAL v1 `load` preserves the numerical
compatibility path. A new process still needs fresh or separately
verified evidence to establish reusable history.

Native `CAL_STAGE` JSON lines report mode, stage, outcome, elapsed simulated
motor time, monotonic Host wall time, process CPU time, completed/reused/skipped
stages, remaining work and review state. The default product stream is live.
Quick and Full with `--json` collect a final JSON report and print the events
after the runner exits. User interaction and ETA remain unmeasured. FCAL
restore emits `PRODUCT_FCAL_SOURCE` with `SOURCE_CONFIDENCE_INSUFFICIENT`
for historical evidence. [Quality acceptance](../../docs/evidence/2026-11-21-next-cal-quality-01.md)
