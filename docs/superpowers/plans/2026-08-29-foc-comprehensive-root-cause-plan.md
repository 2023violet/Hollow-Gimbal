# AT32F415 + DRV8311H comprehensive low-voltage open-loop root-cause plan

## Goal

Use a staged, orthogonal diagnostic flow to distinguish PWM mapping, ADC rank,
CSA bias/gain, ADC trigger timing, FOC motion, encoder, DRV/power, and
mechanical torque causes of low-voltage open-loop stall/high current.

## Fixed constraints

- Preserve `OPEN_LOOP_SOFTWARE_PATH=PASS` and keep physical-operation and root
  cause status unverified/unresolved until repeated board evidence exists.
- Preserve the dirty working tree; do not reset, clean, checkout, delete, or
  overwrite unrelated user/generated files.
- Product defaults remain unchanged. Experimental mappings, trigger values,
  trace buffers, and Shell commands are `TM_BENCH_DIAG`-only, RAM-only, and
  IDLE/stopped gated.
- No energized calibration, I-f, encoder offset search, speed loop, or long
  continuous run.
- Use 12 V/1 A and short pulses. Any current/VBUS/data/mechanical anomaly is an
  immediate PSU cutoff; do not wait for Shell output.

## Tasks

1. Freeze current image/worktree baseline before changing motor control files.
2. Add a 64-entry fixed-point RAM-only FOC trace and first-fault hardware
   register snapshot, with stopped-only bounded Shell readback.
3. Add independent bench-only six-way PWM mapping, six-way ADC rank mapping,
   and bounded TMR1 CH4 trigger compare overrides; keep current defaults.
4. Add a 32-entry bench-only encoder trace for manual forward/reverse sweep.
5. Add the allowlisted board matrix runner and update Code Wiki, scripts README,
   safety procedure, session memory, and maintenance log.
6. Extend host tests for trace, mapping, trigger, encoder, FOC math and safety;
   run host/CTest/static-contract lanes and all five firmware builds.
7. With the verified bench ELF, run static, encoder, mapping, ADC timing,
   phase-seed, and short voltage/direction matrices. Stop at the first gate
   failure and analyze only after the motor is stopped.
8. Classify one primary root cause only when the minimum command reproduces and
   the same-condition repair repeats twice; otherwise retain blocked status.

## Verification

All new behavior requires a failing host test before production code. Final
checks are the host test script, independent CTest, the FOC peripheral static
contract, all bench/product firmware builds, scoped whitespace/link checks,
and a review of the working-tree diff against the pre-existing baseline.
