# FalconFOC Servo Host example

`falconfoc-servo-demo` is a standalone C11 application that uses the public
Control Plane, Core, Runtime, Encoder, and Virtual Motor APIs. The default run
compares the same impedance reference with explicit model feedforward enabled
and disabled. `--inspect` lists capabilities and parameter readback.
`--iq-limit AMPS` sets an initial candidate for inspection or normal runs.
`--faults` exercises rejection and fault handling. `--recovery` clears a
qualified command timeout on the same Runtime and explicitly arms and starts
it again.

```powershell
cmake -S examples/servo -B build/servo-demo -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
cmake --build build/servo-demo --target falconfoc-servo-demo
& ./build/servo-demo/falconfoc-servo-demo.exe --help
& ./build/servo-demo/falconfoc-servo-demo.exe --inspect --iq-limit 1.8
& ./build/servo-demo/falconfoc-servo-demo.exe
& ./build/servo-demo/falconfoc-servo-demo.exe --faults
& ./build/servo-demo/falconfoc-servo-demo.exe --recovery
```

Pass `-DFALCONFOC_SOURCE_DIR=<checkout>` when building a copy outside this
repository. [Servo Quickstart](../../docs/servo/quickstart.md) explains the
timing, receipts, fault reasons, and external build. All parameters in the
example are Host model inputs. The output adapter reports
`physical_applied=false`.
