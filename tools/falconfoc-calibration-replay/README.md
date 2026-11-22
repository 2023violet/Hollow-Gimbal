# FalconFOC Calibration Replay

This Host tool checks a read-only Measurement Trace v1 pack and sends the same verified CSV bytes to the public C11 trace adapter and the existing Calibration Workflow. Python 3.10 or newer uses only the standard library. See the [V1 format, time contract, and Board1 acquisition requirements](../../calibration/MEASUREMENT_TRACE.md).

## Build and run

```powershell
cmake -S tools/falconfoc-calibration-replay -B build/calibration-replay -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/calibration-replay
ctest --test-dir build/calibration-replay --output-on-failure -R '^test-falconfoc-calibration-replay$'
$runner = 'build/calibration-replay/falconfoc-calibration-replay-runner.exe'
$tool = 'tools/falconfoc-calibration-replay'
python "$tool/replay.py" validate "$tool/fixtures/host-valid" --runner $runner
python "$tool/replay.py" replay "$tool/fixtures/host-valid" --runner $runner --json
python "$tool/replay.py" explain "$tool/fixtures/host-misaligned" --runner $runner
```

`validate` checks the manifest, content hash, identity claims, stage record counts, and the typed sample contract, without fitting. `replay` runs the formal Workflow and returns the quality and candidate disposition. `explain` also prints plain-language reasons. An invalid, incomplete or rejected operation exits 1. A completed selected-stage replay can succeed without an application candidate. Use `--json` for stable V1 field names. Numerical outputs are available under `workflow.metrics`; use the stage quality and the candidate disposition to interpret them.

For real-source packages, leave provenance unverified unless an independent acquisition owner has checked the setup and original capture. A caller-trusted `--attestation` file outside the pack can bind the exact manifest SHA-256, claimed source, and complete device/motor/encoder/frame identity to that review. The tool verifies that binding. It cannot observe the hardware or authenticate the statement from the file itself. An offline candidate still requires explicit SOFT-05 review and initialization. The CLI never applies or persists settings.

[Fixture provenance and regeneration](fixtures/README.md) · [Public C11 interface](../../calibration/include/foc_measurement_trace.h)

## Board1 acquisition

`board_capture.py` reads the versioned SRAM image held by the locked Board1
firmware and archives raw bytes and source declarations. It can prepare a
selected ADC_ZERO Trace V1 when the caller supplies the required witness.
See [Board1 capture and SOFT-06 import](../../firmware/at32f415/BOARD_CAPTURE.md).

A calibrated-configuration image (header `encoder_profile` 2) is read only with an
external configuration declaration: metadata format 2 for `import`,
`describe --configuration <file>`, or `locked_timing.py --capture-configuration <file>`.
Nominal images and existing V1 samples read as before. See
[configuration identity](../../firmware/at32f415/BOARD_CAPTURE.md#configuration-identity).

`board_adc_candidate.py` runs the importer, applies the zero-current gate and calls the
Host adapter `foc-board-adc-candidate` (existing ADC estimator, isolated
`foc_board_boot_select()` and readback). Board-recorded captures stop at
`PRECONDITION_UNVERIFIED`. See
[ADC zero candidate](../../firmware/at32f415/BOARD_CAPTURE.md#adc-zero-candidate).
