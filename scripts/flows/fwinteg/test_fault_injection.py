"""Fault injection through the top-level run_fwinteg.sh process (stub mode).

Usage: python -I test_fault_injection.py <new work dir>

Each case runs `bash run_fwinteg.sh <dir> --stub` with FWINTEG_INJECT set and checks
the top-level exit code: nonzero for every fault, zero only for the all-green control
group, whose verdict must also show every required step completed.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORK = Path(sys.argv[1])
CASES = {
    "control": ("", 0),
    "clang-fail": ("clang-fail", 1), "gcc-fail": ("gcc-fail", 1), "sanitizer-fail": ("sanitizer-fail", 1),
    "partial-run": ("partial-run", 1), "not-run": ("not-run", 1), "skipped": ("skipped", 1),
    "removed-test": ("removed-test", 1), "duplicate-test": ("duplicate-test", 1),
    "unexpected-test": ("unexpected-test", 1), "truncated-log": ("truncated-log", 1),
    "missing-log": ("missing-log", 1), "arm-build-fail": ("arm-build-fail", 1),
    "static-fail": ("static-fail", 1), "uart-fail": ("uart-fail", 1), "gate-fail": ("gate-fail", 1),
    "empty-step": ("empty-step", 1), "skip-step": ("skip-step", 1),
}

WORK.mkdir(parents=True)
rows, ok = [], True
for name, (fault, want) in CASES.items():
    out = WORK / name
    env = dict(os.environ, FWINTEG_INJECT=fault)
    process = subprocess.run(["bash", str(HERE / "run_fwinteg.sh"), str(out), "--stub"], env=env,
                             capture_output=True, text=True, timeout=300)
    (WORK / f"{name}.log").write_text(process.stdout + process.stderr, encoding="utf-8")
    verdict = json.loads((out / "verdict.json").read_text(encoding="utf-8")) if (out / "verdict.json").is_file() else {}
    findings = verdict.get("failures", [])
    passed = (process.returncode == 0) == (want == 0) and (want == 0) == (not findings)
    if name == "control":
        passed = passed and all(code == "0" for code in verdict.get("steps", {}).values()) and bool(verdict.get("steps"))
    ok = ok and passed
    rows.append(f"{name:16s} top-level exit {process.returncode} findings {len(findings):3d} "
                f"{'OK' if passed else 'WRONG'}  {findings[0] if findings else ''}")
summary = "\n".join(rows) + "\n" + ("FAULT INJECTION PASS" if ok else "FAULT INJECTION FAIL") + "\n"
(WORK / "summary.txt").write_text(summary, encoding="utf-8")
print(summary, end="")
sys.exit(0 if ok else 1)
