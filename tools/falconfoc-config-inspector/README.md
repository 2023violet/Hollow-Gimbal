# FalconFOC Configuration Inspector

`falconfoc-config-inspector` is the offline C11 command line for SOFT-03 Host
storage images. Each input must be a regular binary file of exactly 512 bytes,
with Slot A at bytes 0–255 and Slot B at bytes 256–511. The inspector opens
files for reading and passes a read-only driver to the official Config Store
diagnostics. It produces reports on standard output.

## Build and run

Run from the repository root. The executable needs the FalconFOC libraries
built from this checkout. Python is used by the test suite. Pass
`-DBUILD_TESTING=OFF` to configure a CLI-only build without Python.

```powershell
cmake -S tools/falconfoc-config-inspector -B build/config-inspector -G Ninja
cmake --build build/config-inspector
ctest --test-dir build/config-inspector -R '^test-falconfoc-config-inspector$' --output-on-failure
```

With testing enabled, the separate fixture executable emits a real dual-slot
snapshot through standard output. The following creates a fresh sample file
from the production SOFT-03 save path with the simulated backend.

```powershell
python -c "from pathlib import Path; from subprocess import run; image = Path('build/config-inspector/normal.bin'); run(['build/config-inspector/falconfoc-config-fixtures.exe', 'normal'], stdout=image.open('xb'), check=True)"
$inspector = 'build/config-inspector/falconfoc-config-inspector.exe'
$image = 'build/config-inspector/normal.bin'
& $inspector inspect $image
& $inspector inspect --json --profile host-fixture-v1 $image
& $inspector diff --profile host-fixture-v1 $image
& $inspector diff --json --profile host-fixture-v1 --against-default $image
& $inspector explain IDENTITY_MISMATCH
```

On Linux or macOS, omit `.exe`.

- `inspect IMAGE` checks the image structure and records the formal envelope
  selection. `--profile host-fixture-v1` enables qualification against the
  independent SOFT-03 virtual demonstration setup and reports whether the
  selected record can be restored.
- `diff --profile host-fixture-v1 IMAGE` compares Slot A with Slot B.
  `--against-default IMAGE` compares the selected candidate with the profile
  defaults. `IMAGE IMAGE` compares the selected candidates of two images.
  The tool gets stable Parameter IDs, names, types, units, and reinitialization
  requirements from the Control Plane descriptors. Terminal output groups
  motion protection fields. Unavailable sources remain explicit in both formats.
- `explain REASON` describes codes reported by `inspect` or `diff`. It also
  accepts Runtime diagnostic terms such as `REINITIALIZATION_REQUIRED`,
  `NOT_APPLIED`, `SNAPSHOT_UNAVAILABLE`, and `OUTPUT_LOCKED`.
- `--json` selects the machine-readable result for any command.
  `--help` displays the commands and arguments.

`host-fixture-v1` is qualified only for the existing Host demonstration in
[examples/parameters](../../examples/parameters/README.md). It uses the virtual
setup with 4 pole pairs, 65,536 encoder counts per revolution, direction +1,
mechanical frame 71, encoder identity 101, and motor setup 201. The checked
impedance profile, electrical offset, and torque constant also come from that
trusted setup. For another setup, its Host owner can provide the trusted
Pipeline and Runtime defaults and qualified identity directly to the public
[C11 diagnostics API](../../control_plane/include/foc_config_diagnostics.h).

The recorded identity is image data. Interpret it alongside the CRC state
and the result of the independent identity check. CRC-32 serves corruption
detection. An applicable record is a detached candidate. Runtime state requires
a coherent Host Runtime capture through the public C11 diagnostics API. The
CLI source is always `HOST_STORAGE_IMAGE`. Both live Runtime application and
physical AT32F415 Flash contents are outside this offline observation.

Exit code 0 means a report was produced, including reports with rejected
records or unavailable comparison values. Exit code 2 denotes an invalid
command, unsupported profile, or unreadable input file. Exit code 1 denotes an
internal comparison error. Errors are written to stderr.

## JSON schema version 1

Each successful command writes one JSON object. Every object contains
`schema_version` (integer `1`) and `command` (`inspect`, `diff`, or `explain`).
The top-level schema version describes CLI JSON. Each slot's `schema_version`
is the persisted record's schema.

| Object | Required members and meaning |
| --- | --- |
| `inspect`, `diff` | `source` is `HOST_STORAGE_IMAGE`. `trust_boundary` is a human-readable scope. `profile` is the profile name or `null` for integrity-only inspection. `runtime_applied` is `null` because the Runtime was not observed. |
| `inspect.recovery` | `selected_slot` is `A`, `B`, or `null`. `available` is boolean or `null`. `status` is the Config Store status. `reason` is a stable code, and `explanation` is readable text. The Store chooses the envelope before Schema and identity admission, so selection can coexist with `available: false` or `null`. |
| `inspect.slots[]` | Contains two objects in A/B order. `slot` identifies the slot and `record_present` indicates recognized magic. `format_version`, `payload_length`, `schema_version`, `sequence` and `record_count` are integers or `null`. Format-specific fields are available for the supported format. `magic`, `crc`, `schema`, `tlv`, `semantics`, and `identity` are `PASS`, `FAIL`, or `null`. `tlv_reason` is an independent structural rejection code or `null`. `integrity_valid`, `migration_needed`, and `applicable` are booleans or `null`. |
| `inspect.slots[]` continued | `commit` is `ERASED`, `TORN`, `COMPLETE`, or `null`. `recorded_identity` is the stored identity with raw Float32 bit strings for `electrical_offset_bits` and `kt_bits`, or `null` when unavailable. `identity_mismatches` lists differing field names or is `null` when the check was unavailable. `selection` is `SELECTED`, `OLDER`, `EQUIVALENT`, `INELIGIBLE`, or `UNDECIDED`. `envelope_status` and `status` are numeric Config Store results. `reason` is the main rejection code (or `NONE`), and `explanation` is readable text. |
| `diff.left`, `diff.right` | `source` is `HOST_STORAGE_IMAGE` or `DEFAULT_CONFIG`. `slot` is `A`, `B`, or `null`. `available` indicates a qualified configuration. `status` and `reason` explain why the source is unavailable. |
| `diff.fields[]` | Each entry has an integer `parameter_id`, strings `name`, `type`, `unit`, and `classification`, values `left` and `right` (numeric or `null`), bit strings `left_f32_bits` and `right_f32_bits` (e.g. `0x3e4ccccd`, or `null`), and booleans `motion_protection` and `requires_reinitialization`. |
| `diff.float32_comparison` | `EXACT_IEEE_754_BITS` identifies comparison of the decoded Float32 bits. Distinct zero signs compare different. The decimal fields show up to nine significant digits. |
| `explain` | `reason` carries the requested code and `explanation` carries its description. |

The field classifications are `UNCHANGED`, `VALUE_CHANGED`, `MISSING_LEFT`,
`MISSING_RIGHT`, `TYPE_MISMATCH`, `SCHEMA_INCOMPATIBLE`,
`IDENTITY_MISMATCH`, and `VALUE_UNAVAILABLE`. Consumers can branch on codes
or numeric statuses and present the explanations as text.
