# FalconFOC external C11 consumers

`core-consumer` uses a separately installed static archive and public headers.
It takes calibrated pole pairs, electrical offset and PI gains as caller
settings, pairs a directed encoder count with phase-current and bus-voltage
observations, computes electrical phase, and cascades velocity PI to Current
PI. It checks signed SVPWM output requests, a rejected capture, STOP and a
latched FAULT. The serial caller owns permission to update and commits both
trial PI contexts after the Host sink accepts the request.

`runtime-consumer` links the standard Runtime and Sensor source targets. It
warms up one encoder instance, publishes same-capture motion feedback at H1,
supplies the reference four-rank ADC sample to H0, and polls Foreground on the
same microsecond clock. It publishes and checks lifecycle acknowledgements,
steps Current, revokes the command for STOP, and selects Velocity while
inhibited. It then publishes fresh signed targets and tests a latched WDT
fault. Runtime owns Safety and admission. `output_confirmed` describes the
closed G3 Host Mock's acknowledgement.

## Build outside the checkout

Run this PowerShell example from the checkout. Use `gcc` in place of `clang`
and a fresh `$external` directory to repeat the build with MinGW GCC. CMake
3.25 or newer and Ninja are required.

```powershell
$repo = (Get-Location).Path
$external = Join-Path ([IO.Path]::GetTempPath()) ('falconfoc-consumer-' + [guid]::NewGuid())
$compiler = (Get-Command clang).Source
New-Item -ItemType Directory -Path $external, "$external/modules" | Out-Null
Copy-Item -Recurse "$repo/core" "$external/core"
Copy-Item -Recurse "$repo/examples/external-consumer" "$external/app"

cmake -S "$external/core" -B "$external/core-build" -G Ninja -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$compiler"
cmake --build "$external/core-build"
cmake --install "$external/core-build" --prefix "$external/install"
Move-Item -LiteralPath "$external/install" -Destination "$external/sdk"

cmake -S "$external/app" -B "$external/core-app-build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$compiler" "-DCMAKE_PREFIX_PATH=$external/sdk"
cmake --build "$external/core-app-build"
ctest --test-dir "$external/core-app-build" -V -R '^external-core-consumer$'

foreach ($module in @('core', 'g3', 'runtime', 'sensors')) {
    Copy-Item -Recurse "$repo/$module" "$external/modules"
}
cmake -S "$external/app" -B "$external/runtime-app-build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$compiler" "-DCMAKE_PREFIX_PATH=$external/sdk" "-DFALCONFOC_RUNTIME_SOURCE_DIR=$external/modules"
cmake --build "$external/runtime-app-build" --target runtime-consumer
ctest --test-dir "$external/runtime-app-build" -V -R '^external-runtime-consumer$'
```

A third-party CMake project can use `find_package(FalconFOCCore CONFIG REQUIRED)`
and link `FalconFOC::Core`. Use a compatible compiler, target architecture and
C runtime for the library and application. `BUILD_TESTING` defaults to `ON`
for the Core repository tests, which include Legacy comparison coverage. Set
it to `OFF` when building from a copy of `core/` alone. Runtime currently uses
`falconfoc-pipeline` and `falconfoc-encoder` from the supplied module sources.

## Interface and ownership

The classifications describe the source contracts available in this engineering
baseline. Consumers compile against the matching headers and library. The v0.1
public context layouts can change before a release.

| Classification | Integration surface and owner |
| --- | --- |
| `PUBLIC_STABLE` | Core transforms, SVPWM, Current, rotor phase, basic Velocity and Position via `foc_core.h`, `foc_current.h` and `foc_motion.h`. Caller owns coherent SI input, timing, settings and state storage. Basic `foc_encoder.h` init/update owns conversion and quality after the caller supplies a timestamped frame. `foc_protocol.h` V1 wire layout and vectors are a separate transport contract. |
| `PUBLIC_EXPERIMENTAL` | `foc_runtime.h` and the G3 Device/Safety seam, optional control modes and protections, calibration candidates and reviewed application, FCAL, and the cold-path PMSM reachability API. Public Runtime calls use caller-allocated storage with H1/H0/Foreground ownership. Runtime takes immutable copied settings and publishes observable state; use snapshot and lifecycle APIs for readback. |
| `INTERNAL` | `core/src/foc_core_internal.h`, Runtime context implementation members, PI trial and commit internals, mailboxes, and G3 Mock internals. |
| `BOARD_SPECIFIC` | Raw ADC rank and PWM mapping conventions in G3's reference Device contract. `firmware/at32f415` owns target acquisition and the locked timer/bridge adapter. |
| `HOST_ONLY` | Closed G3 Mock, Virtual Motor, Host synthetic calibration, Host storage and Python CLI virtual workflows. |
| `NOT_VERIFIED` | Physical calibration, target WCET, hardware output timing and shutdown, and other devices' acquisition and power-stage adaptations. |

For Core, validate observation and command age and quality before stepping.
Use explicit `INHIBIT` behavior in the application output adapter when any
condition fails. The sample sink clears its request and records no update.
The test uses serial ownership and a fixed synthetic rotor.

For Runtime, initialize while quiescent. Publish command sequence and time
from Foreground, and preserve the sensor capture time from H1. Feed valid
observations while disabled, then ARM and START on different H0 events. The
next fresh sample can produce the first update. Confirm STOP at the adapter
scope before changing mode, and use new source identities for the new session.
Keep H0 higher priority than H1 and Foreground in an implementation with
preemption.

The existing [Servo example](../servo/README.md) adds Control Plane and Virtual
Motor. The [Calibration example](../calibration-workflow/README.md) acquires
Host observations, checks candidate identity and explicit review, applies the
settings in quiescence, and initializes new Runtime and encoder contexts for
readback. Calibration owns candidates; the Control Plane validates application
and configuration identity. Review and apply parameters there before using
the calibration values as Core or Runtime inputs.
