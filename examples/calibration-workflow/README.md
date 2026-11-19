# FalconFOC Calibration Workflow Host example

`falconfoc-calibration-workflow-demo` is a standalone C11 consumer. It opens a
`FULL_PROFILE` session, verifies the fixture identity, streams observations
into the existing calibration algorithms, and prints each stage's capture
status and measured quality.

```powershell
cmake -S examples/calibration-workflow -B build/calibration-demo -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
cmake --build build/calibration-demo --target falconfoc-calibration-workflow-demo
ctest --test-dir build/calibration-demo -V -R '^test-falconfoc-calibration-workflow-demo$'
```

For an external consumer, copy this directory away from the FalconFOC source
tree and configure from the copied directory. Pass
`-DFALCONFOC_SOURCE_DIR=<absolute-path-to-FalconFOC-source>` to CMake. The source
can be a Git-free copy. The example uses public headers and links the
`falconfoc-calibration-application` and `falconfoc-virtual-motor` CMake targets.
The latter supplies the baseline Host Runtime profile.

The example can export the same observations it submits to the workflow to a
new CSV path. Its usual diagnostics still go to stdout.

```powershell
build/calibration-demo/falconfoc-calibration-workflow-demo.exe --export build/calibration-demo/samples.csv
python tools/falconfoc-calibration-replay/generate_fixtures.py build/calibration-demo/falconfoc-calibration-workflow-demo.exe build/recreated-calibration-packs
```

The generator records the CSV SHA-256 and capture setup in a measurement
manifest. See the [standard Host packs](../../tools/falconfoc-calibration-replay/fixtures/README.md)
for the sample counts and timestamps.

| Stage | `HOST_SYNTHETIC` observations and checks |
| --- | --- |
| ADC zero | Three ADC counts with small deterministic noise and explicit zero-current attestation |
| Electrical R/L | One fixed d-axis, standstill current endpoints and applied interval-average voltages from a held-voltage fixture |
| Encoder LUT | Separate capture IDs and disjoint raw codes for training and holdout, independent angular reference and holdout noise |
| Rotor and electrical offset | Separate captures of raw encoder codes and aligned electrical references with angular noise, after qualifying the LUT |
| Mechanical J/B | Same-aperture speed and d/q current with signed external load, qualified Kt, and alternating 940/1060 µs intervals |
| Servo K/D | Qualified J/B and Kt, with the caller's ideal dynamics target and gain limits |

The example scores the identified R/L and J/B on separate waveforms that
never enter the estimators. The generated traces describe synthetic
fixtures. Kt is a qualified fixture input with its own capture identity.
`FULL_PROFILE` includes one R/L axis and produces reviewable K/D gains.
The angle captures use the reversed acquisition convention. The rotor fit
identifies its sign relative to the electrical reference and admission
resolves the normal direction used by the new encoder context.
The gains remain command candidates. The new Runtime uses the admitted
rotor mapping, ADC offsets and qualified Kt. The new encoder context owns
the qualified LUT.

The program rejects a review for a different encoder, requires explicit
review for application, and compares the reviewed configuration against the
original with Config Inspector. It verifies fresh Runtime readback and
scores the new encoder and rotor mapping together using raw codes held
back from every fitting and LUT holdout capture. The Config Store
compatibility check reports an identity mismatch against the original
trusted offset. Both Runtime instances remain in confirmed software
INHIBIT with zero output updates. Additional sessions demonstrate
duplicate rejection and explicit cancellation. The example exercises
Host software only and performs no persistence.
