# NEXT-CODE-IMPROVE-02B Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Execute inline; the user has authorized implementation, no additional design approval is needed.

**Goal:** Close P2-02 by feeding bounded periodic electrical phase independently of continuous Float32 position.

**Architecture:** Sensor exposes the same observation's directed integer cycle count. A new Core helper applies integer electrical modulo, scaling and offset. Existing Board/Virtual producers provide phase to motion-enabled H1; H0 mailbox, prediction, lifecycle and Safety ownership stay unchanged.

**Tech Stack:** C11, CMake/Ninja, Clang/GCC, Python, ARM GNU soft-float.

**Spec:** `C:/Users/16429/.codex/attachments/403acb2b-a33a-4367-9030-e3df3ddce292/已粘贴的文本.txt`; approved proposal `D:/study/Software/FalconFOC-code-improve-02-20261116/PRODUCTION_CHANGE_PROPOSAL.md` §8.

## Global Constraints

- Host numeric budget `1e-5 rad`; this is not hardware acceptance.
- Keep Current/Velocity PI, Position P, Safety, output policy, UART V1 and hardware configuration unchanged.
- Preserve continuous position/trajectory and existing Host first-frame zero-speed policy.
- Keep old Rotor API; appended struct fields require consumer rebuild.
- All builds/evidence use new directories; preserve historical artifacts, CSV/JSON and Golden.
- Do not commit, push, merge, delete build directories or perform hardware operations.

## Review Focus

- Zero-initialized new flag must fail closed in motion-enabled mode, while direct-angle mode still works.
- Sensor reject/reset/first-frame must not publish stale integer phase as newly valid.
- Huge pole pairs and signed cumulative counts must avoid overflow and negative-remainder mistakes.
- Producer timestamps/config ownership must keep phase and continuous position from the same capture.
- Normal/early Host startup, STOP/FAULT and invalid-speed combinations must keep existing behavior.

### Task 1: Negative evidence and integer mapping

Files: `core/include/foc_motion.h`, `core/src/foc_motion.c`, `core/tests/test_foc_periodic_phase.c`, `core/CMakeLists.txt`; `sensors/include/foc_encoder.h`, `sensors/src/foc_encoder.c`, existing Sensor tests.

Interface: `foc_rotor_phase_from_counts(const struct foc_rotor_config *, uint32_t mechanical_phase_count, uint32_t counts_per_revolution, float *)`; append Feedback `uint32_t mechanical_phase_count`.

- [x] Preserve baseline and reproduce old real Encoder/H1/H0 precision failure in a new output directory.
- [x] Migrate independent double/int64 Oracle into Core tests; assert 2100 cases, circular error <=1e-5, proper invalid status/output clearing.
- [x] Add Sensor direction/modulo, first/reset/reject invariants; observe missing-API/field failures before implementation.
- [x] Implement helper and Sensor assignment only on accepted first/valid observations; run Core/Sensor tests.

### Task 2: Runtime and producers

Files: `runtime/include/foc_runtime.h`, `runtime/src/foc_runtime.c`, `firmware/at32f415/src/foc_board.c`, `simulation/src/foc_virtual_current.c`; actual motion-enabled test factories found by repository search.

Interface: append Sensor `bool electrical_phase_valid`; bounded `theta_elec_rad` plus existing valid/position/speed/time. CPR comes from the Encoder instance, rotor config from the Runtime instance.

- [x] Add Runtime negative phase tests and a real Sensor→H1→H0/Current test; missing flag/invalid angle must prevent UPDATE.
- [x] Change motion-enabled H1 to consume same-frame phase, finite position/velocity and finite electrical speed; keep direct-angle branch and mailbox unchanged.
- [x] Migrate Board/Virtual producers and test factories from reliable original coordinates; no fabricated counts from huge floats.
- [x] Verify long positive/negative runs, directions, offset, time wrap, Position/Trajectory, invalid phase, STOP/FAULT/session/update rejection; preserve first-frame timelines.

### Task 3: Product regression and isolated mutants

Files: new permanent phase/producer tests and their existing CMake registration; external execution scripts/logs.

- [x] Run real software Current/Velocity/Position/load/fault/CLI scenarios and trace raw sample→H1→H0 phase→Current/output.
- [x] In isolated copies, mutate accumulated float fallback, direction, poles, offset, mixed timestamp and invalid reuse; require compiled behavioral failures, with baseline passing.
- [x] Run Core/Sensors/Runtime/Simulation/Board/CLI and sanitizer, Legacy Host, Source/Formatter/CMake/Scope falconfoc/all.
- [x] Build ARM in a new directory and record binary/section/symbol/layout/stack changes and locked-output evidence.

### Task 4: Documentation and closeout

Files: relevant Core/Sensor/Runtime/Simulation README/API contracts, new `docs/evidence/2026-11-16-next-code-improve-02b.md`, append maintenance log.

- [x] Record authorization match, producer inventory, numeric worst case, regression/mutations, startup timelines, resources and limits.
- [x] Fresh independent final review of this stage's diff; address important findings with RED→GREEN evidence.
- [x] Verify old source/evidence/artifact preservation and no unauthorized changes. Stop after the final result.
