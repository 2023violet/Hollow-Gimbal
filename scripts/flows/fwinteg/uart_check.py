"""NEXT-FW-INTEG-01R UART readback through the real Python CLI and Host simulators.

Usage: python -I uart_check.py <baseline demo jsonl> <simulator dir> <output dir>
Exits nonzero if the default demo differs from baseline (sequence excluded) or any
readback/locked-policy response differs from the expected value.
"""
import json
import subprocess
import sys
from pathlib import Path

BASELINE, SIMDIR, OUT = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
OUT.mkdir(parents=True, exist_ok=True)
CLI = Path("tools/falconfoc-cli/falconfoc_cli.py")
EXT = ".exe" if sys.platform == "win32" else ""
failures = []


def run(sim, *args):
    result = subprocess.run(
        [sys.executable, "-B", str(CLI), "--simulator", str((SIMDIR / sim).resolve()) + EXT, *args],
        capture_output=True,
        text=True,
    )
    return result.stdout + result.stderr, result.returncode


def records(text):
    out = []
    for line in text.splitlines():
        if line.startswith("{"):
            record = json.loads(line)
            record.pop("sequence", None)
            out.append(record)
    return out


# Intentional differences of the default demo from the option-matched baseline:
# capability bits 4/5 (remote mode switch, controller tuning), the 12 bytes M1 appended
# to TELEMETRY_READ and the 8 bytes P1 appended (rotor position and velocity; bytes
# 38/42 are output-side but equal the rotor side at the boot gear ratio 1 and zero 0).
# Only these are tolerated; anything else still fails the check.
M1_CAPABILITIES = {"baseline": 15, "current": 63}
M1_TELEMETRY_KEYS = ("id_target_a", "iq_target_a", "tuning_revision",
                     "rotor_position_rad", "rotor_velocity_rad_s")


def explained(record, role):
    record = dict(record)
    if record.get("message") in ("hello", "capability") and "capabilities" in record:
        if record["capabilities"] != M1_CAPABILITIES[role]:
            return record
        record["capabilities"] = "M1"
        record.pop("payload_hex", None)
    if record.get("message") == "telemetry":
        if role == "current" and not all(key in record for key in M1_TELEMETRY_KEYS):
            return record
        for key in M1_TELEMETRY_KEYS:
            record.pop(key, None)
        record.pop("payload_hex", None)
    return record


EXPECTED = {
    "foc-serial-sim": {16: 7, 17: 0.0, 32: 1, 33: 1, 34: 0, 35: 0, 36: 0, 37: 0, 38: 2048.0, 39: 2048.0,
                       40: 2048.0, 41: 65536, 42: 2, 43: 2, 44: 0, 45: 0},
    "foc-serial-sim-verification": {16: 11, 17: 1.2509765625, 32: 2, 33: 2, 34: 0, 35: 0,
                                    36: 0x56464331, 37: 0x01210001, 38: 2031.5, 39: 2063.25,
                                    40: 2017.75, 41: 65536, 42: 1, 43: 2, 44: 2, 45: 0},
}
LOCKED = [(("request", "param-write", "--id", "38", "--type", "f32", "--value", "2000"), "NOT_SUPPORTED"),
          (("request", "param-write", "--id", "17", "--type", "f32", "--value", "0"), "NOT_SUPPORTED"),
          (("request", "arm"), "OUTPUT_LOCKED"), (("request", "start"), "OUTPUT_LOCKED"),
          (("request", "save"), "NOT_SUPPORTED"), (("request", "stop"), "ACCEPTED")]

for sim, expected in EXPECTED.items():
    tag = "default" if sim == "foc-serial-sim" else "verification"
    demo, code = run(sim, "demo")
    (OUT / f"uart-demo-{tag}.jsonl").write_text(demo + f"exit {code}\n")
    if tag == "default" and [explained(r, "current") for r in records(demo)] != [
            explained(r, "baseline") for r in records(BASELINE.read_text(encoding="utf-8"))]:
        failures.append("default demo differs from baseline beyond the explained M1 changes")
    log = []
    for identifier in list(expected) + [46]:
        text, code = run(sim, "request", "param-read", "--id", str(identifier))
        log.append(text.strip() + f" [id {identifier} exit {code}]")
        got = records(text)[0]
        if identifier == 46:
            ok = got["result"] == "INVALID_ARGUMENT"
        else:
            ok = got["result"] == "OK" and got["value"] == expected[identifier] and not got["writable"]
        if not ok:
            failures.append(f"{tag} id {identifier}: {got}")
    (OUT / f"uart-param-read-{tag}.txt").write_text("\n".join(log) + "\n")
    log = []
    for args, result in LOCKED:
        text, code = run(sim, *args)
        log.append(text.strip() + f" [{' '.join(args)} exit {code}]")
        if records(text)[0]["result"] != result:
            failures.append(f"{tag} {' '.join(args)}: {records(text)[0]['result']}")
    (OUT / f"uart-locked-{tag}.txt").write_text("\n".join(log) + "\n")

print("FAILURES: " + "; ".join(failures) if failures else "ALL UART CHECKS PASS")
sys.exit(1 if failures else 0)
