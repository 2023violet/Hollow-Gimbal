# FalconFOC Servo Host example

`falconfoc-servo-demo` is a standalone C11 application that uses the public
Core, Runtime, Encoder, and Virtual Motor APIs. The default run compares
the same impedance reference with explicit model feedforward enabled and
disabled. Use `--faults` for the command, sample, lifecycle, and output
failure demonstrations.

```powershell
cmake -S examples/servo -B build/servo-demo -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
cmake --build build/servo-demo --target falconfoc-servo-demo
& ./build/servo-demo/falconfoc-servo-demo.exe
```

Pass `-DFALCONFOC_SOURCE_DIR=<checkout>` when building a copy outside this
repository. [Servo Quickstart](../../docs/servo/quickstart.md) explains the
timing, status, commands, and external build. All parameters in the example
are Host model inputs. The output adapter reports `physical_applied=false`.
