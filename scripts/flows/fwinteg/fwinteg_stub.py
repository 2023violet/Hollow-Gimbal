"""Synthetic step artifacts for fault injection of run_fwinteg.sh --stub.

Usage: python -I fwinteg_stub.py <step> <run dir> <expected_tests.tsv>

Writes the files a real step would leave, in the same formats, and exits with the
code a real step would return. FWINTEG_INJECT names one fault. Several faults make
the step lie (exit 0) so that only the content checks in fwinteg_verdict.py can
catch them. This never builds or runs product code.
"""

import collections
import os
import sys
from pathlib import Path

STEP, RUN, EXPECTED = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
FAULT = os.environ.get("FWINTEG_INJECT", "")
expected = collections.defaultdict(list)
for line in EXPECTED.read_text(encoding="utf-8").splitlines():
    if line and not line.startswith("#"):
        suite, test = line.split("\t")
        expected[suite].append(test)
IMAGES = {"default": "falconfoc-v2-at32f415", "verification": "falconfoc-v2-at32f415-calibration-verification",
          "default-capture": "falconfoc-v2-at32f415",
          "verification-capture": "falconfoc-v2-at32f415-calibration-verification",
          "default-trace": "falconfoc-v2-at32f415"}


def matrix(tc):
    status = ["suite\tconfigure\tbuild\tctest"]
    code = 0
    for suite in sorted(expected):
        tests = list(expected[suite])
        states = ["Passed"] * len(tests)
        lying_exit = False
        if FAULT == f"{tc}-fail" and suite == "core":
            states[0] = "***Failed"
            code = 1
        if FAULT == "not-run" and tc == "sanitizer" and suite == "runtime":
            states[0] = "***Not Run"
        if FAULT == "skipped" and tc == "clang" and suite == "sensors":
            states[0] = "***Skipped"
        if FAULT == "removed-test" and tc == "clang" and suite == "calibration":
            tests, states = tests[1:], states[1:]
        if FAULT == "duplicate-test" and tc == "gcc" and suite == "protocol":
            tests, states = tests + tests[:1], states + states[:1]
        if FAULT == "unexpected-test" and tc == "clang" and suite == "core":
            tests, states = tests + ["test-foc-not-in-inventory"], states + ["Passed"]
        if FAULT == "partial-run" and tc == "gcc" and suite == "tools-falconfoc-calibration-replay":
            continue
        lines = [f"{i + 1}/{len(tests)} Test #{i + 1}: {name} ........   {state}    0.01 sec"
                 for i, (name, state) in enumerate(zip(tests, states))]
        failed = sum(state != "Passed" for state in states)
        lines.append(f"{100 * (len(tests) - failed) // max(len(tests), 1)}% tests passed"
                     + (f", {failed} tests failed" if failed else "") + f" out of {len(tests)}")
        ctest = 1 if failed and not lying_exit and FAULT == f"{tc}-fail" else 0
        if not (FAULT == "truncated-log" and tc == "sanitizer" and suite == "simulation"):
            lines.append(f"FWINTEG-SUITE-END {suite}")
        if not (FAULT == "missing-log" and tc == "clang" and suite == "protocol"):
            (RUN / "matrix" / f"{tc}-{suite}.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
        status.append(f"{suite}\t0\t0\t{ctest}")
    status.append(f"FWINTEG-MATRIX-END {tc}")
    (RUN / "matrix" / f"{tc}-status.tsv").write_text("\n".join(status) + "\n", encoding="utf-8")
    print(f"stub matrix {tc}")
    return code


def main():
    if FAULT == "empty-step" and STEP == "uart-check":
        # Simulate a step that silently produced nothing: no log content, nonzero.
        return 127
    if STEP.startswith("matrix-"):
        return matrix(STEP.split("-", 1)[1])
    if STEP.startswith("witness-"):
        if STEP.endswith("-default"):
            print("default boot: NOMINAL_CONFIGURATION + CALIBRATION_NOT_AVAILABLE")
        else:
            print("accepted supply 0x56464331 review 0x01210001: stub\nreverse check, stub\n"
                  "calibrated boot: FAULT_CLEAR ACCEPTED -> stub\nnominal boot: FAULT_CLEAR ACCEPTED -> stub\n"
                  "locked RUNNING: 1 UPDATE declined, stub\ntest-foc-board-boot: all cases passed")
        return 0
    if STEP.startswith("arm-") and STEP.endswith("-build"):
        name = STEP[4:-6]
        folder = RUN / f"arm-{name}"
        folder.mkdir(exist_ok=True)
        if FAULT == "arm-build-fail" and name == "verification-capture":
            print("stub link error")
            return 1
        (folder / f"{IMAGES[name]}.bin").write_bytes(name.encode() * 16)
        print(f"stub ARM {name}")
        return 0
    if STEP == "arm-static":
        if FAULT == "static-fail":
            print("[verification-capture] prologue_identical: FAIL\nFAILURES: verification-capture:prologue")
            return 0  # lying exit: the content check must catch it
        print("ALL STATIC CHECKS PASS")
        return 0
    if STEP == "uart-check":
        if FAULT == "uart-fail":
            print("FAILURES: verification id 42: wrong direction")
            return 1
        print("ALL UART CHECKS PASS")
        return 0
    if STEP == "quality-gate":
        if FAULT == "gate-fail":
            print("[cmake_quality] check failed\n[Total]   FAIL")
            return 0  # lying exit
        print("[falconfoc_source_contract] checked C/H files: 1\n"
              "[quality_gate] all requested checks passed.\n[Total]   PASS")
        return 0
    if STEP == "fault-injection":
        print("FAULT INJECTION PASS (stub)")
        return 0
    print(f"stub {STEP}")
    return 0


sys.exit(main())
