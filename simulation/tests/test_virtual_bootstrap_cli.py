"""Exercise the bootstrap acquisition, review, application, and stop boundaries."""

import csv
import json
import math
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[2]
CLI = ROOT / "tools/falconfoc-cli/falconfoc_cli.py"
SIMULATOR = pathlib.Path(sys.argv[1]).resolve()
MOTORS = (
    ("GENERIC_RESEARCH_MOTOR", 201, 101, 0, 1, 0.37, 0.025),
    ("CALIBRATION_SALIENT_MOTOR", 202, 102, 1, -1, 0.52, 0.032),
)


def run(profile, *args, status=0, commands=None):
    result = subprocess.run(
        [sys.executable, "-B", str(CLI), "calibrate", "--simulator", str(SIMULATOR),
         "--profile-id", profile, *map(str, args)],
        input=commands, capture_output=True, text=True, check=False, timeout=45,
    )
    assert result.returncode == status, (args, result.returncode, result.stderr, result.stdout)
    return [json.loads(line) for line in result.stdout.splitlines()]


def events(rows, name):
    return [row for row in rows if row.get("event") == name]


def event(rows, name):
    matches = events(rows, name)
    assert len(matches) == 1, (name, rows)
    return matches[0]


def assert_not_applied(rows):
    assert not events(rows, "CONFIGURATION_APPLIED"), rows
    assert not events(rows, "CONTROL_TRIAL"), rows
    assert not events(rows, "CONFIGURATION_PERSISTED"), rows


def check_capture(path, captured, coarse):
    with path.open(newline="", encoding="ascii") as stream:
        samples = list(csv.DictReader(stream))
    assert len(samples) == captured["samples"] == 256
    assert captured["accepted_updates"] == 0 and captured["output_inhibited"]
    assert [int(row["sequence"]) for row in samples] == list(range(1, 257))
    times = [int(row["aperture_us"]) for row in samples]
    assert all(a < b for a, b in zip(times, times[1:])) and times[-1] < captured["completed_us"]
    for field in ("motor", "encoder", "frame", "phase_order", "capture"):
        assert {int(row[field]) for row in samples} == {coarse[field]}
    assert all(row["valid"] == row["output_inhibited"] == "1" for row in samples)

    # Check net phase advance from the voltages and encoder independently of
    # the fit. Each settled sweep must exhibit the same signed winding ratio.
    for sweep, sign in enumerate((1, -1, 1, -1)):
        window = [row for row in samples if int(row["sweep"]) == sweep]
        assert len(window) == 64
        assert all(1 < sign * float(row["shaft_rad_s"]) < 10 for row in window)
        angles = []
        counts = []
        for row in window:
            alpha, beta = float(row["emf_alpha_v"]), float(row["emf_beta_v"])
            assert 0.04 < math.hypot(alpha, beta)
            angles.append(math.atan2(-sign * alpha, sign * beta))
            counts.append(int(row["raw_encoder"]))
        assert all(0 <= count < 65536 for count in counts)
        electrical = sum(math.remainder(b - a, math.tau) for a, b in zip(angles, angles[1:]))
        mechanical = sum(math.remainder(b - a, 65536) for a, b in zip(counts, counts[1:]))
        mechanical *= math.tau / 65536
        assert sign * electrical > math.tau
        assert math.isclose(electrical / mechanical, 4 * coarse["encoder_direction"], abs_tol=0.08)


def verify_trials(rows):
    applied = event(rows, "CONFIGURATION_APPLIED")
    assert applied["pole_pairs"] == 4 and applied["damping_filter_hz"] == 48
    assert applied["lut_nodes"] == 32
    trials = events(rows, "CONTROL_TRIAL")
    assert len(trials) == 16
    assert {(row["mode"], row["configuration"], row["reference"] > 0) for row in trials} == {
        (mode, configuration, positive)
        for mode in ("current", "velocity", "position", "impedance")
        for configuration in ("before", "after") for positive in (False, True)
    }
    assert all(row["status"] == 0 and row["updates"] > 0 and row["stop_confirmed"] and
               row["faults"] == 0 for row in trials)
    event(rows, "FAULT_RECOVERY_VERIFIED")
    event(rows, "CONTROL_QUALITY_PASS")
    return applied, trials


def verify_modes(motor, folder):
    profile, motor_id, encoder_id, phase_order, direction, offset, flux_wb = motor
    inspect = run(profile, "--bootstrap", "inspect")
    plan = event(inspect, "BOOTSTRAP_PLAN")
    assert plan["status"] == "REFERENCE_ACQUISITION_REQUIRED"
    assert not events(inspect, "BOOTSTRAP_CAPTURE")
    assert_not_applied(inspect)

    for mode in ("offset", "unknown"):
        trace = folder / f"{profile}-{mode}.csv"
        package = folder / f"{profile}-{mode}.fcal"
        preview = run(profile, "--bootstrap", mode, "--boot-trace", trace)
        plan = event(preview, "BOOTSTRAP_PLAN")
        assert plan["status"] == "REFERENCE_ACQUISITION_REQUIRED"
        assert plan["mechanical_scan_required"] and not plan["electrical_drive"]
        known = (plan["known_pole_pairs"], plan["known_direction"], plan["offset_known"])
        assert known == ((4, direction, False) if mode == "offset" else (0, 0, False))
        coarse = event(preview, "BOOTSTRAP_COARSE_REFERENCE")
        assert coarse["quality"] == "COARSE_ELECTRICAL_REFERENCE"
        assert coarse["status"] == "REFERENCE_REVIEW_REQUIRED"
        assert coarse["source"] == "HOST_OPEN_CIRCUIT_EMF" and not coarse["runtime_applicable"]
        assert (coarse["motor"], coarse["encoder"], coarse["phase_order"]) == (
            motor_id, encoder_id, phase_order,
        )
        assert (coarse["pole_pairs"], coarse["encoder_direction"]) == (4, direction)
        assert abs(math.remainder(coarse["offset_rad"] - offset, math.tau)) < 0.10
        assert coarse["candidate_separation_rad"] >= 0.08 and coarse["span_rad"] >= 0.7
        assert coarse["max_residual_rad"] <= 0.085 and coarse["offset_disagreement_rad"] <= 0.10
        check_capture(trace, event(preview, "BOOTSTRAP_CAPTURE"), coarse)

        final = event(preview, "BOOTSTRAP_FINAL_REFERENCE")
        assert final["quality"] == "QUALIFIED_OFFSET_CANDIDATE"
        assert (final["pole_pairs"], final["encoder_direction"], final["relative_direction"]) == (
            4, direction, 1,
        )
        assert abs(math.remainder(final["offset_rad"] - offset, math.tau)) < 0.002
        assert final["offset_rms_rad"] < 0.001 and final["lut_holdout_rms_rad"] < 0.001
        flux = event(preview, "FLUX_CANDIDATE")
        assert (flux["motor"], flux["phase_order"], flux["dq_frame"]) == (motor_id, phase_order, 402)
        assert flux["flux_origin"] == "FLUX_FROM_MEASUREMENT"
        assert flux["kt_origin"] == "KT_DERIVED_FROM_FLUX"
        assert flux["windows"] == [8, 8] and flux["intervals"] == 512
        assert math.isclose(flux["flux_wb"], flux_wb, abs_tol=0.0004)
        assert math.isclose(flux["kt_nm_per_peak_q_amp"], 1.5 * 4 * flux["flux_wb"], abs_tol=1e-7)
        review = event(preview, "CANDIDATE_PREVIEW")["review_id"]
        numerical = final["fcal_review_id"]
        assert review == final["review_id"] and review != numerical
        event(preview, "REVIEW_REQUIRED")
        assert_not_applied(preview)

        wrong = run(profile, "--bootstrap", mode, "--approve-candidate", numerical,
                    "--save-config", package, status=2)
        assert wrong[-1]["error"] == "REVIEW_ID_MISMATCH"
        assert_not_applied(wrong)
        assert not package.exists()
        approved = run(profile, "--bootstrap", mode, "--approve-candidate", review,
                       "--save-config", package)
        assert event(approved, "CANDIDATE_REVIEWED")["review_id"] == review
        applied, trials = verify_trials(approved)
        contents = package.read_bytes()
        assert len(contents) == 548 and struct.unpack_from("<4sI", contents) == (b"FCAL", 1)
        assert struct.unpack_from("<I", contents, len(contents) - 4)[0] == zlib.crc32(contents[:-4])

        # The numerical package continues to use the existing FCAL review path.
        known_preview = run(profile, "--bootstrap", "known", "--load-config", package)
        assert event(known_preview, "CANDIDATE_PREVIEW")["review_id"] == numerical
        trusted = event(known_preview, "BOOTSTRAP_TRUSTED_REFERENCE")
        assert trusted["status"] == "REFERENCE_REVIEW_REQUIRED"
        assert not trusted["mechanical_scan_required"] and not trusted["electrical_drive"]
        assert not events(known_preview, "BOOTSTRAP_CAPTURE")
        assert_not_applied(known_preview)
        restored = run(profile, "--bootstrap", "known", "--load-config", package,
                       "--approve-candidate", numerical)
        assert event(restored, "BOOTSTRAP_TRUSTED_REFERENCE")["status"] == "READY_WITH_TRUSTED_REFERENCE"
        assert not events(restored, "BOOTSTRAP_CAPTURE")
        assert verify_trials(restored) == (applied, trials)
        assert package.read_bytes() == contents


def verify_rejections(profile, folder):
    scenarios = {
        "load": "measurement_limit", "partial": "shaft_following", "stalled": "shaft_stalled",
        "encoder": "encoder_transport", "current-limit": "measurement_limit", "weak-emf": "weak_emf",
        "phase": "IDENTITY_MISMATCH", "direction": "REFERENCE_UNIDENTIFIABLE",
        "stop": "STOP", "fault": "FAULT", "timeout": "TIMEOUT", "cancel": "SESSION_CANCELLED",
        "identity-change": "IDENTITY_MISMATCH", "output-reject": "INHIBIT_CONFIRMATION_LOST",
    }
    interrupts = {"stop", "fault", "timeout", "cancel", "identity-change", "output-reject"}
    for scenario, reason in scenarios.items():
        path = folder / f"{profile}-{scenario}.csv"
        mode = "offset" if scenario == "direction" else "unknown"
        rows = run(profile, "--bootstrap", mode, "--boot-scenario", scenario, "--boot-trace", path,
                   status=2)
        assert rows[-1]["error"] == reason, (scenario, rows)
        assert_not_applied(rows)
        for forbidden in ("BOOTSTRAP_COARSE_REFERENCE", "BOOTSTRAP_FINAL_REFERENCE",
                          "CANDIDATE_PREVIEW", "CALIBRATION_STARTED", "FLUX_CANDIDATE"):
            assert not events(rows, forbidden), (scenario, rows)
        if scenario == "phase":
            assert not path.exists() and not events(rows, "BOOTSTRAP_CAPTURE")
            continue
        captured = event(rows, "BOOTSTRAP_CAPTURE")
        assert captured["accepted_updates"] == 0 and captured["output_inhibited"], (scenario, rows)
        with path.open(newline="", encoding="ascii") as stream:
            samples = list(csv.DictReader(stream))
        assert len(samples) == captured["samples"]
        assert all(row["output_inhibited"] == "1" for row in samples)
        if scenario in interrupts:
            assert 0 < len(samples) < 256 and int(samples[-1]["aperture_us"]) < 400000


def verify_history(profile):
    arguments = ("--bootstrap", "unknown", "--reuse-session")
    preview = run(profile, *arguments, commands="capture\nquit\n")
    review = event(preview, "REUSE_PREVIEW")["review_id"]
    assert_not_applied(preview)
    rows = run(profile, *arguments, commands=f"capture\napprove {review}\nplan same\nmeasure\nquit\n")
    assert [row["command"] for row in events(rows, "REUSE_COMMAND_RESULT")] == [
        "capture", "approve", "plan", "measure", "quit",
    ]
    assert all(row["ok"] for row in events(rows, "REUSE_COMMAND_RESULT")), rows
    receipt = event(rows, "REUSE_APPLIED")
    assert receipt["fresh_runtime"] and not receipt["physical_applied"]
    verify_trials(rows)
    event(rows, "BOOTSTRAP_CAPTURE")  # Initial acquisition only.
    plan = event(rows, "REUSE_PLAN")
    first, second = events(rows, "REUSE_PREVIEW")
    assert first["retained_mask"] == 0
    assert second["retained_mask"] == plan["retained_mask"] != 0
    retained = {row["parameter"]: row for row in events(rows, "REUSE_PARAMETER")}
    for parameter in ("encoder_lut", "pole_pairs", "rotor_offset", "rs", "ld_lq", "flux_kt", "jb"):
        row = retained[parameter]
        assert row["status"] == "REUSE" and row["source"] == "HOST_VIRTUAL", row
        assert row["session"] == 1 and row["capture"] > 0 and row["revision"] > 0


def verify_source_boundary():
    # The portable fitter and scan can consume instrument readings. They must
    # never open the simulator's broad oracle observation or direct model state.
    truth = (r"(?:\.|->)\s*(?:params|state)\b|\belectrical_position(?:_rad)?\b|"
             r"\bfoc_virtual_motor_observe\s*\(")
    for name in ("calibration/src/foc_bootstrap.c", "simulation/src/foc_virtual_bootstrap_capture.c"):
        source = (ROOT / name).read_text(encoding="utf-8")
        assert not re.search(truth, source), name
    capture = (ROOT / "simulation/src/foc_virtual_bootstrap_capture.c").read_text(encoding="utf-8")
    assert not re.search(r"\bfoc_virtual_motor_(?:set_fixture_speed|step|apply_duty)\s*\(", capture)
    assert "foc_virtual_motor_measure_open_circuit(" in capture
    calibration = (ROOT / "simulation/src/foc_virtual_calibration.c").read_text(encoding="utf-8")
    qualified = calibration.split("static bool qualified_phase(", 1)[1].split("\n}\n", 1)[0]
    assert not re.search(truth, qualified)
    for api in ("foc_encoder_correct_code", "foc_rotor_phase_from_counts", "foc_virtual_motor_measure_shaft"):
        assert api in qualified, api
    assert "return poll(session, options, evidence, true, NULL);" in calibration


if __name__ == "__main__":
    verify_source_boundary()
    with tempfile.TemporaryDirectory(prefix="falconfoc-bootstrap-test-") as directory:
        folder = pathlib.Path(directory)
        for fixture in MOTORS:
            verify_modes(fixture, folder)
            verify_rejections(fixture[0], folder)
            verify_history(fixture[0])
