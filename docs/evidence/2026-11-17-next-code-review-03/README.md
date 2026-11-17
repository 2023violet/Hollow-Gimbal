# CR03 audit evidence

The report is [NEXT-CODE-REVIEW-03](../2026-11-17-next-code-review-03.md).
Verdict: **IMPROVE / P2**, with no confirmed P0/P1 or BLOCK.

This is a read-only source review followed by the user's explicitly authorized
Git delivery. The original audit snapshot and complete logs are external to
the repository. `readonly-verification.json` describes the review phase BEFORE
the authorized commits; HEAD/status intentionally change during publication.
Production sources, formal tests, Golden and build configuration were not
changed by CR03. Existing completed work was committed separately at
02425e06556accd3a38762ed1412bb8ffcfebae3.

## Results

- 61233 prediction cases, 20000 PI cases, 20021 Position cases.
- 72 independent error/transaction checks, ordinary and ASan/UBSan.
- Core 15/15, Runtime 11/11, Core sanitizer 15/15, Simulation sanitizer 15/15.
- Six isolated production mutations. The error-branch integral-commit mutant
  passes all 15 Core tests, but fails the new external negative probe.
- Default Board software-envelope prediction maximum observed error:
  8.983257844e-7 rad. This is not physical motor accuracy or MCU timing proof.
- P2-B remains **OPEN — NEXT-QUALITY-GATE-01**. Strict/CI gate behavior is not
  renamed PASS and the PowerShell gate was not modified.

`motion-numerical-oracle.json` contains stored-input Decimal references and
worst cases. `motion-test-matrix.json` contains actual commands, outcomes and
log hashes, including early audit-driver assumptions corrected after evidence.
The 16.9 MB full CSV and source/build copies remain in the external directory
`D:/study/Software/FalconFOC-code-review-03-20261117`; the CSV hash is in the JSON.
Historical paths in command records identify the actual local experiment;
they are not a requirement that a consumer reproduce that directory layout.

## Reproduce the independent negative probe

The `.c.txt` files are audit artifacts, not product compilation units or new
formal tests. Copy `motion_errors.c.txt` into a NEW external directory as
`motion_errors.c`. From the repository root, for a compiler with POSIX libm:

```sh
cc -std=c11 -O0 -I core/include core/src/foc_core.c core/src/foc_motion.c /new/audit/motion_errors.c -lm -o /new/audit/motion-errors
/new/audit/motion-errors
```

On Windows, use the equivalent installed C compiler and its math linkage.
Correct production code reports 72 checks passed and prints the historical
prediction counterexample. No hardware or serial port is opened.

For the A/B/C export, copy `motion_export.c.txt` and `predict_candidates.c.txt`
as C sources into the same new external directory, compile with the same
production Core sources, and send this line on stdin:

```text
P 0.35820722579956055 65535000 400 400 2000 0.0003
```

Output fields: kind, status_A, phase_A, status_B, phase_B, status_C, phase_C,
float_dt, float_delta, float_sum, float_model_contribution, sin_A, cos_A.
The independent exact-stored-input reference phase is approximately
0.9091056725647788 rad. Candidate B/C remain external prototypes.

## Boundaries

Board1 remains output locked. Software UPDATE acceptance is not physical
application. Hardware safety, MCU WCET, stack high-watermark and physical
motor operation remain unverified. No algorithm fix or next-file work is
included in this review delivery.
