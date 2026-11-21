"""Exercise the three Host calibration product modes over the actual CLI."""

import argparse
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time


CLI = pathlib.Path(__file__).resolve().parents[2] / "tools/falconfoc-cli/falconfoc_cli.py"
PROFILES = ("GENERIC_RESEARCH_MOTOR", "CALIBRATION_SALIENT_MOTOR")


def find(rows, name):
    return [row for row in rows if row.get("event") == name]


def one(rows, name):
    matches = find(rows, name)
    assert len(matches) == 1, (name, matches)
    return matches[0]


def assert_preview(rows):
    for name in ("CANDIDATE_REVIEWED", "CONFIGURATION_APPLIED", "CONTROL_TRIAL", "REUSE_APPLIED"):
        assert not find(rows, name), (name, rows)


def assert_accepted(rows):
    applied = one(rows, "CONFIGURATION_APPLIED")
    assert applied["damping_filter_hz"] == 48
    trials = find(rows, "CONTROL_TRIAL")
    assert {(row["mode"], row["configuration"], row["reference"] > 0) for row in trials} == {
        (mode, version, positive)
        for mode in ("current", "velocity", "position", "impedance")
        for version in ("before", "after") for positive in (True, False)
    }
    assert len(trials) == 16 and all(row["stop_confirmed"] and row["faults"] == 0 for row in trials)
    one(rows, "CONTROL_QUALITY_PASS")
    assert one(rows, "CURRENT_BENEFIT_PASS")["policy_hz"] == 130
    return applied, trials


def stages(rows, mode):
    progress = find(rows, "CAL_STAGE")
    assert progress, ("missing timing", mode, rows)
    for row in progress:
        assert row["mode"] == mode and row["total_stages"] > 0
        assert row["user_interaction_us"] is None and row["eta_us"] is None
        for field in ("completed_stages", "reused_stages", "skipped_stages", "remaining_stages"):
            assert isinstance(row[field], list), row
        for field in ("host_wall_us", "host_compute_us", "simulated_motor_us"):
            assert row[field] is None or row[field] >= 0, row
    return progress


class Product:
    def __init__(self, simulator, name, output):
        self.base = [sys.executable, "-B", str(CLI), "calibrate", "--simulator",
                     str(simulator), "--profile-id", name]
        self.output = output

    def save(self, label, command, rows, elapsed, status):
        with (self.output / f"{label}.json").open("x", encoding="utf-8") as event_log:
            json.dump({"command": list(map(str, command)), "events": rows, "exit_status": status,
                       "host_wall_elapsed_ns": elapsed}, event_log, indent=2, allow_nan=False)

    def run(self, label, mode, *options, status=0):
        command = [*self.base, "--calibration-mode", mode, *map(str, options)]
        start = time.perf_counter_ns()
        result = subprocess.run(command, check=False, capture_output=True, text=True, timeout=60)
        assert result.returncode == status, (result.args, result.returncode, result.stdout, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.save(label, command, rows, time.perf_counter_ns() - start, status)
        return rows

    def selective(self, label):
        return Selective(self, label)


class Selective:
    def __init__(self, product, label):
        self.product = product
        self.label = label
        self.command_line = [*product.base, "--calibration-mode", "selective"]
        self.sequence = 0

    def __enter__(self):
        self.child = subprocess.Popen(self.command_line, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      text=True, encoding="utf-8")
        self.rows = queue.Queue()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        return self

    def read(self):
        for line in self.child.stdout:
            self.rows.put(line)
        self.rows.put(None)

    def __exit__(self, exception_type, *_):
        self.child.stdin.close()
        try:
            status = self.child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.child.kill()
            status = self.child.wait(timeout=10)
        self.reader.join(timeout=10)
        self.child.stdout.close()
        if exception_type is None:
            assert status == 0, status

    def command(self, line, *, error=None):
        self.sequence += 1
        start = time.perf_counter_ns()
        self.child.stdin.write(line + "\n")
        self.child.stdin.flush()
        rows = []
        while True:
            raw = self.rows.get(timeout=50)
            assert raw is not None, ("selective session closed", line, rows)
            event = json.loads(raw)
            rows.append(event)
            if event.get("event") == "REUSE_COMMAND_RESULT":
                break
        assert event["command"] == line.split()[0] and event["ok"] == (error is None), rows
        if error:
            assert error in {row.get("error") for row in rows}, rows
        self.product.save(f"{self.label}-{self.sequence:02d}", [*self.command_line, line], rows,
                          time.perf_counter_ns() - start, 0 if error is None else 2)
        return rows


def verify_profile(product, output):
    fcal = output / "qualified.fcal"
    preview = product.run("full-preview", "full", "--boot-trace", output / "boot.csv")
    assert_preview(preview)
    progress = stages(preview, "full")
    measured = {row["stage"]: row["simulated_motor_us"] for row in progress
                if row["status"] == "COMPLETED" and row["simulated_motor_us"]}
    assert {"ADC_ZERO", "BOOTSTRAP_SCAN", "ENCODER_LUT", "ROTOR", "ELECTRICAL_OFFSET",
            "ELECTRICAL_D", "ELECTRICAL_Q", "FLUX", "MECHANICAL"} <= measured.keys(), measured
    assert one(preview, "BOOTSTRAP_CAPTURE")["accepted_updates"] == 0
    reviewed = one(preview, "CANDIDATE_PREVIEW")["review_id"]
    approved = product.run("full-reviewed", "full", "--approve-candidate", reviewed,
                           "--save-config", fcal)
    assert {"REVIEW", "APPLICATION", "FCAL_SAVE"} <= {
        row["stage"] for row in stages(approved, "full") if row["status"] == "COMPLETED"
    }
    expected = assert_accepted(approved)
    saved = fcal.read_bytes()
    assert len(saved) == 548 and saved[:8] == b"FCAL\x01\x00\x00\x00"

    quick = product.run("quick-preview", "quick", "--load-config", fcal)
    assert_preview(quick)
    assert one(quick, "PRODUCT_FCAL_SOURCE")["status"] == "SOURCE_CONFIDENCE_INSUFFICIENT"
    assert not find(quick, "BOOTSTRAP_CAPTURE")
    quick_progress = stages(quick, "quick")
    assert {"FCAL_LOAD", "LIVE_ADC_CHECK", "LIVE_ENCODER_CHECK"} <= {
        row["stage"] for row in quick_progress if row["status"] == "COMPLETED"
    }
    assert not any(row["stage"] in measured and row["simulated_motor_us"]
                   for row in quick_progress)
    numerical = one(quick, "CANDIDATE_PREVIEW")["review_id"]
    assert numerical != reviewed
    rejected = product.run("quick-wrong-review", "quick", "--load-config", fcal,
                           "--approve-candidate", reviewed, status=2)
    assert_preview(rejected)
    assert any(row.get("error") == "REVIEW_ID_MISMATCH" for row in rejected)
    restored = product.run("quick-reviewed", "quick", "--load-config", fcal,
                           "--approve-candidate", numerical)
    assert assert_accepted(restored) == expected
    assert fcal.read_bytes() == saved
    damaged = output / "damaged.fcal"
    damaged.write_bytes(saved[:12] + bytes([saved[12] ^ 1]) + saved[13:])
    rows = product.run("quick-checksum-rejection", "quick", "--load-config", damaged, status=2)
    assert any(row.get("error") == "PACKAGE_CHECKSUM" for row in rows)
    assert_preview(rows)

    with product.selective("selective") as session:
        rows = session.command(f"load {fcal}")
        assert_preview(rows)
        assert not one(rows, "REUSE_LEGACY_FCAL")["dependency_history_imported"]
        session.command("plan same", error="HISTORY_REQUIRED")
        rows = session.command("capture")
        first = one(rows, "REUSE_PREVIEW")["review_id"]
        assert_preview(rows)
        assert_accepted(session.command(f"approve {first}"))
        session.command("plan same")
        rows = session.command("measure")
        assert not find(rows, "BOOTSTRAP_CAPTURE") and not find(rows, "REUSE_CAPTURED")
        assert any(row["status"] == "REUSED" for row in stages(rows, "selective"))
        current = one(rows, "REUSE_PREVIEW")["review_id"]
        assert_preview(rows)
        assert_accepted(session.command(f"approve {current}"))
        for change, required, retained in (
            ("encoder", {"encoder_lut", "rotor_offset", "ld_lq", "flux_kt"}, {"adc_offset", "jb"}),
            ("load", {"jb", "servo_gains"}, {"encoder_lut", "ld_lq", "flux_kt"}),
            ("adc", {"adc_offset", "ld_lq", "flux_kt"}, {"encoder_lut", "rotor_offset", "jb"}),
        ):
            plan = session.command(f"plan {change}")
            statuses = {row["parameter"]: row["status"] for row in find(plan, "REUSE_PARAMETER")}
            assert all(statuses[name] == "REMEASURE" for name in required), statuses
            assert all(statuses[name] == "REUSE" for name in retained), statuses
            rows = session.command("measure")
            stages(rows, "selective")
            candidate = one(rows, "REUSE_PREVIEW")["review_id"]
            assert candidate != current
            assert_preview(rows)
            assert_accepted(session.command(f"approve {candidate}"))
            current = candidate
        session.command("plan same")
        rows = session.command("measure")
        session.command(f"review {one(rows, 'REUSE_PREVIEW')['review_id']}")
        session.command("fault")
        assert_preview(session.command("apply", error="REVIEW_BINDING_REVOKED"))
        assert_preview(session.command("recover"))
        session.command("plan same")
        rows = session.command("measure")
        assert_accepted(session.command(f"approve {one(rows, 'REUSE_PREVIEW')['review_id']}"))
        session.command("quit")


def verify(simulator, output):
    for name in PROFILES:
        dest = output / name
        dest.mkdir()
        verify_profile(Product(simulator, name, dest), dest)
    generic = output / PROFILES[0]
    foreign = output / PROFILES[1] / "qualified.fcal"
    rows = Product(simulator, PROFILES[0], generic).run(
        "quick-identity-rejection", "quick", "--load-config", foreign, status=2
    )
    assert any(row.get("error") == "PACKAGE_IDENTITY" for row in rows)
    assert_preview(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulator", type=pathlib.Path)
    parser.add_argument("--output-dir", type=pathlib.Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir()
        verify(args.simulator.resolve(), args.output_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="falconfoc-cal-product-") as folder:
            verify(args.simulator.resolve(), pathlib.Path(folder))


if __name__ == "__main__":
    main()
