# Standard Host measurement packs

Both packs use the deterministic SOFT-05 [calibration workflow example](../../../examples/calibration-workflow/README.md).
The manifests use `HOST_SYNTHETIC`, record seed `0` for the analytic generator,
and document synthetic device and instrument identities. The generator uses no PRNG.

| Pack | Expected trace result | SHA-256 of `samples.csv` |
| --- | --- | --- |
| `host-valid` | Seven workflow stages pass from 3,294 observations, with a candidate available for explicit review. | `f45c1e7d5f13f32405f94cba989f921f2c431db50bd9b3291966e492623e1c53` |
| `host-misaligned` | LUT training row 1 is rejected for `ALIGNMENT`. Encoder aperture is 40000 µs and the claimed reference aperture is 39980 µs, exceeding the declared 5 µs alignment budget. | `3f6b40a989757924b964a0edbb0712d267e690d0c1d18228b7488225d18e8f37` |

The valid sample counts are 64 ADC, 321 R/L, 256 LUT training, 171 LUT holdout,
40 Rotor, 40 electrical offset, 2,401 J/B, and one independent Kt observation.
The valid pack preserves the estimator inputs and chronological source times of
SOFT-05. The Kt observation appears first so it can qualify the observed-current
J/B path and later support K/D tuning. All timestamps are on fixture clock 1.
For streaming observations, `completion_us` represents synthetic availability
3 µs after source capture, as used by the original demo. Trigger timestamps and
Kt completion are unspecified. The LUT reference frame uses direction +1 and
the raw encoder acquisition uses direction −1.

The invalid pack takes two LUT training rows and one independent holdout row
from the valid pack. It shifts the first training reference 20 µs earlier and
records the resulting file hash so the replay reaches alignment validation.

For regeneration, pass the built example and a fresh output directory to
`generate_fixtures.py`. Generated files use LF line endings. Windows Clang
22.1.8 and MinGW GCC 16.2.0 Debug builds produce identical CSV bytes.
