# FalconFOC Servo SDK 0.1.0-rc.1

**INTERNAL EVALUATION CANDIDATE**

**REDISTRIBUTION BLOCKED — OWNER APPROVAL REQUIRED**

The package provides the portable C11 control Core and a Servo Host SDK. Its
exact source snapshot, Git identity, and API header list are in
[SDK-METADATA.json](SDK-METADATA.json) and
[SDK-SOURCE-MANIFEST.json](SDK-SOURCE-MANIFEST.json). Verify the archive using
its accompanying SHA-256 file. This candidate uses the current pre-release API
and CMake targets. Source and binary compatibility are not frozen.

## Build from the extracted directory

Use CMake 3.25+, Ninja, and a C11 compiler. Windows Clang and MinGW GCC are
covered by the SDK gate. Use separate output directories for each compiler.
The source targets use the C standard library and link the system math
library where needed.

```powershell
cmake -S . -B build-sdk -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
cmake --build build-sdk
ctest --test-dir build-sdk --output-on-failure
& ./build-sdk/examples/servo/falconfoc-servo-demo.exe
& ./build-sdk/examples/servo/falconfoc-servo-demo.exe --faults
```

For MinGW GCC use `-DCMAKE_C_COMPILER=gcc` with a fresh build directory.
Non-Windows hosts omit the `.exe` suffix. The documented regression evidence
records the environments actually run.

## Use as a CMake dependency

Core only. The target exposes headers such as `foc_impedance.h` and the
entry point `foc_impedance_step()`.

```cmake
set(FALCONFOC_SDK_CORE_ONLY ON)
add_subdirectory("${FALCONFOC_SOURCE_DIR}" falconfoc-sdk EXCLUDE_FROM_ALL)
target_link_libraries(my_app PRIVATE falconfoc-core)
```

For Servo Host, omit `FALCONFOC_SDK_CORE_ONLY` and link
`falconfoc-virtual-motor`. This includes `falconfoc-pipeline` (Runtime,
Device, and Safety), `falconfoc-core`, and `falconfoc-encoder`.
Link `falconfoc-calibration` when using candidate estimators for
current/angle calibration, Motor-ID, Mechanical-ID or Encoder LUT.
The package also includes IMPEDANCE output and Mechanical-ID regression tests.
The [Servo Quickstart](docs/servo/quickstart.md) covers the existing example,
explicit parameters and candidate sources, scheduling and mode admission,
telemetry, and failure handling.

Host output confirmation has `physical_applied=false`. The example uses
an ideal average-voltage plant and explicit model feedforward. Tracking and
load results apply to the documented Host scenarios. The 48 Hz research
filter is outside production IMPEDANCE. G3-SAFE hardware and physical motor
operation remain unverified, G3-PWR is locked, and AT32F415 WCET is unverified.

See the [notice review draft](NOTICE-REVIEW.md) for source origins and outstanding
permissions.
