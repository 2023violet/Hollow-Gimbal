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

Malformed frames are counted and discarded. Business errors use a one-byte
response payload. Damaged bytes never extend a lease.

## Message payloads

All empty requests reject extra bytes. Lengths below include the response result.

| ID | Message | Request payload | Response |
| --- | --- | --- | --- |
| 01 | HELLO | Empty, session 0 | 9-byte capability, new session in header |
| 02 | CAPABILITY_READ | Empty | 9-byte capability |
| 03 | STATUS_READ | Empty | 30-byte status |
| 10 | COMMAND_SETPOINT | mode u8 + d/q f32 for current (9 bytes), or mode u8 + reference f32 for velocity/position (5 bytes) | ACCEPTED + Runtime publication u32 (5 bytes) |
| 11 | COMMAND_STOP | Empty | ACCEPTED, query status for H0 acknowledgement |
| 12 / 13 | COMMAND_ARM / COMMAND_START | Empty | OUTPUT_LOCKED on Board1; on Host virtual targets (virtual device, Board1 SIL) ACCEPTED + lifecycle request u32 (5 bytes) |
| 14 | COMMAND_SET_MODE | mode u8 | ACCEPTED + lifecycle request u32 (5 bytes); BAD_STATE unless DISABLED with INHIBIT confirmed |
| 15 | LIFECYCLE_READ | Empty | 25-byte receipt of the latest ARM/START/SET_MODE (below) |
| 20 | FAULT_READ | Empty | 30-byte status including faults and recovery result |
| 21 | FAULT_CLEAR | Empty | ACCEPTED only for isolated command-timeout recovery |
| 30 | PARAM_READ | ID u16 | 18-byte typed parameter |
| 31 | PARAM_WRITE | ID u16 + type u8 + value u32/f32 | APPLIED + the updated typed parameter |
| 32 | PARAM_SAVE | Empty | APPLIED + stored record sequence u32 (5 bytes); NOT_SUPPORTED on images without storage |
| 33 | CONFIG_ERASE | Empty | APPLIED; the next boot uses the boot configuration. NOT_SUPPORTED without storage |
| 40 | TELEMETRY_READ | Empty | 75-byte status and telemetry |
| 41 | TELEMETRY_STREAM | Empty | NOT_SUPPORTED |

Modes are CURRENT 0, VELOCITY 1 and POSITION 2. Units are A, mechanical rad/s
and continuous mechanical rad. Setpoints must match the current Runtime mode.
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
requests only isolated COMMAND_TIMEOUT recovery. H0 rechecks the fault mask,
fresh ADC/angle data, Foreground progress, request age and software inhibition
before invoking its existing clear operation. The clear result is queryable.
Other fault masks return BAD_STATE. Recovery ends in DISABLED with old command
and sample validity cleared.

## Capability and status layout

Capability response bytes are result, protocol version, three firmware version
bytes, algorithm mode bitmask, physical mode bitmask, and capability u16.
Current values are protocol 1, firmware 0.1.0, algorithms 7, physical modes 0,
and capabilities 63, or 127 when the image has persistent storage. Capability
bits are output lock 0, RAM policy parameters 1, polling telemetry 2, UART
transport 3, remote mode switch 4, controller tuning 5 and persistent settings
6. No calibration claim is set.

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

TELEMETRY appends f32 d current at 30, f32 q current at 34, f32 mechanical
position at 38, f32 velocity at 42, u32 sensor source time at 46, u32 CRC errors
at 50, u32 dropped frames at 54, u32 RX overflow/error events at 58, and a validity byte
at 62. Its bits are physical applied 0, calibrated current valid 1, sensor
calibrated 2. In the locked Board1 image all three bits and the current fields
stay zero; see the current-field note under Parameters. Position/velocity preserve their H0-consumed source time
and are valid only when the fresh-sensor flag is set. Their calibration remains
unverified.

## Parameters

A parameter response is result, ID u16, type u8, unit u8, attributes u8, minimum,
maximum and value. The final three values occupy four bytes each and use the
declared type. Types are u32 1 and f32 2. Units are dimensionless 0, A 1, rad 2,
rad/s 3, raw ADC counts 4. Attribute bits are writable 0, applied to RAM 1 and
calibration required 2.

| ID | Parameter | Type | Range / initial value |
| --- | --- | --- | --- |
| 1 | Bytes examined per Foreground protocol poll | u32 | 8–32 / 24 |
| 2 | Remote current-vector magnitude cap | f32 A | 0.05–2.5 / 2.5 |
| 3 | Absolute remote position-reference cap | f32 rad | 1–1000 / 100 |
| 16 | Pole pairs | u32, read only | Board1 nominal 7 |
| 17 | Electrical offset | f32 rad, read only | Uncalibrated nominal 0 |
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

### Persistent settings

PARAM_SAVE and CONFIG_ERASE require DISABLED with INHIBIT confirmed and no
pending tuning. PARAM_SAVE stores the live controller tuning (IDs 64–74) and the
RAM policies (IDs 1–3) in one record bound to the effective motor configuration
(IDs 32, 16, 17, 38–42). At boot, before any interrupt, a stored record is
applied only when that configuration is identical and every value passes the
same checks as a UART write; otherwise the boot configuration stays in use.
Two slots and a separately programmed commit word keep the previous record
when a save is interrupted. A successful save clears the dirty flag.

| ID | Readback | Values |
| --- | --- | --- |
| 48 | Boot outcome | 0 unavailable (no storage), 1 empty, 2 loaded, 3 invalid record, 4 identity mismatch, 5 value rejected, 6 I/O error |
| 49 | Applied record sequence | 0 when none was applied |
| 50 | Newest stored record sequence | 0 when storage is erased |

### Controller tuning

| ID | Parameter | Type and unit | Range / Board1 initial |
| --- | --- | --- | --- |
| 60 / 61 / 62 | Tuning revision / receipt / request identity | u32, read only | 0 / 0 / 0 |
| 64 / 65 | Current Kp d / q | f32 V/A (unit 6) | 0–20 / 1.08, 1.35 |
| 66 / 67 | Current Ki d / q | f32 V/(A·s) (unit 7) | 0–20000 / 540 |
| 68 | Current-vector limit | f32 A | 0.05–boot value / 2.5 |
| 69 / 70 | Velocity Kp / Ki | f32 A/(rad/s) (unit 8) / A/rad (unit 9) | 0–10 / 0.18; 0–1000 / 1.4 |
| 71 | Position Kp | f32 1/s (unit 10) | 0–1000 / 4 |
| 72 | Speed reference limit (also bounds VELOCITY setpoints) | f32 rad/s | 0.1–1000 / 12 |
| 73 | Outer-loop q-current limit | f32 A | 0.05–current limit / 2.5 |
| 74 | Command lease | u32 µs (unit 5) | 2500–500000 / 2500 |

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
