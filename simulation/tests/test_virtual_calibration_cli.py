"""Product calibration CLI acceptance and Host package rejection cases."""

import csv
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

from current_startup_quality import score

ROOT = pathlib.Path(__file__).resolve().parents[2]
CLI = ROOT / "tools/falconfoc-cli/falconfoc_cli.py"
SIM = pathlib.Path(sys.argv[1]).resolve()


def run(*args, status=0):
    result = subprocess.run(
        [sys.executable, "-B", str(CLI), "calibrate", "--simulator", str(SIM),
         *map(str, args)], capture_output=True, text=True, check=False, timeout=45,
    )
    assert result.returncode == status, (result.returncode, result.stderr, result.stdout)
    return [json.loads(line) for line in result.stdout.splitlines()]


def event(events, name):
    return next(item for item in events if item.get("event") == name)


def error(events, reason):
    assert events[-1]["error"] == reason, events
    assert not any(item.get("event") == "CONFIGURATION_APPLIED" for item in events)


def near(observed, expected, tolerance):
    assert abs(observed - expected) <= tolerance, (observed, expected, tolerance)


def verify(events, truth):
    measured = event(events, "PARAMETERS_IDENTIFIED")
    for key, value, tolerance in truth:
        near(measured[key], value, tolerance)
    assert measured["d_capture"] != measured["q_capture"]
    assert measured["d_aperture_us"][1] < measured["q_aperture_us"][0]
    assert measured["rotor_pole_pairs"] == 4
    assert measured["lut_holdout_rms_rad"] < 0.001
    applied = event(events, "CONFIGURATION_APPLIED")
    assert applied["adc"] == [2090.0, 2030.0, 2024.0]
    assert applied["lut_nodes"] == 32
    assert event(events, "CANDIDATE_PREVIEW")["damping_filter_hz"] == 48
    assert applied["damping_filter_hz"] == 48
    assert applied["pi"][0] != applied["pi"][1]
    captions = {(row["mode"], row["configuration"], row["reference"] > 0)
                for row in events if row.get("event") == "CONTROL_TRIAL"}
    assert len(captions) == 16
    assert all(row["updates"] > 0 and row["stop_confirmed"] and row["faults"] == 0
               for row in events if row.get("event") == "CONTROL_TRIAL")
    # Compare the worst full active-window RMS across both directions. The
    # weaker 36 Hz candidate looked acceptable at the end of a 60 ms window.
    current = [row for row in events if row.get("event") == "CONTROL_TRIAL"
               and row["mode"] == "current"]
    before = max(row["rms"] for row in current if row["configuration"] == "before")
    after = max(row["rms"] for row in current if row["configuration"] == "after")
    assert after < before, (before, after)
    event(events, "FAULT_RECOVERY_VERIFIED")
    event(events, "CONTROL_QUALITY_PASS")
    return applied


def check_current_cycle_study(folder, review_id):
    trace = folder / "cq-cycles.csv"
    result = subprocess.run(
        [str(SIM), "--cogging", "--approve-candidate", review_id, "--cq-current-csv", str(trace)],
        capture_output=True, text=True, check=False, timeout=45,
    )
    assert result.returncode == 0, (result.stderr, result.stdout[-1024:])
    with trace.open(newline="", encoding="ascii") as file:
        rows = list(csv.DictReader(file))
    active = [r for r in rows if r["update_applied"] == "1"]
    assert len(active) == 30 * 300
    low_bus = [r for r in active if r["scenario"] == "low_bus_reverse_exit"
               and r["group"] == "selected_130hz"]
    assert len(low_bus) == 300
    assert any(r["near_voltage_cap"] == "1" for r in low_bus)
    assert all(r["near_voltage_cap"] == "0" and
               abs(float(r["iq_plant_a"]) - float(r["iq_ref_a"])) <= 0.04
               for r in low_bus[-75:])
    assert all(r["h0_us"] == r["sample_us"] and r["faults"] == "0" for r in active)


def check_startup(folder, profile, review_id, *flags):
    trace = folder / f"startup-{profile}.csv"
    args = [str(SIM), "--profile-id", profile, *flags, "--cq-startup-csv", str(trace)]
    missing_review = subprocess.run(
        args, capture_output=True, text=True, check=False, timeout=45,
    )
    assert missing_review.returncode == 2 and not trace.exists()
    assert json.loads(missing_review.stdout.splitlines()[-1])["error"] == "EXPLICIT_REVIEW_REQUIRED"
    run("--profile-id", profile, *flags, "--approve-candidate", review_id,
        "--startup-trace", trace)
    cases = score(trace, profile)["cases"]
    failed = {name for name, case in cases.items()
              if any(p["verdict"] == "CONTROL_QUALITY_FAIL" for p in case["phases"])}
    # V1 observes the frozen Salient large-step limit as a failure, with the
    # same time and voltage gates used for Generic. All other feasible phases pass.
    expected = {"cold_large/negative", "cold_large/positive"} if "SALIENT" in profile else set()
    assert failed == expected, (failed, expected)
    phases = [phase for case in cases.values() for phase in case["phases"]]
    verdicts = [phase["verdict"] for phase in phases]
    assert verdicts.count("CONTROL_QUALITY_PASS") == 16 - len(expected), phases
    assert verdicts.count("LIMITED_BY_FEASIBILITY") == 2, phases


def reachability(package, profile, speed, target, vbus, *, torque=False):
    query = "--reachability-torque" if torque else "--reachability"
    rows = run("--profile-id", profile, "--load-config", package, query, speed, 0, target, vbus)
    assert not any(row.get("event") in ("CONFIGURATION_APPLIED", "CONTROL_TRIAL") for row in rows)
    point = event(rows, "REACHABILITY")
    assert not point["configuration_applied"] and not point["physical_applied"]
    assert point["assumption"] == "STEADY_STATE_ZERO_DQ_CURRENT_DERIVATIVES"
    return point


def main():
    with tempfile.TemporaryDirectory(prefix="falconfoc-cal-test-") as temporary:
        root = pathlib.Path(temporary)
        package = root / "motor.fcal"
        trace = root / "rl.csv"
        capabilities = event(run("--capabilities"), "HOST_CAPABILITIES")
        assert capabilities["output_locked_on_board1"] and capabilities["review_required_to_apply"]
        assert not capabilities["target_wcet_verified"] and not capabilities["mcu_flash"]
        wrong_runner = subprocess.run(
            [sys.executable, "-B", str(CLI), "calibrate", "--simulator", sys.executable,
             "--capabilities"], capture_output=True, text=True, check=False, timeout=45,
        )
        assert wrong_runner.returncode == 2
        assert json.loads(wrong_runner.stdout)["error"] == "SIMULATOR_PROTOCOL_ERROR"
        preview = run("--cogging")
        reviewed = event(preview, "CANDIDATE_PREVIEW")["review_id"]
        approval = ("--approve-candidate", reviewed)
        event(preview, "REVIEW_REQUIRED")
        assert not any(item.get("event") == "CONFIGURATION_APPLIED" for item in preview)

        error(run("--cogging", "--approve-candidate", "unknown", status=2),
              "REVIEW_ID_MISMATCH")
        first = run(*approval, "--cogging", "--save-config", package, "--trace", trace)
        check_current_cycle_study(root, reviewed)
        check_startup(root, "GENERIC_RESEARCH_MOTOR", reviewed, "--cogging")
        before = verify(first, [
            ("rs_ohm", 0.6, 0.01), ("ld_h", 0.0012, 0.00003), ("lq_h", 0.0015, 0.00003),
            ("flux_wb", 0.025, 0.0003), ("kt_nm_per_peak_q_amp", 0.15, 0.002),
            ("j_kg_m2", 0.002, 0.00004), ("b_nm_s", 0.006, 0.00008),
            ("electrical_offset_rad", 0.37, 0.002),
        ])
        assert before["cogging_enabled"]
        assert event(first, "CONFIGURATION_PERSISTED")["host"]
        assert not event(first, "CONFIGURATION_PERSISTED")["mcu_flash"]
        quality = event(first, "COGGING_QUALIFIED")
        assert quality["holdout_after_rms_nm"] < quality["holdout_before_rms_nm"]
        with trace.open(newline="", encoding="ascii") as file:
            rows = list(csv.DictReader(file))
        assert len(rows) == 1538
        for axis, capture in (("d", "6"), ("q", "7")):
            data = [row for row in rows if row["axis"] == axis]
            assert len(data) == 769 and {row["capture_id"] for row in data} == {capture}
            assert data[0]["voltage_valid"] == "0"
            assert all(row["voltage_valid"] == "1" and row["stationary"] == "1" and
                       row["output_confirmed"] == "1" and
                       row["begin_us"] == previous["aperture_us"] and
                       row["end_us"] == row["aperture_us"]
                       for previous, row in zip(data, data[1:]))

        inspection = run("--load-config", package)
        assert event(inspection, "CANDIDATE_PREVIEW")["review_id"] == reviewed
        event(inspection, "REVIEW_REQUIRED")
        error(run("--approve-candidate", "unknown", "--load-config", package, status=2),
              "REVIEW_ID_MISMATCH")
        restored = run(*approval, "--load-config", package)
        assert event(restored, "CONFIGURATION_APPLIED") == before
        assert [x for x in restored if x.get("event") == "CONTROL_TRIAL"] == [
            x for x in first if x.get("event") == "CONTROL_TRIAL"]
        error(run(*approval, "--load-config", package, "--profile-id",
                  "CALIBRATION_SALIENT_MOTOR", status=2), "PACKAGE_IDENTITY")
        point = reachability(package, "GENERIC_RESEARCH_MOTOR", 0, 0.45, 12)
        assert point["verdict"] == "MODEL_FEASIBLE" and point["review_id"] == reviewed
        near(point["required_v"], 0.27, 0.001)
        point = reachability(package, "GENERIC_RESEARCH_MOTOR", 0, 0.34, 12, torque=True)
        assert point["verdict"] == "MODEL_INFEASIBLE" and point["reasons"] == ["TORQUE_LIMIT"]
        point = reachability(package, "GENERIC_RESEARCH_MOTOR", 0, 0.15, 5.9)
        assert point["reasons"] == ["BUS_MINIMUM"] and not point["range_valid"]
        error(run("--load-config", package, "--reachability", 0, 0, "nan", 12, status=2),
              "INVALID_REACHABILITY_POINT")
        original = package.read_bytes()
        corrupted = root / "corrupt.fcal"
        for offset, replacement, reason in (
            (4, struct.pack("<I", 2), "PACKAGE_VERSION"),
            (75, b"\xff", "PACKAGE_CHECKSUM"),
            (64, struct.pack("<f", float("nan")), "PACKAGE_PARAMETER_RANGE"),
            (124, struct.pack("<f", 60.0), "PACKAGE_PARAMETER_RANGE"),
        ):
            data = bytearray(original)
            data[offset:offset + len(replacement)] = replacement
            if reason == "PACKAGE_PARAMETER_RANGE":
                struct.pack_into("<I", data, len(data) - 4, zlib.crc32(data[:-4]))
            corrupted.write_bytes(data)
            error(run(*approval, "--load-config", corrupted, status=2), reason)

        rejected_output = root / "cancelled.fcal"
        error(run(*approval, "--cancel-after", "ELECTRICAL_D",
                  "--save-config", rejected_output, status=2), "CALIBRATION_CANCELLED")
        assert not rejected_output.exists()
        assert package.read_bytes() == original
        salient = ("--profile-id", "CALIBRATION_SALIENT_MOTOR")
        next_review = event(run(*salient), "CANDIDATE_PREVIEW")["review_id"]
        salient_package = root / "salient.fcal"
        second = run(*salient, "--approve-candidate", next_review, "--save-config", salient_package)
        calibrated = verify(second, [
            ("rs_ohm", 0.78, 0.012), ("ld_h", 0.0017, 0.00004), ("lq_h", 0.0022, 0.00004),
            ("flux_wb", 0.032, 0.0004), ("kt_nm_per_peak_q_amp", 0.192, 0.0025),
            ("j_kg_m2", 0.0032, 0.00008), ("b_nm_s", 0.009, 0.00015),
            ("electrical_offset_rad", 0.52, 0.002),
        ])
        assert not calibrated["cogging_enabled"]
        check_startup(root, "CALIBRATION_SALIENT_MOTOR", next_review)
        for sign in (-1, 1):
            rest = reachability(salient_package, "CALIBRATION_SALIENT_MOTOR", 0, sign * 2.2, 12)
            assert rest["verdict"] == "MODEL_FEASIBLE"
            assert rest["iq_range_a"][0] <= sign * 2.2 <= rest["iq_range_a"][1]
            moving = reachability(
                salient_package, "CALIBRATION_SALIENT_MOTOR", sign * 6.9, sign * 2.2, 12,
            )
            assert moving["verdict"] == "MODEL_INFEASIBLE" and moving["reasons"] == ["VOLTAGE_LIMIT"]
            near(moving["required_v"], 2.602, 0.004)


if __name__ == "__main__":
    main()
