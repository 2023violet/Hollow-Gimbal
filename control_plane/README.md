# FalconFOC control plane

The optional `falconfoc-control-plane` C11 target provides parameter metadata,
typed Runtime observations, detached initial configuration, complete
IMPEDANCE command publication and existing Runtime actions.

```powershell
cmake -S control_plane -B build/control-plane -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/control-plane --target test-foc-parameter
ctest --test-dir build/control-plane -R '^test-foc-parameter$' --output-on-failure
```

See the [parameter quickstart](../docs/control-plane/quickstart.md) for write
admission, input bounds and receipt semantics. The [standalone example](../examples/parameters/README.md)
uses the same targets from a separate application directory.

## Motion protection

See the [motion protection guide](../docs/control-plane/motion-protection.md) for
mode admission, timing, and the output confirmation contract.

The parameter catalog exposes these static configuration fields. Position and
error values use continuous mechanical coordinates in the configured frame.
The motion limits and following error monitor default to disabled.

| ID | Field | Type and unit |
| --- | --- | --- |
| `0x100B` | `motion.limits_enabled` | u32 flag |
| `0x100C` | `motion.position_min` | f32 rad |
| `0x100D` | `motion.position_max` | f32 rad |
| `0x100E` | `motion.limit_tolerance` | f32 rad |
| `0x100F` | `motion.protection_frame` | u32 frame identity |
| `0x1010` | `motion.following_enabled` | u32 flag |
| `0x1011` | `motion.following_threshold` | f32 rad |
| `0x1012` | `motion.following_duration` | u32 µs |
| `0x1013` | `motion.following_max_gap` | u32 µs |
| `0x3010` | `motion.following_state` | read-only Runtime enum |
| `0x3011` | `motion.rejection` | read-only Runtime enum |

Submit interdependent settings together. The whole candidate must have a
valid position interval and timing once the corresponding feature is
enabled. Active protection requires motion feedback in the selected frame.
When IMPEDANCE is configured, its frame must match. A zero readback while
protection is disabled can denote an unconfigured field.

Use `foc_parameter_prepare_initial()` before initialization. For an existing
instance, `foc_parameter_stage()` still requires the current Session,
DISABLED, no fault, and confirmed INHIBIT. Both functions write an independent
candidate. The application must initialize a new Runtime instance with that
candidate to apply it.

The two status values come from the coherent Runtime snapshot. They describe
software protection decisions. To confirm stopping, read the output action
and adapter confirmation from the same snapshot. Physical power shutdown is
a separate hardware property.

## Versioned configuration storage

The optional `falconfoc-config-store` target provides configuration validation,
encoding and two-slot transactions. The `falconfoc-config-store-host` target
supplies the Host device model. See the [wire format and eligibility
matrix](../docs/control-plane/config-store.md) for setup and recovery rules.

## Read-only configuration diagnostics

The optional `falconfoc-config-diagnostics` target exposes dual-slot
inspection, parameter comparison and coherent Host Runtime observation. See
the [diagnostic contract](../docs/control-plane/config-diagnostics.md), the
[standalone example](../examples/config-diagnostics/README.md) and the
[offline inspector](../tools/falconfoc-config-inspector/README.md).

## Reviewed calibration application

The optional `falconfoc-calibration-application` target binds reviewed
calibration to a captured Host Runtime. It prepares detached configuration
and explicitly initializes separate Runtime and encoder instances. See the
[workflow and application contract](../calibration/WORKFLOW.md).
