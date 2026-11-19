# FalconFOC Configuration Store

## Format 1 wire contract

A store slot is 256 bytes. All multibyte values use little-endian order. The bytes after the record are ignored. A completely erased slot is all `FF`.

| Offset | Width | Meaning |
| ---: | ---: | --- |
| 0 | 4 | Commit word `0xC35A4C7E`, erased value `0xFFFFFFFF` |
| 4 | 4 | Magic bytes `46 43 46 47` (FCFG) |
| 8 | 2 | Format version 1 |
| 10 | 2 | Schema version 1 or 2 |
| 12 | 4 | Sequence modulo 2³² |
| 16 | 2 | Payload byte length |
| 18 | 2 | TLV count |
| 20 | 2 | Confirmed motor pole pairs |
| 22 | 2 | Bit 0 = IMPEDANCE profile enabled. Other bits reserved. |
| 24 | 4 | Encoder CPR |
| 28 | 4 | Runtime encoder direction, signed i32 (`+1` or `−1`) |
| 32 | 4 | Mechanical frame identity |
| 36 | 4 | Encoder and calibration setup identity |
| 40 | 4 | Qualified electrical offset, IEEE binary32 |
| 44 | 4 | Motor setup identity |
| 48 | 4 | Qualified IMPEDANCE Kt as IEEE binary32, zero when unavailable |
| 52 | 4 | IEEE CRC-32 of bytes 4–51 followed by the payload |
| 56 | variable | TLV stream |

Each TLV is 8 bytes. Its ID is u16, its type is u8 and its byte length is u8 (4), followed by a 4-byte value. Type tags are 1 (u32), 2 (two's-complement i32), 3 (finite IEEE binary32), and 4 (boolean, zero or one). Existing Control Plane boolean edits retain their u32 type and 0–1 range. Type 4 is available to optional extensions. The canonical encoder emits existing Control Plane IDs in increasing order. Readers accept arbitrary order and reject duplicate IDs, including optional fields. IDs `0x8000–0xFFFE` are reserved for noncritical extensions of these scalar types. Unknown IDs outside that range and known but ineligible IDs fail. Maximum count is 25. An explicit re-save emits the canonical known fields.

CRC uses the existing reflected IEEE polynomial `0xEDB88320`, initial and final XOR `0xFFFFFFFF`. The vector `123456789` yields `0xCBF43926`. The CRC excludes its own field and the separately programmed commit word. The commit word is verified independently. Recognizable future formats must retain the outer commit, magic, format and length fields, CRC offset and coverage. The scanner checks outer integrity before interpreting the format. A corrupt format byte can be recovered using the other valid copy. A CRC-valid foreign format prevents selection and saving. Firmware version is independent of the record format and schema.

Schema 1 contains the original five motion/impedance static limits (or the one motion limit when IMPEDANCE is disabled). Schema 2 adds nine explicit motion protection parameters. Schema 1 is an independent compatibility fixture for this store. Migration needs the full, separately confirmed protection group for the same identity and revalidates the combined candidate. Older records containing motion-protection fields are incompatible with Schema 1. A successful migration returns a detached candidate. Saving it requires a separate request.

## Persistence Eligibility Matrix

See the [Control Plane catalog](../../control_plane/include/foc_parameter.h) for names and calculation units. All saved settings affect output admission or its bounds and require candidate validation followed by explicit Runtime initialization.

| ID | Field | Persistence role and recovery check |
| --- | --- | --- |
| `0x1005` | Motion q current limit | Saved. Positive, finite and at most the trusted Pipeline cap. Active staging retains the rule that it may only lower the live cap. |
| `0x1006` | IMPEDANCE torque limit | Saved for IMPEDANCE. Positive and finite. |
| `0x1007` | Stiffness ceiling | Saved for IMPEDANCE. Finite and nonnegative. |
| `0x1008` | Damping ceiling | Saved for IMPEDANCE. Finite and nonnegative. |
| `0x1009` | Feedforward torque ceiling | Saved for IMPEDANCE. Finite and nonnegative. |
| `0x100B` | Position limits enabled | Saved in schema 2. Explicit 0 or 1. |
| `0x100C` / `0x100D` | Position lower and upper bounds | Saved in schema 2. Finite and strictly ordered in continuous mechanical radians. |
| `0x100E` | Inward boundary tolerance | Saved in schema 2. Finite, nonnegative, leaves a gap between the two boundary bands. |
| `0x100F` | Motion protection frame | Saved in schema 2. Must match the qualified frame and the IMPEDANCE frame when applicable. |
| `0x1010` | Following monitor enabled | Saved in schema 2. Explicit 0 or 1. |
| `0x1011` | Following threshold | Saved in schema 2. Positive and finite. |
| `0x1012` / `0x1013` | Following duration and maximum capture gap | Saved in schema 2. Positive and below the UInt32 half-range. Maximum gap covers both nominal H0 and H1 periods. |
| `0x1001–0x1004` | Pole pairs, electrical offset, Kt, IMPEDANCE frame | Identity checks against the trusted base. The frame uses the shared identity and the other three use explicit header fields. Restore preserves the base values. |
| `0x100A` | Position speed ceiling | Remains a read-only setting supplied by the trusted base. |
| `0x2001–0x2005` | Position, velocity, command K/D and feedforward | Transient commands, excluded. |
| `0x3001–0x3011` | Mode, state, faults, Session, command sequence and time, sensor time, output action and confirmation, updates, inhibit pending, sensor validity, measured position and speed, Pipeline status, following state and motion rejection | Runtime observations, excluded. The new Runtime produces its own observations. |
| `0x4001–0x4005` | STOP, CLEAR_TIMEOUT, SET_MODE, ARM and START | Actions, excluded. Restore creates no lifecycle request. |

The store supports motion profiles with and without IMPEDANCE. A saved protection group always includes both enable flags and valid dormant values. For a fresh default configuration with unset protection values, prepare the full group explicitly before saving. The original Runtime disabled defaults remain available to callers outside persistence.

Encoder CPR, direction and qualified encoder identity come from the accepted sensor/calibration setup. Pole pairs and electrical offset are checked against the trusted Runtime rotor mapping. Supply qualified coordinate and motor setup IDs from the application, such as the Motor-ID capture setup where applicable. These IDs express software configuration provenance. Reused IDs or incorrectly trusted inputs cannot identify a replaced device. An unqualified identity returns `ERR_IDENTITY_UNKNOWN`. A known mismatch returns `ERR_IDENTITY`. When the application confirms a replacement, it owns the decision to provision fresh storage.

## Save, recover and apply

1. Use `foc_parameter_prepare_initial()` with an application-owned base, or use `foc_config_store_save_active()` for a live Runtime. The active API uses the initialized Runtime as its baseline. Re-send changes that must persist across repeated saves before reinitializing. It requires the current Session, DISABLED, no faults, no pending lifecycle request and confirmed INHIBIT. Serialize with other lifecycle producers.
2. Saving erases the inactive slot and programs the complete record with its commit word left erased. It verifies those bytes, programs the commit word in a separate aligned operation, and verifies the complete committed bytes. The active API rechecks admission before erasing and before committing. Retrying a save that matches the canonical payload leaves the sequence and slots unchanged.
3. At reboot use `foc_config_store_load()`. A nonzero sequence difference smaller than `0x80000000` identifies the newer record modulo 2³². Identical same-sequence records select A. Conflicting ties and the exact half-range return `ERR_SEQUENCE`. A damaged or uncommitted copy can be ignored when the other committed envelope is valid. Read failure, a CRC-valid foreign format, or a selected envelope with incompatible schema, identity or values stops the operation.
4. `EMPTY` means unconfigured. `ERR_NO_VALID` means recovery failed. `ERR_RECONFIRM` requests explicit migration input. After `ERR_UNCERTAIN`, reload before deciding whether to retry. Every rejection preserves the caller's candidate. The report can identify the selected envelope and show both slot inspection outcomes even if candidate validation fails.
5. On `OK` or `MIGRATED`, the application may initialize a new Runtime using the detached candidate and its trusted Pipeline policy. INHIBIT and normal lifecycle requirements continue to apply.

The caller-owned Host model uses independent slots, erased `0xFF` bytes, 4-byte programming units and 1→0 writes. It can inject torn steps and lost acknowledgements. The [standalone demo](../../examples/parameters/README.md) and [SOFT-03 evidence](../evidence/2026-11-18-next-soft-03.md) describe the software acceptance scope.
