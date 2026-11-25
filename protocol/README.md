# FalconFOC UART V1

Portable C11 byte framing with bounded storage. The codec reuses the repository
COBS implementation. The existing CRC module implements MODBUS, so this module
provides the specified CCITT-FALSE calculation.

## Build and verify

```powershell
cmake -S protocol -B build/sw04-protocol -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/sw04-protocol
ctest --test-dir build/sw04-protocol --output-on-failure
```

Fixed [vectors](tests/vectors.tsv) are shared with the Python tool. Columns are
hex message ID, role, sequence, session, payload (a dash means empty), and the
complete wire frame. The vectors include request and response layouts.

## Wire format

A wire frame is COBS of the raw frame followed by one zero byte. Integer fields
and CRC use little-endian byte order. Floats in business payloads use IEEE-754
binary32 LE. Readers use explicit byte operations. CRC is
CRC-16/CCITT-FALSE, polynomial 0x1021, initial 0xFFFF, no reflection, xor-out
zero. It covers all 16 header bytes and the payload. The standard check is
`123456789 → 0x29B1`.

| Raw offset | Size | Field |
| --- | ---: | --- |
| 0 | 2 | Magic 46 46 |
| 2 | 1 | Version 01 |
| 3 | 1 | Message ID |
| 4 | 1 | Role 01 request, 02 response, 04 event. Combinations rejected. |
| 5 | 1 | Reserved 00 |
| 6 | 4 | Sequence |
| 10 | 4 | Logical session |
| 14 | 2 | Payload byte length |
| 16 | 0–96 | Payload |
| 16 + length | 2 | CRC16 |

The largest raw frame is 114 bytes, COBS body 115 bytes and complete wire frame
116 bytes. Empty delimiters do nothing. Oversize frames are discarded through a
delimiter. COBS, CRC, version, magic, reserved and length failures cannot enter
the command adapter. Unknown message IDs may receive UNKNOWN_COMMAND after
integrity validation.

## Sessions, sequence and results

HELLO uses session zero and empty payload. A different HELLO sequence opens a new
volatile session and revokes the old Runtime command. Repeating the current
HELLO sequence returns the same session without extending a command lease. The
CLI chooses a random initial sequence. Session IDs are boot-local counters and
can repeat after reset. CRC and session checks provide no authentication.

All other requests require the current session. The first sequence may be zero.
Later sequences require a modulo delta strictly between zero and 2^31.
Duplicate, old and ambiguous sequences return REPLAY. An admitted request
consumes a sequence even when its business arguments are rejected. Framing
errors, wrong sessions and requests dropped before admission consume no
sequence. Responses echo the request type and sequence.

The first response payload byte is its result.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | OK | Read completed |
| 1 | ACCEPTED | Runtime publication or asynchronous operation accepted |
| 2 | APPLIED | Foreground-owned RAM policy write completed |
| 3–7 | BAD_FRAME, BAD_CRC, UNSUPPORTED_VERSION, UNKNOWN_COMMAND, INVALID_ARGUMENT | Validation failure |
| 8–11 | BAD_SESSION, REPLAY, BUSY, BAD_STATE | Request admission or state failure |
| 12–14 | OUTPUT_LOCKED, NOT_SUPPORTED, INTERNAL_ERROR | Explicit device limitation or internal failure |
| 15 | NOT_CALIBRATED | ARM/START without a valid commutation group (appended) |

Malformed frames are counted and discarded. Business errors use a one-byte
response payload. Damaged bytes never extend a lease.

## Message payloads

All empty requests reject extra bytes. Lengths below include the response result.

| ID | Message | Request payload | Response |
| --- | --- | --- | --- |
| 01 | HELLO | Empty, session 0 | 9-byte capability, new session in header |
| 02 | CAPABILITY_READ | Empty | 9-byte capability |
| 03 | STATUS_READ | Empty | 30-byte status |
| 10 | COMMAND_SETPOINT | mode u8 + d/q f32 for current (9 bytes), mode u8 + reference f32 for velocity/position (5 bytes), or mode 3 + position, velocity, stiffness, damping, torque f32 for impedance (21 bytes); all output-side (see Output frame) | ACCEPTED + Runtime publication u32 (5 bytes) |
| 11 | COMMAND_STOP | Empty | ACCEPTED, query status for H0 acknowledgement |
| 12 / 13 | COMMAND_ARM / COMMAND_START | Empty | OUTPUT_LOCKED on Board1; on Host virtual targets (virtual device, Board1 SIL) ACCEPTED + lifecycle request u32 (5 bytes) |
| 14 | COMMAND_SET_MODE | mode u8 | ACCEPTED + lifecycle request u32 (5 bytes); BAD_STATE unless DISABLED with INHIBIT confirmed |
| 15 | LIFECYCLE_READ | Empty | 25-byte receipt of the latest ARM/START/SET_MODE (below) |
| 20 | FAULT_READ | Empty | 30-byte status including faults and recovery result |
| 21 | FAULT_CLEAR | Empty | ACCEPTED only for command-timeout and motor-protection recovery |
| 22 | FAULT_SNAPSHOT_READ | Empty | 39-byte snapshot latched at the first protection trip (below) |
| 30 | PARAM_READ | ID u16 | 18-byte typed parameter |
| 31 | PARAM_WRITE | ID u16 + type u8 + value u32/f32 | APPLIED + the updated typed parameter |
| 32 | PARAM_SAVE | Empty | APPLIED + stored record sequence u32 (5 bytes); NOT_SUPPORTED on images without storage |
| 33 | CONFIG_ERASE | Empty | APPLIED; storage is erased and the running instance returns to the boot configuration, boot tuning and default RAM policies at once (commutation group invalid). NOT_SUPPORTED without storage |
| 40 | TELEMETRY_READ | Empty | 83-byte status and telemetry |
| 41 | TELEMETRY_STREAM | Empty | NOT_SUPPORTED |
| 50 | CALIBRATE_START | Empty, or stage mask u8: 1 rotor (CAL-1/2), 2 electrical (CAL-3/4/5), 3 both (default), 4 ADC zero only (needs a valid group, no drive), 8 inertia (CAL-7) alone or added to 1–3 (9–11); 8 without 2 needs Rs, Ld, Lq and flux in the profile | ACCEPTED; BAD_STATE unless DISABLED, INHIBIT confirmed and the shaft still (electrical alone also needs a rotor result applied in this boot); OUTPUT_LOCKED on the locked image; NOT_SUPPORTED without a manager |
| 51 | CALIBRATE_READ | Empty | 93-byte calibration progress, results and active configuration (below) |
| 52 | CALIBRATE_ABORT | Empty | ACCEPTED; the drive stops at the next Foreground pass. STOP also aborts |
| 53 | COMMUTATION_WRITE | pole pairs u16 (1–64) + encoder direction i8 (+1/−1) + electrical offset f32 rad [0, 2π) | APPLIED (group source HOST); BAD_STATE unless DISABLED with INHIBIT confirmed; BUSY during a run or pending tuning; INVALID_ARGUMENT out of range |
| 54 | SET_ZERO | Empty | APPLIED + zero offset f32 output rad (5 bytes): the present output position becomes zero (ID 124); BAD_STATE unless DISABLED with INHIBIT confirmed; BUSY without a fresh sensor or during a calibration run; PARAM_SAVE stores it |

Modes are CURRENT 0, VELOCITY 1, POSITION 2 and IMPEDANCE 3. Units are A, mechanical rad/s
and continuous mechanical rad. Setpoints must match the current Runtime mode. IMPEDANCE
applies τ = Kp (p − p_meas) + Kd (v − v_meas) + τ_ff on the output side (rad, rad/s,
N·m/rad, N·m·s/rad, N·m), converted to the rotor with the gear ratio (position and speed
× gear, Kp and Kd ÷ gear², torque ÷ gear) and to q current with Kt = 1.5 × pole pairs ×
flux (ID 84); the live q-current limit bounds it. It needs an identified flux: SET_MODE 3
is refused while ID 84 is 0. The same law serves CAN MIT (contract 26).
The reference firmware starts in CURRENT. COMMAND_SET_MODE selects another mode
while DISABLED; it changes software control state only, so the locked Board1
image accepts it too. Capability differentiates available algorithms from
physical output.

ARM, START and SET_MODE are published to H0 and take effect at its next due
cycle. ACCEPTED means published, not applied. Read the outcome with
LIFECYCLE_READ and match its request identity:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u8 | Result OK |
| 1 | u8 | Receipt NONE 0, PENDING 1, APPLIED 2, REJECTED 3, CANCELLED 4 |
| 2 / 3 | u8 / u8 | Action ARM 1, START 2, SET_MODE 4 / requested mode |
| 4 | u32 | Lifecycle request identity from the ACCEPTED response |
| 8 / 9 | u8 / u32 | Safety state / fault bits after H0 handled it |
| 13 / 17 | u32 / i32 | H0 time in µs / Runtime result explaining a rejection |
| 21 | u32 | Motion session after H0 handled it |

H0 rejects ARM and START until fresh sensing follows boot or STOP, so a Host
retries a REJECTED ARM after a short wait. A pending STOP or a latched fault
cancels a request.

A validated setpoint is stamped once with the MCU time at completed validation.
Runtime publication identities continue across wire sessions. STATUS returns the
last H0-observed publication identity and timestamp. A matching identity with
command-observed set establishes H0 command observation. Fault and pipeline
results describe subsequent admission. Physical applied remains false.

STOP revokes the reference before asking H0 to acknowledge INHIBIT. FAULT_CLEAR
requests recovery of a latch that holds only COMMAND_TIMEOUT, motor-protection
causes (bits 9–15, below) and DRIVER_FAULT (bit 16). H0 rechecks the fault mask, fresh ADC/angle data,
Foreground progress, request age and software inhibition, and for OVER_VOLTAGE or
UNDER_VOLTAGE a fresh bus voltage back inside IDs 114–113, and for DRIVER_FAULT the
nFAULT line released, before invoking its existing clear operation. The clear result is queryable. Other fault masks return
BAD_STATE. Recovery ends in DISABLED with old command and sample validity cleared
and the protection snapshot cleared.

### Motor protection

Fault bits: COMMAND_TIMEOUT 0, SAMPLE_TIMEOUT 1, SAMPLE_INVALID 2, FAST_HEARTBEAT 3,
FOREGROUND_HEARTBEAT 4, INVALID_STATE 5, WDT 6, FLASH 7, FATAL_RESET_REQUIRED 8,
and the motor-protection causes OVER_CURRENT 9, OVER_VOLTAGE 10, UNDER_VOLTAGE 11,
OVER_SPEED 12, SUSTAINED_CURRENT 13, STALL 14, TRACKING_DIVERGENCE 15, and the gate
driver cause DRIVER_FAULT 16.

DRIVER_FAULT: the Board reads the DRV8311 nFAULT line (PB2) once per H0 cycle; two
samples in a row with the line low latch DRIVER_FAULT with INHIBIT in any state,
including DISABLED and calibration. The DRV8311H disables its bridge itself on
over-current, supply under-voltage or thermal shutdown and retries after 5 ms; the
latch keeps the output inhibited until FAULT_CLEAR after the line is released. It
carries no FAULT_SNAPSHOT_READ record.

H0 evaluates the rules after every accepted running cycle and latches a trip with
INHIBIT (at most one 200 µs cycle late; the DRV8311 over-current protection is the
faster hardware path):

| Rule | Condition | Modes |
| --- | --- | --- |
| Over-current | Largest absolute phase current above ID 112 | All, including CALIBRATION |
| Over / under voltage | Bus above ID 113 or below ID 114 for 3 consecutive samples | All |
| Over-speed | Absolute mechanical speed above ID 115 (0: 1.25 × ID 72) | CURRENT, VELOCITY, POSITION |
| Sustained current | Measured d/q current magnitude ≥ 0.95 × ID 68 for ID 116 | CURRENT, VELOCITY, POSITION |
| Stall | Current magnitude ≥ 0.95 × ID 73 at a speed below 0.05 × ID 72 for ID 117 | VELOCITY, POSITION |
| Tracking divergence | In a window of ID 118, the outer q-current reference ≥ 0.95 × ID 73 for at least half of the samples, and the window's mean speed error is not at least 2% below the previous such window's | VELOCITY, POSITION |

A wrong commutation (a stored profile on a replaced motor, or the nominal
placeholder) shows up as sustained current, over-speed or tracking divergence in
VELOCITY and POSITION. In CURRENT mode a wrong electrical offset can reverse the
torque at a normal current; no rule detects that, so a replaced motor must be
recalibrated.

FAULT_SNAPSHOT_READ returns the measurements of the first trip since the last
applied clear:

| Byte | Type | Field |
| --- | --- | --- |
| 1 / 2 | u8 / u8 | Valid / mode |
| 3 / 7 | u32 / u32 | Protection fault bits / H0 time µs |
| 11 / 15 | f32 / f32 | Mechanical speed / outer speed reference rad/s (0 without one) |
| 19 / 23 / 27 | f32 | Measured id / iq / outer q-current reference A |
| 31 / 35 | f32 / f32 | Largest phase current A / bus voltage V |

## Capability and status layout

Capability response bytes are result, protocol version, three firmware version
bytes, algorithm mode bitmask, physical mode bitmask, and capability u16.
Current values are protocol 1, firmware 0.1.0, algorithms 15 (bit 3 IMPEDANCE), physical modes 0,
and capabilities 63 plus 64 for persistent storage and 128 for on-MCU
calibration. Capability bits are output lock 0, RAM policy parameters 1,
polling telemetry 2, UART transport 3, remote mode switch 4, controller tuning
5, persistent settings 6 and on-MCU calibration 7.

| Status offset | Type | Meaning |
| --- | --- | --- |
| 0 | u8 | Result |
| 1 / 2 | u8 / u8 | Safety state / Runtime mode |
| 3 | u8 | Flags below |
| 4 | u8 | Software output action |
| 5 / 9 | u32 / u32 | Fault bits / MCU time in µs modulo 2^32 |
| 13 | i32 | Pipeline result |
| 17 | u8 | Recovery result NONE 0, PENDING 1, APPLIED 2, REJECTED 3 |
| 18 / 22 / 26 | u32 | Sample source time / command source time / Runtime publication sequence |

Status flag bits are LOCKED 0, software-output acknowledgement 1, fresh sensor 2,
H0-observed command 3, inhibit pending 4, RAM dirty 5, Fast seen 6 and sample
seen 7. Timestamp zero is legal, so the seen flags matter. Ages use the existing
half-range Runtime rules. Software acknowledgement does not certify a physical
bridge transition.

TELEMETRY appends f32 d current at 30, f32 q current at 34, f32 output
position at 38, f32 output velocity at 42 (see Output frame), u32 sensor source time at 46, u32 CRC errors
at 50, u32 dropped frames at 54, u32 RX overflow/error events at 58, and a validity byte
at 62. Its bits are physical applied 0, calibrated current valid 1, sensor
calibrated 2. In the locked Board1 image all three bits and the current fields
stay zero; see the current-field note under Parameters. Position/velocity preserve their H0-consumed source time
and are valid only when the fresh-sensor flag is set. Their calibration remains
unverified.

## Parameters

The complete list (ID, name, type, unit, access, range, saved) is generated from
`firmware/at32f415/parameters.json` into [docs/parameters.md](../docs/parameters.md);
it takes precedence over any table here. CANSimple SDO uses the same IDs. This
section describes the implemented parameters. The planned grouping, IDs,
write rules and persistence of future parameters are in the
[parameter contract design](../docs/superpowers/specs/2026-11-22-falconfoc-v2-parameter-contract-design.md);
nothing there is implemented until it appears here.

A parameter response is result, ID u16, type u8, unit u8, attributes u8, minimum,
maximum and value. The final three values occupy four bytes each and use the
declared type. Types are u32 1 and f32 2. Units are dimensionless 0, A 1, rad 2,
rad/s 3, raw ADC counts 4, ... (full list in [docs/parameters.md](../docs/parameters.md)
and `scripts/gen/gen_parameters.py`; N·m 22 and N·m/rad 23 were added for MIT). Attribute bits are writable 0, applied to RAM 1 and
calibration required 2.

| ID | Parameter | Type | Range / initial value |
| --- | --- | --- | --- |
| 1 | Bytes examined per Foreground protocol poll | u32 | 8–32 / 24 |
| 2 | Remote current-vector magnitude cap | f32 A | 0.05–2.5 / 2.5 |
| 3 | Absolute remote position-reference cap, output-side | f32 rad | 1–1000 / 100 |
| 16 | Pole pairs | u32, read only | Board1 nominal 7 |
| 17 | Electrical offset | f32 rad; writable as a trim of a valid commutation group (BAD_STATE without one) | 0–2π / uncalibrated nominal 0 |
| 18 | Velocity-reference limit | f32 rad/s, read only | Runtime configured 12 |

IDs 32–45 are a read-only startup readback, added without changing the frame
format, message IDs, response length or field meaning. They exist only after a
Board1 startup selection completed and the adapter was given its report;
otherwise they return INVALID_ARGUMENT like any unknown ID. Values are read from
the live Runtime and encoder policy. Writes return NOT_SUPPORTED. Minimum and
maximum equal the value. IDs 16 and 17 report the effective pole pairs and
electrical offset of the selected configuration.

| ID | Readback | Type | Values |
| --- | --- | --- | --- |
| 32 | Effective configuration | u32 | 0 none, 1 NOMINAL_CONFIGURATION, 2 CALIBRATED_CONFIGURATION |
| 33 | Calibration input | u32 | 1 NOT_AVAILABLE, 2 ACCEPTED, 3 REJECTED |
| 34 | Rejection | u32 | 0 none, 1 format, 2 supply identity, 3 coordinate, 4 admission, 5 snapshot mismatch, 6 Board identity, 7 Board range, 8 direction product, 9 initialization, 10 missing review reference (appended; 0–9 unchanged) |
| 35 | Admission failure | u32 | `foc_calibration_admission_failure` code, 0 none |
| 36 | Supply identity | u32 | 0 when absent |
| 37 | Review reference | u32 | 0 when absent; nonzero for every accepted supply |
| 38–40 | Phase A/B/C ADC offset | f32 counts | Effective decode offsets |
| 41 | Encoder counts per revolution | u32 | Effective Sensor policy |
| 42 | Runtime encoder direction | u32 | 1 = +1, 2 = −1 |
| 43 | Raw source encoder direction | u32 | 1 = +1, 2 = −1 |
| 44 | Calibration direction | u32 | 0 none, 1 = +1, 2 = −1 |
| 45 | Physical current measurement qualified | u32 | Always 0 in the locked image |

ID 36 is compared with the build's independent expected supply identity. ID 37
is only a reference to the review material: its presence is required, but it is
not matched against any expectation, not signature-verified and not a proof of
physical provenance.

Accepting calibration parameters does not qualify physical current
measurement.

TELEMETRY_READ bytes 30–37 carry the firmware's own decoded d/q current (A) of
the latest accepted control cycle, and byte 62 bit 1 is set while that cycle
was an accepted UPDATE. The response is 75 bytes: bytes 63–70 carry the d/q
current target (A) that cycle tracked (in VELOCITY and POSITION this is the
outer loop's demand) and bytes 71–74 the controller tuning revision (u32). Both are zero when no UPDATE is accepted, which is
always the case in the locked Board1 image. Bit 0 (physical output applied)
and bit 2 (calibrated current sensing) stay zero; ID 45 alone reports physical
current-measurement qualification.

Writes require a coherent DISABLED view with software inhibition acknowledged
and no pending stop. The three writable policies (IDs 1–3) belong to Foreground
and take effect together with their APPLIED response. Unknown IDs, incorrect
types, NaN, Inf and range violations are rejected. Dirty means RAM changed
since the last boot or save. Without PARAM_SAVE a restart restores the boot or stored values.

Controller tuning (IDs 64–74) belongs to H0. A write returns ACCEPTED with the
requested value once the complete candidate (the live tuning with that one field
replaced) passed the same checks as Runtime initialization. H0 applies it at
its next cycle, only while still DISABLED with INHIBIT confirmed, and counts it
in the revision. ID 61 is the receipt (NONE 0, PENDING 1, APPLIED 2, REJECTED 3,
CANCELLED 4) for request ID 62; ID 60 and TELEMETRY bytes 71–74 carry the
revision. A write while another is pending returns BUSY. The current limit may
be lowered below its boot value but never raised above it; the q-current limit
stays at or below the current limit.

### On-MCU calibration

CALIBRATE_START runs CAL-1 and CAL-2 in the firmware:

1. CAL-1 ADC zero: with the output inhibited and the shaft still, H0 feeds 256
   valid ADC frames to the current-zero estimator. A zero whose noise exceeds 4
   counts rms is refused here, before the motor is driven. The samples are
   declared zero-current because INHIBIT is confirmed and the shaft is still
   (open bridge, no back-EMF); nothing measures the current independently.
2. CAL-2 rotor mapping: the manager selects the internal CALIBRATION mode,
   ARMs and STARTs, ramps a d-axis voltage at electrical angle 0 until the
   decoded current reaches 0.5 A, then sweeps the commanded angle two electrical
   turns forward and back at half a turn per second, pairing each angle with the
   encoder position. Forward and backward points at the same angle are averaged
   (the drive lag cancels); the estimators fit direction, pole pairs (1–30) and
   electrical offset.
3. The existing admission builds one snapshot; the instance is re-initialized
   with it and the controller tuning re-applied. All results apply, or none.
4. CAL-3 Rs, Ld, Lq: voltage-mode FOC commutated by the calibrated sensor angle
   applies d-axis pulses (no torque) and a zero-mean q-axis pulse train (the free
   shaft only dithers), 768 H0 intervals each, fitted by `foc_motor_id`.
5. CAL-4 flux and Kt: q-axis voltage levels a, 2a, −a, −2a with
   a = min(15 % of the voltage limit, 0.2 × current limit × the CAL-3 Rs), so the
   reversal step (about 3a / Rs against the running back-EMF) stays within 60 % of
   the current limit on low-resistance motors; each level settles for 400 ms and
   its steady part (4 windows of 32 intervals) is one segment for `foc_flux_id`.
   Kt = 1.5 x pole pairs x flux.
6. CAL-5: `foc_motor_pi_design` (ID 104 bandwidth, 200 µs period)
   designs the current PI; H0 applies it through the tuning mailbox and the
   Runtime returns to CURRENT mode.

The electrical stages run at the H0 rate on the logical slot clock. The
interval voltage is reconstructed from the applied duties and bus voltage
(0.8 V(k-2) + 0.2 V(k-1) for the TMR1 preload) and declared `COMMANDED`: on
hardware it includes dead-time and switch-drop error, which biases Rs. With
200 µs intervals the R/L fit accepts Rs 0.03–10 Ω and L 0.05–50 mH, and refuses a
fitted pair whose pole the interval does not resolve (interval × Rs / L above
0.3, i.e. Rs / L above 1500 /s).

While a run is active, SETPOINT, ARM, START, SET_MODE, PARAM_WRITE, PARAM_SAVE,
CONFIG_ERASE, FAULT_CLEAR and CALIBRATE_START return BUSY. STOP or
CALIBRATE_ABORT ends the run as FAILED (aborted).

A run commits in two phases, and only these:

- Rotor phase (CAL-1/2) commits at APPLY, when the admitted snapshot has
  re-initialized the instance. It stays in RAM if a later stage fails or is
  aborted.
- Electrical phase (CAL-3/4/5) commits at TUNE, when H0 has applied the
  designed current PI. R/L and flux results are reported, never applied on
  their own.
- Inertia phase (CAL-7) commits at the same TUNE: J and B join the profile and
  the refresh derives the outer gains in the same candidate.

A run that ends FAILED after it selected the CALIBRATION mode first passes
through RESTORE: it waits for the stop and returns the Runtime to CURRENT mode
(as a successful run does), then reports FAILED with the original reason. Byte
71 states what this run committed; bytes 73–92 state what is active. PARAM_SAVE
stores the active rotor calibration and the tuning (which carries the CAL-5 PI)
whatever the outcome of the latest run. After a boot that restored a
calibration, the electrical stages may run alone: CAL-3 then first ramps the
d-axis voltage on the rotor angle until 0.5 A to find its excitation scale.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u8 | Result OK |
| 1 / 2 / 3 | u8 | State, failure, state at failure (below) |
| 4 | u8 | Admission failure code when failure is 9 |
| 5 / 7 | u16 / u16 | Sweep samples / fitted pole pairs |
| 9 / 10 | i8 / i8 | Fitted direction / Runtime encoder direction of the applied result (0 before) |
| 11 / 15 | f32 / f32 | Electrical offset rad / rotor fit rms residual rad |
| 19 / 23 / 27 | f32 | ADC zero A / B / C in counts |
| 31 / 35 | f32 / u32 | Largest ADC noise rms in counts / applied runs |
| 39 / 43 / 47 | f32 | Rs Ω / Ld H / Lq H |
| 51 / 55 | f32 | Flux Wb / Kt N·m per peak q ampere |
| 59 / 63 / 67 | f32 | Designed current Kp d / Kp q V/A / Ki V/(A·s) |
| 71 | u8 | Committed by the latest run: bit 0 rotor phase, bit 1 electrical phase |
| 72 | u8 | Results the latest run produced: bit 0 rotor fit, 1 R/L, 2 flux, 3 PI applied |
| 73 | u8 | Active rotor calibration source: 0 boot configuration, 1 a run in this boot, 2 restored from storage, 3 COMMUTATION_WRITE, 4 accepted build supply |
| 74 | u8 | Largest backlog of the H0 capture ring (64 entries) in the latest run |
| 75 / 77 | u16 / f32 | Active pole pairs / electrical offset rad (Runtime) |
| 81 / 85 / 89 | f32 | Active ADC zero A / B / C in counts (Runtime) |

Bytes 5–70 describe the latest run (zero for what it did not reach: an
electrical-only run reports no rotor fit). Bytes 73–92 describe the instance
that is running, including a calibration restored at boot.

States: IDLE 0, ADC_ZERO 1, DRIVE_MODE 2, DRIVE_ARM 3, DRIVE_START 4, ALIGN 5,
SWEEP 6, DRIVE_STOP 7, APPLY 8, DONE 9, FAILED 10, RL_D 11, RL_Q 12, FLUX 13,
TUNE 14, RESTORE 15, INERTIA 16, COAST 17. Failures: none 0, state 1, ADC 2, lifecycle 3,
drive 4, current 5, sensor 6, rotor 7, offset 8, admission 9, apply 10, aborted
11, timeout 12, R/L 13, flux 14, tune 15, capture 16, inertia 17. Byte 4 carries
the admission failure, or for R/L, flux and inertia failures the estimator's
`foc_calibration_failure`. Byte 71 adds bit 8 (CAL-7 committed), byte 72 bit 16
(inertia result present). After DONE, IDs 16, 17, 38–42, 64–67, 69–71 and 80–86
read the applied values.

CAL-7 drives a q voltage of the CAL-4 magnitude on the rotor angle and reverses it
when the rotor passes half of ID 109 from where it started, or 80 % of ID 72, so
the rotor accelerates and brakes in turn within ID 109 (the reversal brakes at
least as hard as the start accelerated). A probe of three reversals sets the fit
window to a tenth of the time between reversals (5–500 H0 periods); the capture
then spans four more reversals and at least 40 windows (at most 12 s). Each H0
period's speed and Kt × iq feed `foc_mechanical_id`; TUNE writes J and B (IDs
85/86) and the unified refresh derives IDs 69–71. Without three reversals in
10 s (a very heavy or damped load at 20 % current) the run ends TIMEOUT.

Coast check (state 17, electrical stage without CAL-7, phase-voltage reader
attached): after the fourth CAL-4 level the drive spins up (at most 0.6 × the
voltage limit and an EMF of 80 % of ID 72, the current step within 60 % of the
current limit), settles, and turns the bridge off. With no winding current the
phase dividers see the EMF alone. Each millisecond the line-to-line voltage of the
two highest phases (the lowest may sit on a body-diode clamp) is fitted against
the encoder angle and a central-difference speed, giving the flux and the
electrical angle error. A usable fit replaces CAL-4's flux. Read-only results of
the latest run: ID 91 u32 status (0 not run, 1 used, 2 too few samples, 3
inconsistent), ID 92 f32 coast flux Wb, ID 93 f32 angle error rad (firmware angle
minus true), ID 94 f32 the driven CAL-4 flux Wb. The check never fails a run.

CAL-3 also identifies the inverter dead-time voltage on the still d axis (each
phase loses Vdt × sign(i), with the sign clamped linearly within ±30 mA) and takes
it out of the q-axis and CAL-4 voltages. ID 95 f32 reads Vdt of the latest run in
V (zero when not identified).

ID 87 f32 V (0–1, default 0, saved, applied by H0): dead-time compensation of the
current loop. Each phase adds ID 87 × the reference-current sign (ramped over
±30 mA); anti-windup treats it like the proportional term. Calibration does not
set it; the Host enables it, typically by writing the value of ID 95.
CAL-4 also ends TIMEOUT when a speed level is not steady in its direction within
10 s (SIL: J above about 0.05 kg·m² for the 0.105 N·m/A plant).

### Persistent settings

### Output frame

Host positions and speeds are output-side: SETPOINT velocity and position targets,
TELEMETRY bytes 38/42 and the remote position cap (ID 3). The Runtime, the speed
limits (IDs 72, 98, 115), protection and FAULT_SNAPSHOT_READ stay rotor-side.

    output position = rotor position / gear ratio − zero offset
    output velocity = rotor velocity / gear ratio

| ID | Parameter | Type and unit | Range / initial |
| --- | --- | --- | --- |
| 124 | Output-position zero offset | f32 rad (unit 2), output-side | ±1000 / 0 |

A velocity target times the gear ratio must stay within ID 72 (INVALID_ARGUMENT
otherwise). IDs 99 and 124 and SET_ZERO need DISABLED with INHIBIT confirmed, like
every parameter write, so a held target never jumps. TELEMETRY bytes 75/79 carry
the rotor position and velocity (appended). Board1's encoder is single-turn
absolute and a re-initialization (calibration, COMMUTATION_WRITE, CONFIG_ERASE)
restarts the rotor position at the single-turn angle, so the stored zero gives the
same output position after a power cycle only within one rotor turn
(2π / gear ratio output rad). CONFIG_ERASE returns IDs 99 and 124 to 1 and 0.

PARAM_SAVE and CONFIG_ERASE require DISABLED with INHIBIT confirmed and no
pending tuning. PARAM_SAVE stores the live controller tuning (IDs 64–74), the
motor profile (IDs 80–83, 85, 86, 96–99, 104), the protection thresholds (IDs
112–118), the RAM policies (IDs 1–3), the output zero (ID 124) and, when one is
applied, the on-MCU calibration result (ADC zero, direction, pole pairs,
electrical offset) in one record (format 6, 232 bytes: the format-2 bytes 0–135,
then the profile at 136–179, the thresholds at 180–207, the output zero at 208,
acceleration and deceleration (IDs 106/107) at 212/216, IDs 108/109 at 220/224
and the CRC at 228). Format-5 (224 bytes, boot IDs 108/109), format-4 (216 bytes,
no acceleration limits), format-3 (212 bytes, no output
zero), format-2 (140 bytes) and format-1 records still load; formats 1 and 2 keep
the boot profile and thresholds. The
record is bound to the image's boot motor configuration. A restore applies the
stored PI as saved; it does not re-derive it from the model. At boot, before any interrupt, a stored
record is applied only when that boot configuration is identical and every
value passes the same checks as a UART write and the calibration passes the
same re-initialization as a live run; it applies completely or not at all,
otherwise the boot configuration stays in use. The record is bound to the
board's boot configuration, not to a motor: nothing on Board1 identifies the
motor, so after replacing the motor or moving the encoder, send CONFIG_ERASE
or recalibrate. ID 51 = 1 means "restored", not "verified on this motor".
Two slots and a separately programmed commit word keep the previous record
when a save is interrupted. A successful save clears the dirty flag.

| ID | Readback | Values |
| --- | --- | --- |
| 48 | Boot outcome | 0 unavailable (no storage), 1 empty, 2 loaded, 3 invalid record, 4 identity mismatch, 5 value rejected, 6 I/O error |
| 49 | Applied record sequence | 0 when none was applied |
| 50 | Newest stored record sequence | 0 when storage is erased |
| 51 | Calibration restored | 1 when the applied record carried a calibration that was re-applied at boot (0 after CONFIG_ERASE) |

### Commutation gate

| ID | Readback | Values |
| --- | --- | --- |
| 88 | Commutation group valid | 1 unless the group is the boot placeholder |
| 89 | Group source | as CALIBRATE_READ byte 73 |
| 90 | ADC zero of this boot | 0 pending, 1 ready, 2 last measurement refused (retried after 1 s) |

The commutation group is pole pairs, encoder direction and electrical offset. It is
valid when it came from a calibration run, storage, COMMUTATION_WRITE or an accepted
build supply; the nominal placeholder (7 pole pairs, offset 0) is not. ARM and START
return NOT_CALIBRATED without a valid group, and BUSY until this boot's ADC zero is
measured or while a calibration run owns the Runtime (a Host retries BUSY). The zero
is measured on the MCU: with a valid group and the output inhibited, the manager
runs a zero-only CAL-1 (256 frames, at most 4 counts rms) 100 ms after the group was
installed and replaces the group's ADC zero by re-initialization; a rotor
calibration sets it as well. The ADC zero stored in a record serves only until that
measurement. COMMUTATION_WRITE imports a group exported from another board or a
bench calibration (IDs 16, 17, 42); the measured zero stays this board's. CAL-2
identifies 1–30 pole pairs; COMMUTATION_WRITE accepts 1–64.

### Motor profile

| ID | Parameter | Type and unit | Range / Board1 initial |
| --- | --- | --- | --- |
| 80 | Phase resistance Rs | f32 Ω (unit 11) | 0 unset, or 0.001–100 / 0 |
| 81 / 82 | d / q inductance | f32 H (unit 12) | 0 unset, or 1e-6–1 / 0 |
| 83 | Flux linkage | f32 Wb (unit 13) | 0 unset, or 1e-5–1 / 0 |
| 84 | Torque constant Kt = 1.5 × pole pairs × flux | f32 N·m/A (unit 14), read only | derived |
| 85 / 86 | Inertia / viscous damping, rotor-side (CAL-7 or written) | f32 kg·m² (unit 15) / N·m·s/rad (unit 16) | 0–1 / 0; 0–10 / 0 |
| 96 / 97 | Rated / peak current, rated at most peak | f32 A | 0 unset, or 0.05–1000 / 0 |
| 98 | Maximum mechanical speed | f32 rad/s | 0 unset, or 0.1–1000 / 0 |
| 99 | Gear ratio (rotor turns per output turn): the output frame below | f32 | 1–1000 / 1 |
| 104 | Current-loop bandwidth | f32 Hz (unit 17) | 10–235 / 150 |
| 106 / 107 | Acceleration / deceleration, rotor-side, 0 unlimited | f32 rad/s² (unit 21) | 0, or 0.1–100000 / 0 |
| 108 | Velocity-loop bandwidth for the derived outer gains, 0 manual | f32 Hz (unit 17) | 0, or 0.5–100 and at most ID 104 / 5 / 10 |
| 109 | CAL-7 travel: largest rotor excursion from the start | f32 rad (unit 2) | 0.5–1000 / π |

With inertia (ID 85) and flux (ID 83) set, the unified refresh derives the outer
gains at ID 108 (ω = 2π × ID 108, Kt = 1.5 × pole pairs × flux): velocity Kp
(ID 69) = J·ω / Kt, Ki (ID 70) = Kp·ω / 4, position Kp (ID 71) = ω / 4. Writing
IDs 83, 85, 104 or 108, or a CAL-7 run, re-derives them; a bandwidth above
ID 104 / 5 or 100 Hz is INVALID_ARGUMENT. IDs 69–71 stay writable as a manual
override until the next derivation. While the planner shapes a reference, H1
adds (J × planned acceleration + B × planned velocity) / Kt to the velocity PI
output, clamped to ID 73.

IDs 106/107 shape the outer references: speeding up is limited by the
acceleration, slowing down and reversing by the deceleration, and either nonzero
enables the planner (both 0, the boot value, leave references unshaped). In
POSITION mode the planner runs a trapezoid on its own planned position and
velocity, and the speed reference is the planned velocity plus ID 71 times the
planned position minus the measurement. A value between 0 and 0.1 is
INVALID_ARGUMENT. There is no position integrator: the velocity integral (ID 70)
already removes the steady position error under a constant load.

The profile travels with the tuning: a write publishes one complete candidate that
H0 applies like IDs 64–74 (ID 61 is the receipt). Writing a profile field runs the
unified refresh before publication:

- IDs 80–82 and 104: when Rs, Ld and Lq are all set, the current PI (IDs 64–67) is
  re-derived: Kp d/q = 2π·f·Ld/Lq, Ki = 2π·f·Rs. A model or bandwidth the design
  refuses (R·T/L, sampling or delay phase at the 200 µs H0 period) is
  INVALID_ARGUMENT and changes nothing. IDs 64–67 stay writable as a manual
  override until the next such write.
- ID 97: IDs 68 and 73 are tightened to the peak current; ID 98: ID 72 is
  tightened to the maximum speed. A write of ID 68 or 72 beyond them is refused,
  never clamped.
- IDs 96 and 98 set the protection bases: sustained current uses the rated
  current (0.95 × ID 68 when unset), stall uses 5% of the maximum speed (ID 72
  when unset).

Any applied tuning that lowers ID 73 below the remote current cap (ID 2) lowers
that cap to match. CAL-3/4/5 write the identified Rs, Ld, Lq and flux into IDs
80–83 and derive the PI at ID 104 in the same candidate.

### Controller tuning

| ID | Parameter | Type and unit | Range / Board1 initial |
| --- | --- | --- | --- |
| 60 / 61 / 62 | Tuning revision / receipt / request identity | u32, read only | 0 / 0 / 0 |
| 64 / 65 | Current Kp d / q | f32 V/A (unit 6) | 0–20 / 1.08, 1.35 |
| 66 / 67 | Current Ki d / q | f32 V/(A·s) (unit 7) | 0–20000 / 540 |
| 68 | Current-vector limit | f32 A | 0.05–boot value / 2.5 |
| 69 / 70 | Velocity Kp / Ki | f32 A/(rad/s) (unit 8) / A/rad (unit 9) | 0–1000 / 0.18; 0–100000 / 1.4 |
| 71 | Position Kp | f32 1/s (unit 10) | 0–1000 / 4 |
| 72 | Speed reference limit (also bounds VELOCITY setpoints) | f32 rad/s | 0.1–1000 / 12 |
| 73 | Outer-loop q-current limit | f32 A | 0.05–current limit / 2.5 |
| 74 | Command lease | u32 µs (unit 5) | 2500–500000 / 2500 |
| 112 | Over-current trip, above ID 68 | f32 A | ID 68–3.2 / 3.0 |
| 113 / 114 | Over / under-voltage trip, under below over | f32 V (unit 18) | 3–24 / 22, 5 |
| 115 | Over-speed trip, 0 automatic or at least ID 72 | f32 rad/s | 0–1000 / 0 |
| 116 | Sustained-current window, 0 off | u32 ms (unit 20) | 0–60000 / 1000 |
| 117 / 118 | Stall / tracking-divergence window, 0 off | u32 ms (unit 20) | 0–10000 / 200, 300 |

IDs 112–118 follow the tuning rules above (H0 applies them; ID 61 is the receipt)
and are stored by PARAM_SAVE. The initial values come from Board1 facts:
the 2.5 A control limit and the ±3.3 A current-sense span, the 5.25–20 V VM input
of the schematic and the 24 V DRV8311 absolute maximum. They await confirmation on
the target board.

The command lease is how long a setpoint, or a lifecycle request, stays valid.
Without a fresh setpoint within it, H0 latches COMMAND_TIMEOUT and inhibits.
At 115200 baud one setpoint frame takes about 2.5 ms on the wire, so a Host that
streams over a real UART needs a longer lease than the 2.5 ms default. A longer
lease also delays the stop after a Host disappears.

## UART service bounds

Board1 uses remapped USART1 PB6 TX/PB7 RX, 115200 8N1. The SDK confirms DMA1
CH1/CH2 as the Legacy RX/TX candidates. The V2 byte-IRQ implementation owns no DMA.
Its IRQ handles at most one RX byte/error and one TX byte, at priority 2 below
H0 priority 0 and H1 priority 1. Nominal full-duplex traffic can produce up to
23,040 byte events per second. This is an event-rate bound, with MCU time cost
still to be measured.

The RX ring has 256 16-bit entries, including a gap sentinel. It drops corrupt
or overflowing data through a delimiter, then queues the sentinel before any
new bytes. Foreground resets framing at that sentinel. The TX buffer holds one
complete 116-byte frame and stays immutable until the IRQ consumes it. A further
STOP acknowledgement has a reserved metadata slot.

Foreground runs Runtime supervision first, then consumes 8–32 bytes and at most
one complete frame per poll. A busy TX slot rejects ordinary requests before
admission and increments the drop counter. A valid STOP can use the reserve
without blocking on the ordinary response. When both slots are occupied,
further requests are unadmitted and counted. Hosts use stop-and-wait and can
retry an unadmitted request. An admitted duplicate receives REPLAY, so it cannot
repeat an action or renew a lease.

## CAN (CANSimple)

Board1 also speaks the CANSimple frame layout on CAN1 (PA11 RX, PA12 TX, SIT3051),
classic CAN with 11-bit IDs: `node << 5 | command`. Each command becomes the same
Foreground request a UART Host sends, so admission, limits, the output lock and the
command lease (ID 74) are shared. UART and CAN share one control state; the last
command wins. There are no command responses: refusals are counted (ID 126).

Each Board1 carries a 120 ohm termination. A bus needs exactly two, one at each end:
two boards connect directly; with three or more, remove it from the boards in between.

Units are output-side: position in turns, velocity in turns/s, torque in N·m (through
Kt = 1.5 × pole pairs × flux and the gear ratio, ID 99), relative to the output zero
(ID 124). Floats are IEEE-754 little-endian.

| Cmd | Message | FalconFOC behaviour |
| --- | --- | --- |
| 0x00 | Get_Version | protocol 2, hardware 1.0, firmware 2.0.0 (unreleased flag 1) |
| 0x01 | Heartbeat | every ID 123 ms (100): errors u32, axis state, procedure result, trajectory done |
| 0x02 | Estop | STOP; ESTOP_REQUESTED until Clear_Errors |
| 0x03 | Get_Error | active errors, disarm reason (CANSimple bits) |
| 0x04/0x05 | RxSdo/TxSdo | read (opcode 0) or write (1) a FalconFOC parameter by its ID; every RxSdo gets a TxSdo whose byte 0 is the V1 result code and bytes 4–7 the value |
| 0x06 | Address | RTR or empty: reply node + 48-bit serial (MCU UID folded); 8 bytes with a matching serial (0 = any): set the node ID (ID 120) |
| 0x07 | Set_Axis_State | 1 STOP; 8 ARM then START; 3/4/7 CALIBRATE_START all/electrical/rotor; others refused |
| 0x09 | Get_Encoder_Estimates | every ID 125 ms (10): position, velocity |
| 0x0B | Set_Controller_Mode | control 1 torque (CURRENT), 2 velocity, 3 position; input 1 passthrough, 2 velocity ramp, 5 trapezoidal (the ramp follows IDs 106/107); DISABLED only |
| 0x0C/0x0D/0x0E | Set_Input_Pos/Vel/Torque | SETPOINT; nonzero feedforward is refused; torque needs Kt |
| 0x0F | Set_Limits | IDs 72 (× 2π × gear) and 2 |
| 0x11/0x12 | Set_Traj_Vel_Limit/Accel_Limits | ID 72; IDs 106/107 (× 2π × gear) |
| 0x14 | Get_Iq | q target, q measured (A) |
| 0x15 | Get_Temperature | NaN, NaN (no NTC) |
| 0x16 | Reboot | 0 reset (DISABLED), 1 PARAM_SAVE, 2 CONFIG_ERASE, 3 refused |
| 0x17 | Get_Bus_Voltage_Current | NaN, NaN (no live readback yet) |
| 0x18 | Clear_Errors | FAULT_CLEAR; clears ESTOP and calibration flags |
| 0x19 | Set_Absolute_Position | output zero (ID 124) so the present position reads the value |
| 0x1A/0x1B | Set_Pos_Gain/Set_Vel_Gains | ID 71 as is; IDs 69/70 ÷ (2π × Kt × gear²) |
| 0x1C | Get_Torques | target, Kt × gear × iq estimate |

Get_ requests are an RTR or an empty data frame to the node; broadcast (0x3F) Get_
requests are answered only by an unaddressed device. Commands to 0x3F reach every
device. An unaddressed device (node 0x3F, the default) sends no cyclic frames.

Heartbeat states: 0 BOOT, 1 IDLE (DISABLED, READY, FAULT_LATCHED), 8 RUNNING, 3/4/7
while a calibration requested over CAN runs. Procedure results: 0 SUCCESS, 1 BUSY,
2 CANCELLED, 3 DISARMED, 4 NO_RESPONSE, 5 POLE_PAIR_CPR_MISMATCH,
6 PHASE_RESISTANCE_OUT_OF_RANGE, 11 TIMEOUT, 13 INVALID_STATE, 14 NOT_CALIBRATED,
15 NOT_CONVERGING. A closed-loop request retries ARM/START for 250 ms (the Host keeps
streaming setpoints) and ends in SUCCESS, NOT_CALIBRATED or INVALID_STATE; the locked
image ends in INVALID_STATE.

Error bits: COMMAND_TIMEOUT → WATCHDOG_TIMER_EXPIRED 0x1000000; OVER/UNDER_VOLTAGE →
DC_BUS_OVER/UNDER_VOLTAGE 0x100/0x200; OVER_CURRENT, SUSTAINED_CURRENT →
CURRENT_LIMIT_VIOLATION 0x1000; OVER_SPEED → VELOCITY_LIMIT_VIOLATION 0x8000; DRIVER →
DRV_FAULT 0x20; SAMPLE_TIMEOUT/INVALID → MISSING_ESTIMATE 0x8; heartbeats and WDT →
TIMING_ERROR 0x4; everything else → SYSTEM_LEVEL 0x2; Estop → ESTOP_REQUESTED
0x2000000; a failed CAN calibration → CALIBRATION_ERROR 0x40000000. ID 127 reads the
raw fault word.

CAN parameters (contract 25): ID 120 node 0–63 (63), ID 122 bit rate 125000, 250000,
500000 or 1000000 (1000000, applied at boot), ID 123 heartbeat and ID 125 estimate
periods 0–10000 ms (0 off), ID 126 read-only link status (bits 0–7 state: 0 off,
1 active, 2 error passive, 3 bus-off, 4 no crystal; 8–15 refused commands; 16–31
dropped frames), ID 127 read-only raw faults. IDs 120–125 are saved by PARAM_SAVE.
Bit timing: APB1 75 MHz, 15 quanta (1 + 12 + 2), sample point 86.7 %, SJW 1; automatic
bus-off recovery. The HICK fallback clock is too inaccurate for CAN: CAN stays off.
`protocol/tests/cansimple_vectors.tsv` holds example frames.

## CAN (MIT)

With ID 128 = 1 the CAN port speaks the MIT (mini-cheetah) frames instead of CANSimple;
CANSimple frames, cyclic heartbeats and estimates stop. Switch while DISABLED (UART or a
CANSimple SDO to ID 128; back with UART or the CANSimple SDO before switching).

- Command to the node ID (ID 120), standard 11-bit, 8 bytes, big-endian bit packing:
  position 16 bits in ±P_MAX (ID 129, output rad relative to the output zero), velocity
  12 bits in ±V_MAX (ID 130), Kp 12 bits in 0..KP_MAX (ID 132), Kd 12 bits in 0..KD_MAX
  (ID 133), torque 12 bits in ±T_MAX (ID 131). It goes through UART SETPOINT mode 3, so
  the command lease (ID 74, default 2.5 ms) is the watchdog: stream at 400 Hz or more, or
  raise it. Outside motor mode a command is refused (counted in ID 126) but answered.
- Special frames (bytes 0–6 0xFF): 0xFC enter motor mode (SET_MODE IMPEDANCE, a zero-torque
  hold at the present position, ARM, START; refused without an identified flux), 0xFD
  exit (STOP), 0xFE zero (DISABLED only: the present output position becomes 0, ID 124 in
  RAM; PARAM_SAVE stores it), 0xFB clear faults (DaMiao).
- Every frame to the node is answered on the master ID (ID 121). ID 134 = 0, original,
  6 bytes: node, position 16, velocity 12, torque 12 (measured q current × Kt × gear).
  ID 134 = 1, DaMiao, 8 bytes: error << 4 | node (low 4 bits), position, velocity, torque,
  two temperature bytes (0: no sensor). Errors: 0 disabled, 1 enabled, 8 over-voltage,
  9 under-voltage, A over-current, D command lease expired, E any other latched fault.

Ranges default to P_MAX 12.5 rad, V_MAX 65 rad/s, T_MAX 1 N·m, KP_MAX 500, KD_MAX 5; the
host must use the device's values. P_MAX × gear must stay within the Runtime's 1024 rad
rotor range, and V_MAX × gear within the velocity limit, or the commands are refused.
