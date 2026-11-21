"""Host calibration reuse acceptance through the product JSONL interface."""

import json
import pathlib
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[2]
CLI = ROOT / "tools/falconfoc-cli/falconfoc_cli.py"
SIMULATOR = pathlib.Path(sys.argv[1]).resolve()
GENERIC = "GENERIC_RESEARCH_MOTOR"
SALIENT = "CALIBRATION_SALIENT_MOTOR"
MODES = {"current", "velocity", "position", "impedance"}


def events(rows, name):
    return [row for row in rows if row.get("event") == name]


def event(rows, name):
    matches = events(rows, name)
    assert len(matches) == 1, (name, rows)
    return matches[0]


def assert_preview_only(rows):
    assert not events(rows, "CONFIGURATION_APPLIED"), rows
    assert not events(rows, "REUSE_APPLIED"), rows
    assert not events(rows, "CONTROL_TRIAL"), rows


def assert_rejected(rows, reason):
    assert_preview_only(rows)
    errors = [row for row in rows if "error" in row]
    assert len(errors) == 1 and errors[0]["error"] == reason, rows


def calibration_cli(*args):
    return [sys.executable, "-B", str(CLI), "calibrate", "--simulator", str(SIMULATOR),
            *args]


def run_calibration(*args, status=0):
    completed = subprocess.run(calibration_cli(*args), capture_output=True, text=True,
                               check=False, timeout=45)
    assert completed.returncode == status, (completed.returncode, completed.stdout, completed.stderr)
    return [json.loads(line) for line in completed.stdout.splitlines()]


class ReuseSession:
    def __init__(self, profile, *, product_cli=False, cogging=False):
        self.profile = profile
        self.product_cli = product_cli
        self.cogging = cogging

    def __enter__(self):
        self.stderr = tempfile.TemporaryFile(mode="w+t", encoding="utf-8")
        command = [str(SIMULATOR)]
        if self.product_cli:
            command = calibration_cli()
        command.extend(("--profile-id", self.profile, "--reuse-session"))
        if self.cogging:
            command.append("--cogging")
        self.child = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.stderr, text=True, encoding="utf-8",
        )
        self.stdout = queue.Queue()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        return self

    def read(self):
        for line in self.child.stdout:
            self.stdout.put(line)
        self.stdout.put(None)

    def __exit__(self, exception_type, exception, traceback):
        try:
            self.child.stdin.close()
        except BrokenPipeError:
            pass
        try:
            exit_status = self.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.child.kill()
            exit_status = self.child.wait(timeout=5)
        self.reader.join(timeout=5)
        self.child.stdout.close()
        self.stderr.seek(0)
        errors = self.stderr.read()
        self.stderr.close()
        if exception is None:
            assert exit_status == 0 and not errors, (exit_status, errors)

    def command(self, line, *, ok=True):
        try:
            self.child.stdin.write(line + "\n")
            self.child.stdin.flush()
        except BrokenPipeError:
            pass
        operation = line.split(maxsplit=1)[0]
        deadline = time.monotonic() + 45
        rows = []
        while True:
            try:
                output = self.stdout.get(timeout=max(deadline - time.monotonic(), 0))
            except queue.Empty as error:
                raise AssertionError((line, "command timeout", rows)) from error
            if output is None:
                self.stderr.seek(0)
                raise AssertionError((line, "reuse session ended", rows, self.stderr.read()))
            row = json.loads(output)
            rows.append(row)
            if row.get("event") == "REUSE_COMMAND_RESULT":
                assert row["command"] == operation and row["ok"] is ok, (line, rows)
                return rows


def review(rows):
    assert_preview_only(rows)
    review_id = event(rows, "REUSE_PREVIEW")["review_id"]
    assert len(review_id) == 8 and int(review_id, 16) >= 0, rows
    return review_id


def assert_closed_loop(rows, *, reuse=True):
    if reuse:
        receipt = event(rows, "REUSE_APPLIED")
        assert receipt["fresh_runtime"] is True and receipt["physical_applied"] is False, receipt
    configuration = event(rows, "CONFIGURATION_APPLIED")
    trials = events(rows, "CONTROL_TRIAL")
    assert len(trials) == 16, trials
    assert {(row["mode"], row["configuration"], row["reference"] > 0) for row in trials} == {
        (mode, calibration, sign) for mode in MODES
        for calibration in ("before", "after") for sign in (False, True)
    }, trials
    for trial in trials:
        assert trial["status"] == 0 and trial["active_samples"] > 0, trial
        assert trial["updates"] > 0 and trial["faults"] == 0 and trial["stop_confirmed"], trial
    recovery = event(rows, "FAULT_RECOVERY_VERIFIED")
    assert recovery["accepted_updates_after_rearm"] > recovery["accepted_updates_before_clear"]
    assert recovery["stop_confirmed"]
    event(rows, "CONTROL_QUALITY_PASS")
    return configuration, trials


def complete_capture(session):
    preview = session.command("capture")
    review_id = review(preview)
    return assert_closed_loop(session.command(f"approve {review_id}"))


def planned_parameters(rows):
    event(rows, "REUSE_PLAN")
    assert_preview_only(rows)
    details = events(rows, "REUSE_PARAMETER")
    assert len(details) == 13, rows
    result = {row["parameter"].upper(): row["status"] for row in details}
    assert len(result) == len(details), rows
    assert set(result.values()) <= {"REUSE", "REMEASURE", "NOT_CONFIGURED"}, rows
    assert all(row["reason"] for row in details), rows
    assert all(row["source"] and row["session"] > 0 and row["capture"] > 0 and row["revision"] > 0
               for row in details if row["status"] == "REUSE"), rows
    return result


def origins(rows):
    return {row["parameter"]: (row["source"], row["session"], row["capture"], row["revision"])
            for row in events(rows, "REUSE_PARAMETER") if row["status"] != "NOT_CONFIGURED"}


def test_parameter_review_is_not_configuration_admission():
    for selection, expected, tolerances in (
        ("rs", (0.6, 0.0), (0.01, 0.0)),
        ("jb", (0.002, 0.006), (0.00004, 0.00008)),
    ):
        preview_rows = run_calibration("--stages", selection)
        assert_preview_only(preview_rows)
        preview = event(preview_rows, "PARAMETER_PREVIEW")
        assert preview["parameter"] == selection.upper(), preview
        assert preview["source"] == "HOST_VIRTUAL" and preview["capture"] > 0, preview
        assert preview["runtime_applicable"] is False, preview
        assert len(preview["values"]) == len(expected), preview
        for observed, nominal, tolerance in zip(preview["values"], expected, tolerances):
            assert abs(observed - nominal) <= tolerance, preview
        denied = run_calibration("--stages", selection, "--approve-candidate", "invalid", status=2)
        assert_rejected(denied, "REVIEW_ID_MISMATCH")
        reviewed = run_calibration("--stages", selection, "--approve-candidate", preview["review_id"])
        assert_preview_only(reviewed)
        assert event(reviewed, "PARAMETER_REVIEWED")["runtime_applicable"] is False, reviewed


def test_full_reuse_matches_new_calibration(profile):
    with ReuseSession(profile, product_cli=profile == GENERIC) as session:
        initial = session.command("capture")
        measured = event(initial, "FLUX_CANDIDATE")
        fresh = assert_closed_loop(session.command(f"approve {review(initial)}"))
        assert measured["flux_origin"] == "FLUX_FROM_MEASUREMENT"
        assert measured["kt_origin"] == "KT_DERIVED_FROM_FLUX"
        plan_rows = session.command("plan same")
        plan = planned_parameters(plan_rows)
        assert "REMEASURE" not in plan.values(), plan
        preview = session.command("measure")
        assert not events(preview, "REUSE_CAPTURED"), preview
        assert event(preview, "FLUX_CANDIDATE") == measured
        review_id = review(preview)
        assert_rejected(session.command("apply", ok=False), "REVIEW_REQUIRED")
        reviewed = session.command(f"review {review_id}")
        assert_preview_only(reviewed)
        assert event(reviewed, "REUSE_REVIEWED")["review_id"] == review_id
        reused = assert_closed_loop(session.command("apply"))
        assert reused == fresh, (fresh, reused)
        assert origins(session.command("plan same")) == origins(plan_rows)

        rejected = session.command("measure")
        assert_preview_only(session.command(f"reject {review(rejected)}"))
        assert_rejected(session.command("apply", ok=False), "REVIEW_BINDING_REVOKED")
        assert origins(session.command("plan same")) == origins(plan_rows)
        preview = session.command("measure")
        session.command(f"review {review(preview)}")
        session.command("invalidate ld_lq")
        assert_rejected(session.command("apply", ok=False), "REVIEW_BINDING_REVOKED")
        states = planned_parameters(session.command("plan same"))
        assert states["FLUX_KT"] == "REMEASURE"
        session.command("quit")


def test_dependency_plans():
    scenarios = {
        "encoder": (
            {"ADC_OFFSET", "RS", "LD_LQ", "POLE_PAIRS"},
            {"ENCODER_LUT", "ROTOR_OFFSET"},
        ),
        "load": (
            {"ADC_OFFSET", "RS", "LD_LQ", "ENCODER_LUT", "ROTOR_OFFSET", "CURRENT_PI"},
            {"JB", "SERVO_GAINS", "MECHANICAL_LIMITS"},
        ),
        "motor": (
            {"ADC_OFFSET"},
            {"RS", "LD_LQ", "POLE_PAIRS", "ROTOR_OFFSET", "FLUX_KT", "CURRENT_PI", "SERVO_GAINS"},
        ),
        "adc": (
            {"ENCODER_LUT", "POLE_PAIRS", "ROTOR_OFFSET"},
            {"ADC_OFFSET", "RS", "LD_LQ", "FLUX_KT", "CURRENT_PI"},
        ),
        "voltage": ({"ADC_OFFSET", "ENCODER_LUT"}, {"RS", "LD_LQ", "FLUX_KT", "CURRENT_PI"}),
        "phase": ({"POLE_PAIRS"}, {"ROTOR_OFFSET"}),
        "home": ({"RS", "POLE_PAIRS"}, {"MECHANICAL_HOME", "MECHANICAL_LIMITS"}),
        "offset": ({"RS", "ENCODER_LUT", "POLE_PAIRS"}, {"ROTOR_OFFSET"}),
        "lut": ({"RS", "LD_LQ", "POLE_PAIRS"}, {"ENCODER_LUT", "ROTOR_OFFSET"}),
        "kt": ({"RS", "LD_LQ", "JB", "CURRENT_PI"}, {"FLUX_KT", "SERVO_GAINS"}),
        "angle": ({"RS"}, {"LD_LQ", "FLUX_KT"}),
        "limits": ({"RS", "LD_LQ", "JB"}, {"MECHANICAL_LIMITS", "SERVO_GAINS"}),
        "torque": ({"RS", "LD_LQ", "FLUX_KT"}, {"JB", "SERVO_GAINS"}),
        "temperature": ({"ENCODER_LUT", "POLE_PAIRS"}, {"RS", "LD_LQ", "FLUX_KT"}),
    }
    with ReuseSession(GENERIC) as session:
        complete_capture(session)
        for scenario, (retained, invalidated) in scenarios.items():
            plan = planned_parameters(session.command(f"plan {scenario}"))
            assert retained <= {name for name, status in plan.items() if status == "REUSE"}, (
                scenario, plan,
            )
            assert invalidated <= {name for name, status in plan.items() if status == "REMEASURE"}, (
                scenario, plan,
            )
            if scenario in ("voltage", "angle", "torque", "temperature"):
                assert_rejected(session.command("measure", ok=False), "NEW_REFERENCE_REQUIRED")


def test_inhibit_revokes_approval():
    with ReuseSession(GENERIC) as session, tempfile.TemporaryDirectory(
        prefix="falconfoc-reuse-revoked-"
    ) as temporary:
        assert_rejected(session.command("plan same", ok=False), "HISTORY_REQUIRED")
        assert_rejected(session.command("approve 00000000", ok=False), "REVIEW_REQUIRED")
        original = complete_capture(session)
        for interruption in ("stop", "fault", "invalidate encoder_lut"):
            session.command("plan same")
            review_id = review(session.command("measure"))
            assert_preview_only(session.command(f"review {review_id}"))
            interrupted = session.command(interruption)
            assert_preview_only(interrupted)
            assert_rejected(session.command("apply", ok=False), "REVIEW_BINDING_REVOKED")
            if interruption == "fault":
                assert event(interrupted, "REUSE_ORIGINAL_INHIBITED")["faults"] != 0
                assert_rejected(session.command("measure", ok=False), "ORIGINAL_RUNTIME_UNSAFE")
                recovered = session.command("recover")
                assert_preview_only(recovered)
                assert event(recovered, "REUSE_ORIGINAL_INHIBITED")["faults"] == 0
            if interruption == "stop":
                assert event(interrupted, "REUSE_ORIGINAL_INHIBITED")["faults"] == 0
        path = pathlib.Path(temporary) / "revoked.fcal"
        assert_rejected(session.command(f"save {path}", ok=False), "REVIEW_REQUIRED")
        assert not path.exists()
        session.command("plan same")
        preview = session.command("measure")
        assert "ENCODER_LUT" in {row["stage"] for row in events(preview, "REUSE_CAPTURED")}
        review_id = review(preview)
        applied = assert_closed_loop(session.command(f"approve {review_id}"))
        assert applied == original


def test_selected_remeasurement():
    with ReuseSession(GENERIC) as session, tempfile.TemporaryDirectory(
        prefix="falconfoc-reuse-selective-"
    ) as temporary:
        baseline, _ = complete_capture(session)
        before = origins(session.command("plan encoder"))
        preview = session.command("measure")
        captured = {row["stage"] for row in events(preview, "REUSE_CAPTURED")}
        assert {"ENCODER_LUT", "ELECTRICAL_OFFSET"} <= captured, preview
        assert not captured & {"ADC_ZERO", "ELECTRICAL_D", "ELECTRICAL_Q", "MECHANICAL"}, preview
        approved = review(preview)
        wrong = "00000000" if approved != "00000000" else "ffffffff"
        assert_rejected(session.command(f"approve {wrong}", ok=False), "REVIEW_ID_MISMATCH")
        encoder_result = assert_closed_loop(session.command(f"approve {approved}"))
        encoder, _ = encoder_result
        assert encoder["adc"] == baseline["adc"] and encoder["pi"] == baseline["pi"], encoder
        after = origins(session.command("plan same"))
        for parameter in ("rs", "ld_lq", "current_pi", "jb"):
            assert after[parameter] == before[parameter], (before, after)
        for parameter in ("encoder_lut", "rotor_offset"):
            assert after[parameter][1] > before[parameter][1], (before, after)
        path = pathlib.Path(temporary) / "encoder.fcal"
        saved = event(session.command(f"save {path}"), "REUSE_NUMERICAL_SAVED")
        restored = session.command(f"load {path} {saved['review_id']}")
        assert assert_closed_loop(restored, reuse=False) == encoder_result
        assert event(restored, "REUSE_LEGACY_FCAL")["dependency_history_imported"] is False

        session.command("plan load")
        preview = session.command("measure")
        captured = {row["stage"] for row in events(preview, "REUSE_CAPTURED")}
        assert "MECHANICAL" in captured, preview
        assert not captured & {"ADC_ZERO", "ENCODER_LUT", "ELECTRICAL_D", "ELECTRICAL_Q"}, preview
        loaded, _ = assert_closed_loop(session.command(f"approve {review(preview)}"))
        assert loaded["pi"] == encoder["pi"], loaded
        assert loaded["electrical_offset_rad"] == encoder["electrical_offset_rad"], loaded
        assert loaded["outer"] != encoder["outer"], loaded

        session.command("plan motor")
        preview = session.command("measure")
        captured = {row["stage"] for row in events(preview, "REUSE_CAPTURED")}
        assert {"ELECTRICAL_D", "ELECTRICAL_Q", "FLUX", "ELECTRICAL_OFFSET"} <= captured, preview
        assert_rejected(session.command(f"approve {approved}", ok=False), "REVIEW_ID_MISMATCH")
        motor, _ = assert_closed_loop(session.command(f"approve {review(preview)}"))
        assert motor["pi"] != loaded["pi"] and motor["kt"] != loaded["kt"], motor


def test_sampling_chain_and_kt_changes():
    with ReuseSession(GENERIC) as session:
        baseline, _ = complete_capture(session)
        before = origins(session.command("plan adc"))
        adc_preview = session.command("measure")
        captured = {row["stage"] for row in events(adc_preview, "REUSE_CAPTURED")}
        assert {"ADC_ZERO", "ELECTRICAL_D", "ELECTRICAL_Q", "FLUX", "TUNING"} <= captured, (
            adc_preview
        )
        assert not captured & {"ENCODER_LUT", "ROTOR", "ELECTRICAL_OFFSET", "MECHANICAL"}, (
            adc_preview
        )
        adc, _ = assert_closed_loop(session.command(f"approve {review(adc_preview)}"))
        assert adc["adc"] == [2101.0, 2030.0, 2024.0] and adc["adc"] != baseline["adc"], adc
        assert adc["electrical_offset_rad"] == baseline["electrical_offset_rad"], adc
        assert adc["pole_pairs"] == baseline["pole_pairs"], adc
        kt_plan = session.command("plan kt")
        after = origins(kt_plan)
        for parameter in ("encoder_lut", "rotor_offset", "jb"):
            assert after[parameter] == before[parameter], (before, after)

        kt_preview = session.command("measure")
        fit = event(kt_preview, "FLUX_CANDIDATE")
        assert fit["session"] > after["flux_kt"][1] and fit["rl_revision"] == after["ld_lq"][3]
        assert fit["poles_revision"] == after["pole_pairs"][3]
        assert fit["current_revision"] == after["adc_offset"][3]
        captured = {row["stage"] for row in events(kt_preview, "REUSE_CAPTURED")}
        assert {"FLUX", "TUNING"} <= captured and "MECHANICAL" not in captured, kt_preview
        assert_closed_loop(session.command(f"approve {review(kt_preview)}"))
        kt_after = origins(session.command("plan same"))
        assert kt_after["jb"] == after["jb"], (after, kt_after)
        for parameter in ("flux_kt", "servo_gains"):
            assert kt_after[parameter][1] > after[parameter][1], (after, kt_after)


def test_package_round_trip():
    with tempfile.TemporaryDirectory(prefix="falconfoc-reuse-fcal-") as temporary:
        path = pathlib.Path(temporary) / "reuse config.fcal"
        with ReuseSession(GENERIC, cogging=True) as session:
            assert_rejected(session.command(f"save {path}", ok=False), "REVIEW_REQUIRED")
            assert not path.exists()
            first = complete_capture(session)
            plan = planned_parameters(session.command("plan same"))
            assert plan["COGGING_LUT"] == "REUSE"
            review_id = review(session.command("measure"))
            applied = assert_closed_loop(session.command(f"approve {review_id}"))
            assert applied == first
            saved = session.command(f"save {path}")
            assert_preview_only(saved)
            numerical = event(saved, "REUSE_NUMERICAL_SAVED")
            assert numerical["dependency_metadata_persisted"] is False
            session.command("plan limits")
            review_id = review(session.command("measure"))
            assert_closed_loop(session.command(f"approve {review_id}"))
            unsupported = pathlib.Path(temporary) / "unsupported-policy.fcal"
            assert_rejected(session.command(f"save {unsupported}", ok=False),
                            "FCAL_POLICY_NOT_REPRESENTED")
            assert not unsupported.exists()
        package = path.read_bytes()
        assert len(package) == 548 and package[:8] == b"FCAL\x01\x00\x00\x00", package[:8]
        review_id, = struct.unpack("<I", package[-4:])
        assert review_id == zlib.crc32(package[:-4])
        assert numerical["review_id"] == f"{review_id:08x}"
        with ReuseSession(GENERIC) as session:
            inspection = session.command(f"load {path}")
            assert_preview_only(inspection)
            assert event(inspection, "CANDIDATE_PREVIEW")["review_id"] == f"{review_id:08x}"
            loaded = session.command(f"load {path} {review_id:08x}")
            assert assert_closed_loop(loaded, reuse=False) == applied
            assert event(loaded, "REUSE_LEGACY_FCAL")["dependency_history_imported"] is False
            assert_rejected(session.command("plan same", ok=False), "HISTORY_REQUIRED")
        with ReuseSession(GENERIC) as session:
            baseline = complete_capture(session)
            session.command("plan same")
            composite_review = review(session.command("measure"))
            assert_preview_only(session.command(f"review {composite_review}"))
            assert_closed_loop(session.command(f"load {path} {review_id:08x}"), reuse=False)
            assert assert_closed_loop(session.command("apply")) == baseline
        with ReuseSession(SALIENT) as session:
            assert_rejected(session.command(f"load {path} {review_id:08x}", ok=False),
                            "PACKAGE_IDENTITY")
        assert path.read_bytes() == package


def main():
    test_parameter_review_is_not_configuration_admission()
    for profile in (GENERIC, SALIENT):
        test_full_reuse_matches_new_calibration(profile)
    test_dependency_plans()
    test_selected_remeasurement()
    test_sampling_chain_and_kt_changes()
    test_inhibit_revokes_approval()
    test_package_round_trip()


if __name__ == "__main__":
    main()
