# FalconFOC parameter API example

The example runs a virtual Servo through the public C11 control-plane API.
It enumerates the catalog, prepares an initial limit, and reads the effective
value after the Host owner initializes the Runtime. It demonstrates the
difference between an accepted IMPEDANCE publication and an observed H0
application, rejects a change while running, and observes a confirmed
INHIBIT after STOP. It also exercises adapter rejection and invalid feedback.

Run from the checkout root with CMake 3.25+, Ninja, and a C11 compiler.

```powershell
cmake -S examples/parameters -B build/parameter-example -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/parameter-example --target falconfoc-parameter-demo
ctest --test-dir build/parameter-example -R '^test-falconfoc-parameters-example$' --output-on-failure
& ./build/parameter-example/falconfoc-parameter-demo.exe
```

For an independent application, copy this directory outside the checkout and
configure it with `-DFALCONFOC_SOURCE_DIR=<FalconFOC source checkout>`. The
project links the public `falconfoc-control-plane` and `falconfoc-virtual-motor`
targets. Use a new build directory when changing compiler. On platforms that
omit the `.exe` suffix, run the binary as `./build/parameter-example/falconfoc-parameter-demo`.

The catalog describes the supported typed values and action paths. Individual
IMPEDANCE fields are validated as part of a complete command with an explicit
source identity. A live static edit produces a detached candidate and leaves
the current instance unchanged. The example uses Host output and reports
`physical_applied=false`.

## Versioned configuration store

Build the Host persistence example with the same CMake configure command.

```powershell
cmake --build build/parameter-example --target falconfoc-config-store-demo
ctest --test-dir build/parameter-example -R '^test-falconfoc-config-store-demo$' --output-on-failure
& ./build/parameter-example/falconfoc-config-store-demo.exe
```

The application prepares a complete protection candidate, saves it to slot A,
and reboots the simulated storage. It initializes a fresh virtual Runtime from
the restored configuration and checks every persisted value plus confirmed
INHIBIT and zero accepted updates. The demo tears the first payload programming
unit of a save to slot B, reboots, and restores A. It then commits B, reboots,
and restores the updated settings. A second Host device keeps its own record.

The output includes the observed API return codes, selected slot, sequence, and
schema. The public codec is also exercised directly. The demo compares encoded
bytes with the stored record, rejects an encoder CPR mismatch, and detects a
corrupted payload. Save uses the offline API while the application has no live
Runtime for that device. The demo uses an application-assigned virtual setup
identity and the Host storage model.

For independent consumption, copy this directory outside the source tree and
pass `-DFALCONFOC_SOURCE_DIR=<FalconFOC source checkout>` at configure time.
The config store example links the public `falconfoc-config-store-host` and
`falconfoc-virtual-motor` targets. Omit `.exe` from the run command on platforms
that use an unsuffixed executable.
