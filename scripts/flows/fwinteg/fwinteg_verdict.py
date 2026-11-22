"""NEXT-FW-INTEG-01R verdict over one run directory.

Usage: python -I fwinteg_verdict.py <run dir> <expected_tests.tsv>

Reads only files written by run_fwinteg.sh and decides each category on its own:
step exit codes, per-toolchain CTest inventories against the frozen expected list
(names, not counts), witnesses, ARM images, static checks, UART, Legacy presets,
Strict Scope all and Legacy Host. Anything failed, missing, incomplete, duplicated,
unexpected, skipped or not run makes the exit code 1. Exit 0 needs every category.
"""

import collections
import hashlib
import json
import re
import sys
from pathlib import Path

RUN = Path(sys.argv[1])
EXPECTED_FILE = Path(sys.argv[2])
TOOLCHAINS = ("clang", "gcc", "sanitizer")
ARM_IMAGES = {
    "default": "falconfoc-v2-at32f415",
    "verification": "falconfoc-v2-at32f415-calibration-verification",
    "default-capture": "falconfoc-v2-at32f415",
    "verification-capture": "falconfoc-v2-at32f415-calibration-verification",
    "default-trace": "falconfoc-v2-at32f415",
}
PRESETS = ("fw-full-diag-carrier-debug", "fw-standard-carrier-minsizerel", "fw-tiny-carrier-minsizerel")
WITNESS = {
    "test-foc-board-boot": ("accepted supply 0x56464331 review 0x01210001", "reverse check",
                            "calibrated boot: FAULT_CLEAR ACCEPTED", "nominal boot: FAULT_CLEAR ACCEPTED",
                            "locked RUNNING: 1 UPDATE declined", "test-foc-board-boot: all cases passed"),
    "test-foc-board-boot-default": ("default boot: NOMINAL_CONFIGURATION + CALIBRATION_NOT_AVAILABLE",),
}
REQUIRED_STEPS = (
    ["baseline"]
    + [f"matrix-{tc}" for tc in TOOLCHAINS]
    + [f"witness-{tc}-{test}" for tc in TOOLCHAINS for test in WITNESS]
    + [f"arm-{name}-{phase}" for name in ARM_IMAGES for phase in ("configure", "build")]
    + ["arm-static", "uart-check"]
    + [f"preset-{preset}" for preset in PRESETS]
    + ["quality-gate", "fault-injection"]
)
RESULT = re.compile(r"Test +#\d+: (\S+) \.+[ *]*(Passed|\*{0,3}Failed|\*{0,3}Not Run|\*{0,3}Skipped|"
                    r"\*{0,3}Exception|\*{0,3}Timeout)")
RESULT_ANY = re.compile(r"Test +#\d+: (\S+) \.+")
NOT_RUN = re.compile(r"(\d+)/\d+ Test +#\d+: (\S+) .*Not Run|Unable to find executable")

failures = []
report = collections.OrderedDict()


def fail(category, detail):
    failures.append(f"{category}: {detail}")


def read(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None


# Steps -------------------------------------------------------------------------------
steps = collections.OrderedDict()
seen = collections.Counter()
text = read(RUN / "steps.tsv")
if text is None or not text.rstrip().endswith("FWINTEG-STEPS-END"):
    fail("steps", "steps.tsv missing or incomplete")
    text = text or ""
for line in text.splitlines()[1:]:
    parts = line.split("\t")
    if len(parts) == 3:
        name, code, log = parts
        seen[name] += 1
        steps[name] = {"exit": code, "log": log}
for name in REQUIRED_STEPS:
    if name not in steps:
        fail("steps", f"required step {name} did not run")
    elif seen[name] != 1:
        fail("steps", f"step {name} recorded {seen[name]} times")
    else:
        step = steps[name]
        if step["exit"] != "0":
            fail("steps", f"{name} exit {step['exit']}")
        log = RUN / step["log"]
        if not log.is_file() or log.stat().st_size == 0:
            fail("steps", f"{name} log {step['log']} missing or empty")
for name in steps:
    if name not in REQUIRED_STEPS:
        fail("steps", f"unexplained step {name}")
report["steps"] = {name: steps.get(name, {"exit": "missing"})["exit"] for name in REQUIRED_STEPS}

# Expected inventory ---------------------------------------------------------------------
expected = collections.defaultdict(set)
for line in (read(EXPECTED_FILE) or "").splitlines():
    if line and not line.startswith("#"):
        suite, test = line.split("\t")
        expected[suite].add(test)
if not expected:
    fail("inventory", "expected_tests.tsv missing or empty")
expected_total = sum(len(v) for v in expected.values())

# Matrices -------------------------------------------------------------------------------
matrix_report = {}
for tc in TOOLCHAINS:
    counts = collections.Counter(expected=expected_total)
    status = read(RUN / "matrix" / f"{tc}-status.tsv")
    if status is None or not status.rstrip().endswith(f"FWINTEG-MATRIX-END {tc}"):
        fail(f"matrix-{tc}", "status file missing or incomplete")
        status = status or ""
    codes = {}
    for line in status.splitlines()[1:]:
        parts = line.split("\t")
        if len(parts) == 4:
            if parts[0] in codes:
                fail(f"matrix-{tc}", f"suite {parts[0]} recorded twice")
            codes[parts[0]] = parts[1:]
    for suite in sorted(set(expected) | set(codes)):
        if suite not in expected:
            fail(f"matrix-{tc}", f"unexpected suite {suite}")
            continue
        if codes.get(suite) != ["0", "0", "0"]:
            fail(f"matrix-{tc}", f"suite {suite} configure/build/ctest = {codes.get(suite, 'not recorded')}")
        log = read(RUN / "matrix" / f"{tc}-{suite}.log")
        if log is None:
            fail(f"matrix-{tc}", f"suite {suite} log missing")
            counts["missing"] += len(expected[suite])
            continue
        if not log.rstrip().endswith(f"FWINTEG-SUITE-END {suite}"):
            fail(f"matrix-{tc}", f"suite {suite} log truncated")
        names = collections.Counter()
        for line in log.splitlines():
            if RESULT_ANY.search(line):
                match = RESULT.search(line)
                name = RESULT_ANY.search(line).group(1)
                names[name] += 1
                state = match.group(2).strip("*") if match else "Unknown"
                counts["discovered"] += 1
                key = {"Passed": "passed", "Failed": "failed", "Not Run": "not_run",
                       "Skipped": "skipped"}.get(state, "other")
                counts[key] += 1
                if key != "passed":
                    fail(f"matrix-{tc}", f"{suite}/{name} {state}")
        for name, number in names.items():
            if number > 1:
                fail(f"matrix-{tc}", f"{suite}/{name} reported {number} times")
                counts["duplicate"] += number - 1
        for name in sorted(expected[suite] - set(names)):
            fail(f"matrix-{tc}", f"{suite}/{name} expected but not reported")
            counts["missing"] += 1
        for name in sorted(set(names) - expected[suite]):
            fail(f"matrix-{tc}", f"{suite}/{name} not in the expected inventory")
            counts["unexpected"] += 1
        want = f"100% tests passed out of {len(expected[suite])}"
        if want not in log:
            fail(f"matrix-{tc}", f"suite {suite} lacks '{want}'")
    for key in ("discovered", "passed", "failed", "skipped", "not_run", "other", "missing", "unexpected",
                "duplicate"):
        counts.setdefault(key, 0)
    matrix_report[tc] = dict(counts)
report["matrix"] = matrix_report

# Witnesses ------------------------------------------------------------------------------
for tc in TOOLCHAINS:
    for test, lines in WITNESS.items():
        content = read(RUN / f"witness-{tc}-{test}.txt") or ""
        for line in lines:
            if line not in content:
                fail("witness", f"{tc} {test} lacks '{line}'")

# ARM ------------------------------------------------------------------------------------
arm = {}
for name, image in ARM_IMAGES.items():
    binary = RUN / f"arm-{name}" / f"{image}.bin"
    if binary.is_file() and binary.stat().st_size > 0:
        arm[name] = hashlib.sha256(binary.read_bytes()).hexdigest()
    else:
        fail("arm", f"{name} BIN missing")
report["arm_bin_sha256"] = arm

# Content checks with their own verdicts ----------------------------------------------
CONTENT = {
    "arm-static": (("ALL STATIC CHECKS PASS",), ("FAILURES",)),
    "uart-check": (("ALL UART CHECKS PASS",), ("FAILURES",)),
    "quality-gate": (("[quality_gate] all requested checks passed.", "[Total]   PASS",
                      "[falconfoc_source_contract] checked C/H files"), ("FAIL",)),
    "legacy-host": (("100% tests passed out of 45", "[run_host_tests] all host tests passed."),
                    ()),
    "fault-injection": (("FAULT INJECTION PASS",), ("FAULT INJECTION FAIL",)),
}
LOGS = {"legacy-host": "quality-gate.log"}
for name, (needed, forbidden) in CONTENT.items():
    content = read(RUN / LOGS.get(name, f"{name}.log")) or ""
    for marker in needed:
        if marker not in content:
            fail(name, f"lacks '{marker}'")
    for marker in forbidden:
        if marker in content:
            fail(name, f"contains '{marker}'")
report["categories"] = {name: not any(f.split(":", 1)[0] == name for f in failures) for name in
                        ("steps", "matrix-clang", "matrix-gcc", "matrix-sanitizer", "witness", "arm",
                         "arm-static", "uart-check", "quality-gate", "legacy-host", "fault-injection")}
report["failures"] = failures
report["verdict"] = "PASS" if not failures else "FAIL"
(RUN / "verdict.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
for tc, counts in matrix_report.items():
    print(f"{tc}: expected {counts['expected']} discovered {counts['discovered']} passed {counts['passed']} "
          f"failed {counts['failed']} skipped {counts['skipped']} not-run {counts['not_run']} "
          f"missing {counts['missing']} unexpected {counts['unexpected']} duplicate {counts['duplicate']}")
for category, ok in report["categories"].items():
    print(f"{category}: {'PASS' if ok else 'FAIL'}")
for item in failures[:40]:
    print("  - " + item)
print(f"VERDICT {report['verdict']} ({len(failures)} findings)")
sys.exit(0 if not failures else 1)
