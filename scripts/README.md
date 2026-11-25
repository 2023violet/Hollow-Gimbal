# FalconFOC V2 Scripts

The Legacy Themis scripts (run.sh, presets, shell probes, board smoke flows,
Legacy host tests) were removed on 2026-11-24 with the Legacy firmware; they
remain at the Git tag `legacy-final`.

## Entry points

| Script | Purpose |
| --- | --- |
| `scripts/gates/check_falconfoc_quality_gate.ps1` | V2 quality gate: `git diff --check`, source contract, CMake quality |
| `scripts/gates/check_falconfoc_source_contract.ps1` | C/H header sections, end-of-file marker, CMake header and kebab-case targets, README build command, for every V2 source root |
| `scripts/gates/check_cmake_quality.ps1` | `# Themis` CMake header comment and `cmake-format --check` for the V2 CMake projects and the ARM toolchain file |
| `scripts/format_themis.ps1` | clang-format / cmake-format over the V2 trees |
| `scripts/clang-format.cmd` | Resolves the bundled `tools/clang-format/clang-format.exe` or one on PATH |
| `scripts/flows/fwinteg/run_fwinteg.sh` | Full Board1 integration run (see below) |
| `scripts/sdk/build_sdk_package.py`, `check_sdk_package.py` | Servo SDK packaging and release check ([SDK packaging](../docs/servo/sdk-packaging.md)) |

Run the quality gate from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_falconfoc_quality_gate.ps1 -RepoRoot <repo-root> -RequireCMakeFormatter
```

`-RequireCMakeFormatter` turns a missing `cmakelang` into a failure; without it
the formatter check is skipped with a warning (CI always requires it).

## fwinteg

`bash scripts/flows/fwinteg/run_fwinteg.sh <new output dir>` from the
repository root. It needs CMake ≥ 3.25, Ninja, Clang with ASan/UBSan, MinGW
GCC, Arm GNU Toolchain, Python 3, pwsh, git and cmakelang, and takes tens of
minutes; run it alone.

Steps, each with its raw exit code in `<out>/steps.tsv`:

1. `baseline` — ARM images of the frozen baseline commit, for the static comparison.
2. `matrix-{clang,gcc,sanitizer}` — every V2 Host project (`run_matrix.ps1`);
   the CTest names must equal `expected_tests.tsv`.
3. `witness-*` — boot witnesses from `test-foc-board-boot(-default)`.
4. `arm-*` — five ARM images (default, verification, two capture builds, trace).
5. `arm-static` — `arm_static.py`: locked prologue digests, literals, output lock.
6. `uart-check` — SIL UART demonstration against the Host matrix.
7. `quality-gate` — `check_falconfoc_quality_gate.ps1 -RequireCMakeFormatter`.
8. `fault-injection` — `test_fault_injection.py` drives `--stub` runs with
   `FWINTEG_INJECT` faults and checks every one is caught.

The top-level exit code is the verdict of `fwinteg_verdict.py`
([NEXT-FW-INTEG-01R](../docs/evidence/2026-11-21-next-fw-integ-01r.md)). A
change to the CTest inventory must be explained in `expected_tests.tsv`; a new
locked prologue needs a reviewed digest in `arm_static.py`.
