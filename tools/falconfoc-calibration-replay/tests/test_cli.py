"""Exercise the read-only pack boundary and, with --runner, the native workflow."""

import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


TOOL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL))
sys.dont_write_bytecode = True
import replay


RUNNER = None
if "--runner" in sys.argv:
    index = sys.argv.index("--runner")
    RUNNER = Path(sys.argv[index + 1]).resolve()
    del sys.argv[index:index + 2]

HEADER = (
    b"stage,capture,sequence,source_us,completion_us,trigger_us,reference_us,"
    b"clock_id,reference_clock_id,unit,channel,flags,evidence,p1,p2,p3,p4,p5,p6"
)
ROW = b"0,10,0,100,103,99,,1,,1,0,17,1,2043,2052,2048,,,"
SAMPLE = HEADER + b"\r\n" + ROW + b"\r\n"
NATIVE = {
    "trace_valid": True,
    "calibration_state": "COMPLETED",
    "candidate_available": True,
    "review_required": True,
    "stages": {},
    "rejected_samples": 0,
    "reason": "NONE",
}


def make_manifest(raw):
    return {
        "format_version": 1,
        "source": "HOST_SYNTHETIC",
        "host_synthetic": True,
        "session": 9,
        "recorder": {"name": "test recorder", "version": "1"},
        "identity": {"device_id": 1, "motor_id": 2, "encoder_id": 3, "frame_id": 4},
        "profile": "SELECTED_STAGES",
        "stage_mask": 1,
        "stage_rows": [1, 0, 0, 0, 0, 0, 0, 0],
        "time": {"unit": "us", "clock_id": 1, "max_age_us": 20, "max_skew_us": 5,
                 "allow_wrap": False, "alignment": "SAME_CLOCK_PAIR"},
        "adc": {"max_count": 4095, "amp_per_count": 0.01, "channel_map": [0, 1, 2]},
        "encoder": {"cpr": 4096, "raw_direction": 1, "phase_order": 0, "pole_pairs": 4},
        "reference": {"id": 5, "zero_rad": 0.0, "frame_direction": 1},
        "torque_model_id": 6,
        "units": {"adc": "count", "current": "A", "voltage": "V", "angle": "rad",
                  "encoder": "count", "speed": "rad/s", "torque": "N*m", "kt": "N*m/A"},
        "instruments": [{"id": "recorder-1", "model": "test source", "calibration": "virtual"}],
        "preconditions": {"adc_zero_current": "test input", "output_confirmation": "not declared",
                          "independent_reference": "not declared", "load_condition": "not declared",
                          "independent_kt": "not declared"},
        "files": {"samples.csv": {"sha256": hashlib.sha256(raw).hexdigest(), "rows": 1}},
        "generator": {"seed": 0, "sample_rates_hz": {"adc": 1000}, "parameters": {"offset": 2048}},
    }


class PackTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.directory = Path(temp.name)
        self.pack = self.directory / "pack"
        self.pack.mkdir()
        self.samples = self.pack / "samples.csv"
        self.manifest_path = self.pack / "manifest.json"
        self.manifest = make_manifest(SAMPLE)
        self.write_pack()

    def write_pack(self, raw=SAMPLE):
        self.samples.write_bytes(raw)
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def invoke(self, command="validate", *extra, native=NATIVE):
        output = io.StringIO()
        runner_result = subprocess.CompletedProcess([], 0, json.dumps(native).encode(), b"")
        with contextlib.redirect_stdout(output), patch.object(replay.subprocess, "run",
                                                              return_value=runner_result) as runner:
            status = replay.main([command, str(self.pack), "--runner", sys.executable, "--json", *extra])
        return status, json.loads(output.getvalue()), runner

    def assert_parser_rejects(self, reason):
        status, report, runner = self.invoke()
        self.assertEqual(status, 1)
        self.assertEqual(report["reason"], reason)
        runner.assert_not_called()

    def test_runner_receives_exact_verified_bytes_and_configuration(self):
        status, report, runner = self.invoke()
        self.assertEqual(status, 0)
        self.assertEqual(report["manifest_sha256"], hashlib.sha256(self.manifest_path.read_bytes()).hexdigest())
        self.assertEqual(report["provenance"], "PROVENANCE_DECLARED")
        self.assertFalse(report["can_generate_candidate"])
        self.assertEqual(runner.call_args.kwargs["input"], SAMPLE)
        self.assertEqual(runner.call_args.args[0][1:8], ["--validate", "1", "1", "9", "1", "1", "2"])
        self.assertEqual(runner.call_args.args[0][-8:], ["1", "0", "0", "0", "0", "0", "0", "0"])
        self.assertEqual(self.samples.read_bytes(), SAMPLE)

    def test_replacing_file_after_validation_cannot_change_the_runner_input(self):
        validate_csv = replay.validate_csv

        def replace_file(raw, rows):
            validate_csv(raw, rows)
            self.samples.write_bytes(SAMPLE.replace(b"2043", b"2044"))

        with patch.object(replay, "validate_csv", side_effect=replace_file):
            status, _, runner = self.invoke()
        self.assertEqual(status, 0)
        self.assertEqual(runner.call_args.kwargs["input"], SAMPLE)

    def test_hash_rejects_modified_data(self):
        self.samples.write_bytes(SAMPLE.replace(b"2043", b"2044"))
        self.assert_parser_rejects("HASH_MISMATCH")

    def test_oversized_manifest_is_rejected_before_decoding(self):
        self.manifest_path.write_bytes(b" " * (replay.JSON_LIMIT + 1))
        self.assert_parser_rejects("FILE_TOO_LARGE")

    def test_unsupported_trace_version(self):
        self.manifest["format_version"] = 2
        self.write_pack()
        self.assert_parser_rejects("UNSUPPORTED_VERSION")

    def test_invalid_unit_declarations(self):
        for section, field, unit in (("time", "unit", "ms"), ("units", "angle", "deg")):
            with self.subTest(unit=unit):
                self.manifest = make_manifest(SAMPLE)
                self.manifest[section][field] = unit
                self.write_pack()
                self.assert_parser_rejects("UNIT_MISMATCH")

    def test_declared_pairing_cannot_exceed_host_skew_ceiling(self):
        self.manifest["time"]["max_skew_us"] = 51
        self.write_pack()
        self.assert_parser_rejects("CLOCK_POLICY_INVALID")

    def test_duplicate_manifest_key_is_rejected(self):
        raw = self.manifest_path.read_bytes()
        self.manifest_path.write_bytes(b'{"session":9,' + raw[1:])
        self.assert_parser_rejects("MANIFEST_INVALID")

    def test_malformed_csv_is_rejected_even_with_matching_hash(self):
        for row in (ROW + b",", ROW.replace(b"2043", b'"2043"'), ROW.replace(b"2043", b"nan"), ROW + b"\0"):
            with self.subTest(row=row):
                raw = HEADER + b"\n" + row + b"\n"
                self.manifest = make_manifest(raw)
                self.write_pack(raw)
                self.assert_parser_rejects("CSV_INVALID")

    def test_row_count_is_checked_before_native_validation(self):
        raw = SAMPLE + ROW + b"\r\n"
        self.manifest = make_manifest(raw)
        self.write_pack(raw)
        self.assert_parser_rejects("ROW_COUNT_MISMATCH")

    def test_native_reason_and_stage_evidence_survive_explain(self):
        native = dict(NATIVE, trace_valid=False, calibration_state="REJECTED", reason="TIME_SKEW",
                      stages={"LUT": {"reason": "TIME_SKEW", "valid_samples": 17}}, rejected_samples=1)
        status, report, runner = self.invoke("explain", native=native)
        self.assertEqual(status, 1)
        self.assertEqual(report["reason"], "TIME_SKEW")
        self.assertEqual(report["workflow"], native)
        self.assertTrue(report["explanation"])
        self.assertNotIn("--validate", runner.call_args.args[0])

    def test_external_self_declaration_cannot_release_candidate(self):
        self.manifest.update(source="BOARD_RECORDED", host_synthetic=False)
        del self.manifest["generator"]
        self.write_pack()
        status, report, _ = self.invoke("replay")
        self.assertEqual(status, 0)
        self.assertEqual(report["provenance"], "PROVENANCE_UNVERIFIED")
        self.assertFalse(report["can_generate_candidate"])
        self.assertFalse(report["application_eligible"])
        self.assertFalse(report["applied"])
        self.assertFalse(report["persisted"])

    def test_trusted_attestation_binds_exact_manifest_and_independent_path(self):
        self.manifest.update(source="EXTERNAL_RECORDED", host_synthetic=False)
        del self.manifest["generator"]
        self.write_pack()
        attestation = dict(manifest_sha256=hashlib.sha256(self.manifest_path.read_bytes()).hexdigest(),
                           source=self.manifest["source"], identity=self.manifest["identity"],
                           attested_by="authorized acquisition reviewer")
        trust_file = self.directory / "attestation.json"
        trust_file.write_text(json.dumps(attestation), encoding="utf-8")
        status, report, _ = self.invoke("replay", "--attestation", str(trust_file))
        self.assertEqual(status, 0)
        self.assertEqual(report["provenance"], "PROVENANCE_VERIFIED")
        self.assertTrue(report["can_generate_candidate"])
        self.assertFalse(report["application_eligible"])

        bundled = self.pack / "attestation.json"
        bundled.write_bytes(trust_file.read_bytes())
        status, report, runner = self.invoke("replay", "--attestation", str(bundled))
        self.assertEqual((status, report["reason"]), (1, "ATTESTATION_INVALID"))
        runner.assert_not_called()

        self.manifest_path.write_bytes(self.manifest_path.read_bytes() + b"\n")
        status, report, runner = self.invoke("replay", "--attestation", str(trust_file))
        self.assertEqual((status, report["reason"]), (1, "ATTESTATION_MISMATCH"))
        runner.assert_not_called()


@unittest.skipUnless(RUNNER, "pass --runner to exercise the native replay workflow")
class NativeReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.valid = TOOL / "fixtures" / "host-valid"
        cls.invalid = TOOL / "fixtures" / "host-misaligned"

    def invoke(self, command, pack, status=0, as_json=True):
        args = [sys.executable, "-B", str(TOOL / "replay.py"), command, str(pack), "--runner", str(RUNNER)]
        result = subprocess.run(args + (["--json"] if as_json else []), capture_output=True, text=True,
                                encoding="utf-8", timeout=40, check=False)
        self.assertEqual(result.returncode, status, result.stderr or result.stdout)
        self.assertEqual(result.stderr, "")
        return json.loads(result.stdout) if as_json else result.stdout

    def altered_pack(self, transform):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        pack = Path(temp.name)
        manifest = json.loads((self.valid / "manifest.json").read_bytes())
        header, *lines = (self.valid / "samples.csv").read_bytes().splitlines()
        rows = [line.split(b",") for line in lines]
        transform(manifest, rows)
        raw = b"\n".join([header, *(b",".join(row) for row in rows)]) + b"\n"
        manifest["stage_rows"] = [sum(row[0] == str(i).encode() for row in rows) for i in range(8)]
        manifest["files"]["samples.csv"] = {"rows": len(rows), "sha256": hashlib.sha256(raw).hexdigest()}
        (pack / "samples.csv").write_bytes(raw)
        (pack / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        return pack

    def test_canonical_pack_replays_the_soft05_baseline(self):
        paths = [self.valid / filename for filename in ("manifest.json", "samples.csv")]
        original = [path.read_bytes() for path in paths]
        validated = self.invoke("validate", self.valid)
        self.assertTrue(validated["trace_valid"])
        self.assertEqual(validated["calibration_state"], "NOT_RUN")
        self.assertFalse(validated["can_generate_candidate"])
        replayed = self.invoke("replay", self.valid)
        self.assertEqual(replayed, self.invoke("replay", self.valid))
        self.assertEqual(replayed["workflow"], self.invoke("explain", self.valid)["workflow"])
        self.assertEqual(replayed["calibration_state"], "COMPLETED")
        self.assertEqual(replayed["provenance"], "PROVENANCE_DECLARED")
        self.assertEqual(replayed["rejected_samples"], 0)
        self.assertTrue(replayed["can_generate_candidate"])
        self.assertTrue(replayed["workflow"]["review_required"])
        self.assertFalse(replayed["applied"])
        self.assertFalse(replayed["persisted"])
        evidence = replayed["workflow"]
        self.assertEqual({name: stage["valid_samples"] for name, stage in evidence["stages"].items()},
                         {"adc": 64, "electrical": 321, "lut": 256, "lut_holdout": 171, "mechanical": 2401,
                          "offset": 40, "rotor": 40, "tuning": 1})
        metrics = evidence["metrics"]
        self.assertAlmostEqual(metrics["adc"]["offset_counts"][0], 2042.984, delta=0.001)
        self.assertAlmostEqual(metrics["electrical"]["rs_ohm"], 0.5700040, delta=1e-6)
        self.assertAlmostEqual(metrics["electrical"]["inductance_h"], 0.00135003, delta=1e-7)
        self.assertEqual((metrics["rotor"]["direction"], metrics["rotor"]["pole_pairs"]), (-1, 4))
        self.assertAlmostEqual(metrics["offset"]["electrical_offset_rad"], 1.2000237, delta=1e-6)
        self.assertAlmostEqual(metrics["lut"]["holdout_rms_rad"], 0.0017741, delta=1e-6)
        self.assertAlmostEqual(metrics["mechanical"]["inertia_kg_m2"], 0.00240001, delta=1e-7)
        self.assertAlmostEqual(metrics["mechanical"]["damping_nm_s"], 0.00700020, delta=1e-6)
        self.assertAlmostEqual(metrics["tuning"]["stiffness_nm_per_rad"], 0.7776027, delta=1e-6)
        self.assertAlmostEqual(metrics["tuning"]["damping_nm_s_per_rad"], 0.0578000, delta=1e-6)
        self.assertEqual([path.read_bytes() for path in paths], original)

    def test_independent_lut_pair_with_bad_skew_is_rejected(self):
        report = self.invoke("explain", self.invalid, status=1)
        self.assertFalse(report["trace_valid"])
        self.assertFalse(report["can_generate_candidate"])
        self.assertEqual(report["reason"], "ALIGNMENT")
        self.assertEqual(report["workflow"]["stages"]["lut"]["reason"], "ALIGNMENT")
        self.assertIn("skew", " ".join(report["explanation"]))
        terminal = self.invoke("explain", self.invalid, status=1, as_json=False)
        self.assertIn("reason=ALIGNMENT", terminal)

    def test_lut_holdout_cannot_replay_training_payload_with_new_capture_and_time(self):
        def reuse_training_observations(_manifest, rows):
            training = [row for row in rows if row[0] == b"4"]
            holdout = [row for row in rows if row[0] == b"7"]
            for original, replayed in zip(training, holdout):
                replayed[13:] = original[13:]

        report = self.invoke("validate", self.altered_pack(reuse_training_observations), status=1)
        self.assertFalse(report["can_generate_candidate"])
        self.assertEqual(report["workflow"]["stages"]["lut_holdout"]["reason"], "HOLDOUT_REUSED_PAYLOAD")

    def test_declared_encoder_cpr_must_cover_the_raw_observations(self):
        def wrong_cpr(manifest, _rows):
            manifest["encoder"]["cpr"] = 512

        report = self.invoke("validate", self.altered_pack(wrong_cpr), status=1)
        self.assertEqual(report["workflow"]["stages"]["lut"]["reason"], "RANGE")

    def test_missing_kt_still_blocks_complete_calibration(self):
        def synthetic_shaft_capture(manifest, rows):
            # Generate a synthetic shaft torque observation at each existing speed sample.
            # No Kt declaration or D/Q extraction remains in the replay input.
            rows[:] = [row for row in rows if row[0] == b"5"]
            for row in rows:
                row[12] = b"6"
                row[16] = format(0.15 * float(row[14]), ".9g").encode()
                row[14:16] = [b"0", b"0"]
            manifest.update(profile="SELECTED_STAGES", stage_mask=96)

        report = self.invoke("replay", self.altered_pack(synthetic_shaft_capture), status=1)
        self.assertFalse(report["can_generate_candidate"])
        self.assertEqual(report["workflow"]["stages"]["mechanical"]["quality"], "PASS")
        self.assertEqual(report["workflow"]["stages"]["tuning"]["quality"], "DATA_REQUIRED")

    def test_preflight_uses_production_adapter_for_measurement_rejections(self):
        cases = (
            ("adc", b"0", 9, b"2", "UNITS"),           # ADC row in electrical units
            ("adc", b"0", 10, b"786", "IDENTITY"),     # A/B channels exchanged
            ("adc", b"0", 11, b"21", "SATURATION"),     # ADC saturation
            ("electrical", b"1", 12, b"8", "DATA_REQUIRED"),  # command V instead of terminal V
            ("lut", b"4", 12, b"9", "REFERENCE_REQUIRED"),   # derived reference angle
            ("mechanical", b"5", 11, b"11", "LOAD_REQUIRED"),  # unknown load
            ("lut_holdout", b"7", 1, b"12", "CAPTURE_ORDER"),  # reuse training capture ID
            ("lut", b"4", 8, b"2", "CLOCK"),         # unknown reference clock
        )
        for name, kind, column, value, reason in cases:
            with self.subTest(stage=name, column=column):
                def mutate(_manifest, rows):
                    next(row for row in rows if row[0] == kind)[column] = value

                report = self.invoke("validate", self.altered_pack(mutate), status=1)
                self.assertFalse(report["trace_valid"])
                self.assertEqual(report["workflow"]["stages"][name]["reason"], reason)
                self.assertFalse(report["can_generate_candidate"])

    def test_adc_encoder_frame_skew_is_detected_in_electrical_capture(self):
        def skew_encoder(_manifest, rows):
            first = next(row for row in rows if row[0] == b"1")
            first[17:19] = [str(int(first[3]) - 20).encode(), first[7]]

        report = self.invoke("validate", self.altered_pack(skew_encoder), status=1)
        self.assertEqual(report["workflow"]["stages"]["electrical"]["reason"], "ALIGNMENT")

    def test_selected_adc_capture_preserves_an_authorized_wrap(self):
        for allow_wrap in (True, False):
            with self.subTest(allow_wrap=allow_wrap):
                def wrap_adc(manifest, rows):
                    rows[:] = [row for row in rows if row[0] == b"0"]
                    manifest.update(profile="SELECTED_STAGES", stage_mask=1)
                    manifest["time"]["allow_wrap"] = allow_wrap
                    for row in rows:
                        for column in (3, 4):
                            row[column] = str((int(row[column]) - 1100) % (1 << 32)).encode()

                report = self.invoke("replay", self.altered_pack(wrap_adc), status=0 if allow_wrap else 1)
                self.assertEqual(report["trace_valid"], allow_wrap)
                adc = report["workflow"]["stages"]["adc"]
                self.assertEqual(adc["reason"], "NONE" if allow_wrap else "TIME_ORDER")
                self.assertFalse(report["can_generate_candidate"])
                if allow_wrap:
                    self.assertEqual(adc["quality"], "PASS")
                    self.assertGreater(adc["first_us"], adc["last_us"])

    def test_replay_rejects_faults_in_original_adc_timing_and_sequence(self):
        faults = (
            ("TIME_ORDER", 1, {3: b"1000"}),  # duplicate source timestamp
            ("TIME_ORDER", 1, {3: b"999"}),   # backward source timestamp
            ("TIME_ORDER", 1, {3: b"2147484648", 4: b"2147484651"}),  # half-range ambiguity
            ("AGE", 0, {4: b"1051"}),
            ("SEQUENCE", 1, {2: b"0"}),
            ("GAP", 1, None),                # sequence gap after a lost row
        )
        for reason, index, replacements in faults:
            with self.subTest(reason=reason, replacements=replacements):
                def tamper(manifest, rows):
                    rows[:] = [row for row in rows if row[0] == b"0"]
                    manifest.update(profile="SELECTED_STAGES", stage_mask=1)
                    if replacements is None:
                        del rows[index]
                    else:
                        for column, value in replacements.items():
                            rows[index][column] = value

                report = self.invoke("replay", self.altered_pack(tamper), status=1)
                self.assertEqual(report["workflow"]["stages"]["adc"]["reason"], reason)
                self.assertFalse(report["trace_valid"])
                self.assertFalse(report["can_generate_candidate"])


if __name__ == "__main__":
    unittest.main()
