# FalconFOC motion-protection example

This independent Host consumer links the public `falconfoc-virtual-motor`
target. It drives the production Runtime, H1 encoder path, H0 output adapter
and virtual PMSM with a deterministic clock. The profile enables protection
explicitly and binds it to the same continuous motor-shaft frame as the
Encoder and IMPEDANCE policy.

```powershell
cmake -S examples/motion-protection -B build/motion-protection-example -G Ninja
cmake --build build/motion-protection-example --target falconfoc-motion-protection-demo
ctest --test-dir build/motion-protection-example -R '^test-falconfoc-motion-protection-example$' --output-on-failure
& ./build/motion-protection-example/falconfoc-motion-protection-demo.exe
```

The target works in an external checkout with CMake 3.25+, Ninja and a C11
compiler. Copy the example and run the first command with the copied
directory for `-S` and `-DFALCONFOC_SOURCE_DIR=<FalconFOC checkout>`. Use a
separate build directory per compiler and omit `.exe` where appropriate.

| Trace label | Demonstrated behavior |
| --- | --- |
| `CONFIG` | Runtime initialization rejects an invalid interval, nonfinite bound, inconsistent frame and invalid following-error duration. CURRENT has an explicit unsupported status with limits enabled. |
| `IN_BOUNDS` | POSITION and IMPEDANCE accept in-range targets and reject both absolute out-of-range targets. |
| `INWARD_RETURN` | POSITION returns inward from above the limit and VELOCITY returns inward from below. |
| `OUTWARD_VETO` | The H0-consumed motion request cannot continue outward. An IMPEDANCE reference that points inward still fails if its net q-current request points outward after applying feedforward. |
| `INERTIAL_CROSSING` | A fast initial shaft crosses the commanded boundary in the virtual plant. There are no added UPDATEs after the software stop. |
| `FOLLOWING_STOP` | Continuous excess error under a load beyond the command cap triggers Runtime STOP, confirmed INHIBIT and a new session. The diagnostic is separate from Safety fault bits. |
| `BOUNDED_REVERSAL` | A bounded quintic reversal sees short excess error and continues without a sustained trip. |
| `OUTPUT_GUARD` | Invalid Sensor, adapter rejection and independent STOP retain the existing output confirmation and session semantics. |
| `MULTI_TURN` | Continuous motor-shaft position advances beyond one turn without wrapping the protected coordinate. |

The limit constrains position or velocity references and the requested
q-current direction. The plant can still cross a boundary with momentum or
external load. The example reports `physical_applied=false`; the Host
adapter confirms only its software output disposition.
