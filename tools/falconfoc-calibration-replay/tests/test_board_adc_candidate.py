"""FW-ADC-01 Capture -> ADC candidate -> isolated Runtime, through production components only.

Images come from the production C writer (FOC_CAPTURE_BOOT_FIXTURE, formal boot
selection, Host synthetic rows). Import and trace preparation use the production
board_capture.py; the estimator, admission and boot selection run inside the C
adapter (FOC_ADC_CANDIDATE). Expected values are independent test declarations.
"""

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


TOOL = Path(__file__).resolve().parents[1]
FIXTURE = Path(os.environ["FOC_CAPTURE_BOOT_FIXTURE"])
ADAPTER = Path(os.environ["FOC_ADC_CANDIDATE"])
ADC, ENCODER = 128, 128 + 42 * 100
TARGET = (2041.25, 2056.75, 2049.5)
CALIBRATED = {"profile": "CALIBRATED", "source_direction": -1, "calibration_direction": -1,
              "runtime_direction": 1, "pole_pairs": 11, "coordinate": "RAW_NO_LUT",
              "supply_id": 0x56464331, "review_id": 0x01210001}
NOMINAL = {"profile": "NOMINAL", "source_direction": -1, "calibration_direction": 0,
           "runtime_direction": -1, "pole_pairs": 7, "coordinate": "RAW_NO_LUT",
           "supply_id": 0, "review_id": 0}
GAIN = 3.3 / (4095 * 0.5)


class AdcCandidateTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.path = Path(folder.name)
        self.count = 0

    def fresh(self, stem):
        self.count += 1
        return self.path / f"{stem}-{self.count}"

    def image(self, mode="calibrated", adc=TARGET, noise=0, producer="host"):
        path = self.fresh(mode).with_suffix(".bin")
        args = [str(FIXTURE), mode, str(path), "--adc", ",".join(map(str, adc)),
                "--noise", str(noise), "--producer", producer]
        subprocess.run(args, check=True, capture_output=True, timeout=10)
        return path

    def patch(self, path, changes, recompute=True):
        raw = bytearray(path.read_bytes())
        for offset, value in changes:
            struct.pack_into("<I", raw, offset, value & 0xFFFFFFFF)
        if recompute:
            struct.pack_into("<I", raw, 124, 0)
            struct.pack_into("<I", raw, 124, zlib.crc32(raw))
        path.write_bytes(raw)

    def inputs(self, path, configuration, source="HOST_VIRTUAL", witness=True, **meta_overrides):
        raw = path.read_bytes()
        sha = hashlib.sha256(raw).hexdigest()
        capture_id = struct.unpack_from("<I", raw, 20)[0]
        start, end = struct.unpack_from("<2I", raw, 36)
        board = source == "BOARD_RECORDED"
        meta = {"format_version": 1 if configuration is None else 2, "source": source,
                "capture_id": capture_id, "build_tag": 0, "firmware_sha256": "1" * 64 if board else None,
                "raw_sha256": sha, "identity": dict(device_id=11, motor_id=12, encoder_id=13, frame_id=14),
                "origin_note": "FW-ADC-01 Host synthetic rows from the production writer"}
        if configuration is not None:
            meta["configuration"] = configuration
        meta.update(meta_overrides)
        meta_path = self.fresh("meta").with_suffix(".json")
        meta_path.write_text(json.dumps(meta), encoding="utf-8")
        witness_path = None
        if witness:
            witness_path = self.fresh("witness").with_suffix(".json")
            witness_path.write_text(json.dumps({
                "format_version": 1, "stage": "ADC_ZERO", "capture_id": capture_id, "raw_sha256": sha,
                "firmware_sha256": meta["firmware_sha256"], "covers_start_us": start, "covers_end_us": end,
                "witness_source": "EXTERNAL_INSTRUMENT" if board else "HOST_VIRTUAL",
                "independent_zero_current": True, "sampling_window_verified": True, "amp_per_count": GAIN,
                "instrument": {"id": "fixture", "model": "deterministic fixture", "calibration": "test"},
                "evidence_reference": "Host fixture declares zero current; not a physical witness."}),
                encoding="utf-8")
        return meta_path, witness_path

    def orchestrate(self, path, configuration=CALIBRATED, fault=None, host_kind="synthetic", **kw):
        meta, witness = self.inputs(path, configuration, **kw)
        out = self.fresh("out")
        args = [sys.executable, "-B", str(TOOL / "board_adc_candidate.py"), str(path), "--metadata", str(meta),
                "--adapter", str(ADAPTER), "--out", str(out), "--host-kind", host_kind]
        if witness is not None:
            args += ["--qualification", str(witness)]
        if fault:
            args += ["--fault", fault]
        process = subprocess.run(args, capture_output=True, text=True, timeout=60)
        report = json.loads(process.stdout)
        return process.returncode, report, out

    def adapter(self, csv_path, capture_id=34, zero="QUALIFIED", source="HOST_SYNTHETIC", fault=None):
        args = [str(ADAPTER), str(csv_path), "--capture-id", str(capture_id), "--review-id", "1",
                "--source", source, "--zero-current", zero] + (["--fault", fault] if fault else [])
        process = subprocess.run(args, capture_output=True, text=True, timeout=30)
        return process.returncode, json.loads(process.stdout)

    def prepared_csv(self):
        status, report, out = self.orchestrate(self.image())
        self.assertEqual(status, 0, report)
        return (out / "import" / "trace" / "samples.csv").read_text(encoding="ascii").splitlines()

    def edited_csv(self, lines):
        path = self.fresh("edited").with_suffix(".csv")
        path.write_text("\n".join(lines) + "\n", encoding="ascii")
        return path

    def assert_accepted(self, status, report, target=TARGET):
        self.assertEqual((status, report["status"]), (0, "CANDIDATE_ACCEPTED_ISOLATED"), report)
        candidate = report["candidate"]
        for phase, want in zip("abc", target):
            self.assertLessEqual(abs(candidate[f"offset_{phase}"] - want), 1.0e-3, candidate)
        self.assertTrue(candidate["runtime_offsets_equal_candidate"] and candidate["uart_offsets_equal_runtime"])
        self.assertTrue(candidate["output_locked"] and candidate["update_refused"] and candidate["formal_untouched"])
        self.assertEqual((candidate["rows"], candidate["samples_collected"]), (40, 40))
        self.assertEqual((candidate["first_sequence"], candidate["last_sequence"]), (0, 39))
        self.assertEqual(candidate["capture_id"], report["capture_id"])
        for current in candidate["decoded_current_a"]:
            self.assertLessEqual(abs(current), 0.5 * GAIN + 1e-7)
        return candidate

    def assert_no_candidate(self, status, report, failure, code=3):
        self.assertEqual(status, code, report)
        candidate = report["candidate"] if "candidate" in report else report
        self.assertFalse(candidate["candidate_qualified"], candidate)
        self.assertEqual(candidate["candidate_failure"], failure, candidate)
        self.assertTrue(candidate["formal_untouched"], candidate)

    # Positive ------------------------------------------------------------------------

    def test_calibrated_capture_reaches_isolated_runtime(self):
        status, report, _ = self.orchestrate(self.image())
        candidate = self.assert_accepted(status, report)
        self.assertEqual(report["identity_check"], "DECLARED_CONFIGURATION")
        self.assertEqual((report["source"], candidate["source"]), ("HOST_VIRTUAL", "HOST_SYNTHETIC"))
        self.assertEqual(report["zero_current"], "HOST_SYNTHETIC_FIXTURE_DECLARED")
        self.assertEqual(candidate["candidate_failure"], "NONE")
        self.assertGreater(candidate["first_source_us"], candidate["last_source_us"])  # uint32 wrap kept
        print(f"calibrated capture candidate: {candidate['offset_a']}/{candidate['offset_b']}/"
              f"{candidate['offset_c']} -> isolated CALIBRATED, UART == Runtime, formal untouched")

    def test_nominal_capture_reaches_isolated_runtime(self):
        status, report, _ = self.orchestrate(self.image("nominal"), configuration=None)
        self.assert_accepted(status, report)
        self.assertEqual(report["identity_check"], "FROZEN_NOMINAL")
        status, report, _ = self.orchestrate(self.image("nominal"), configuration=NOMINAL)
        self.assert_accepted(status, report)

    def test_host_virtual_label_and_legacy_rows(self):
        status, report, _ = self.orchestrate(self.image(), host_kind="virtual")
        self.assertEqual(self.assert_accepted(status, report)["source"], "HOST_VIRTUAL")
        legacy = self.fresh("legacy").with_suffix(".bin")
        subprocess.run([str(FIXTURE), "nominal", str(legacy)], check=True, capture_output=True, timeout=10)
        status, report, _ = self.orchestrate(legacy, configuration=None)
        self.assert_accepted(status, report, target=(2032, 2063, 2018))

    # Zero-current qualification and provenance ---------------------------------------

    def test_zero_current_must_be_declared(self):
        status, report, out = self.orchestrate(self.image(), witness=False)
        self.assertEqual((status, report["status"], report["candidate"]), (3, "PRECONDITION_REQUIRED", None))
        self.assertFalse((out / "adapter.json").exists())
        status, result = self.adapter(self.edited_csv(self.prepared_csv()), zero="UNQUALIFIED")
        self.assert_no_candidate(status, result, "PRECONDITION")

    def test_board_recorded_without_independent_zero_current(self):
        image = self.image(producer="board")
        status, report, out = self.orchestrate(image, source="BOARD_RECORDED")
        self.assertEqual((status, report["status"]), (3, "PRECONDITION_UNVERIFIED"), report)
        self.assertEqual((report["provenance"], report["zero_current"], report["candidate"]),
                         ("PROVENANCE_UNVERIFIED", "UNVERIFIED", None))
        self.assertFalse((out / "adapter.json").exists())
        status, result = self.adapter(self.edited_csv(self.prepared_csv()), source="BOARD_RECORDED")
        self.assertEqual((status, result["reason"]), (2, "PRECONDITION_UNVERIFIED"))

    # Importer-level rejections (production board_capture.py) ---------------------------

    def import_rejected(self, image, reason, configuration=CALIBRATED, **kw):
        status, report, out = self.orchestrate(image, configuration=configuration, **kw)
        self.assertEqual((status, report["status"], report.get("reason")), (2, "REJECTED", reason), report)
        self.assertFalse((out / "adapter.json").exists())

    def test_capture_integrity_identity_and_time(self):
        image = self.image()
        self.patch(image, [(ADC + 100 + 32, 2060 << 16 | 1241)], recompute=False)
        self.import_rejected(image, "CRC_MISMATCH")
        image = self.image()
        self.patch(image, [(ADC + 100 + 8, 0)])
        self.import_rejected(image, "ROW_SEQUENCE")
        image = self.image()
        self.patch(image, [(ADC + 100 + 16, 100), (ADC + 100 + 92, 100)])
        self.import_rejected(image, "ACQUISITION_INCOMPLETE")
        image = self.image()
        self.patch(image, [(ADC + 39 * 100 + 16, 140), (ADC + 39 * 100 + 92, 140)])
        self.import_rejected(image, "ACQUISITION_INCOMPLETE")  # missing slot 139
        image = self.image()
        self.patch(image, [(ADC + 100 + 16, 99), (ADC + 100 + 92, 99)])
        self.import_rejected(image, "SEQUENCE_ORDER")
        image = self.image()
        first = struct.unpack_from("<I", image.read_bytes(), ADC + 12)[0]
        self.patch(image, [(ADC + 100 + 12, first - 1)])
        self.import_rejected(image, "TIME_ORDER")
        image = self.image()
        self.patch(image, [(ADC + 24, (first - (1 << 31)))])
        self.import_rejected(image, "TIME_ORDER")
        image = self.image()
        start = struct.unpack_from("<I", image.read_bytes(), 36)[0]
        self.patch(image, [(ADC + 39 * 100 + 12, start + 8000)])
        self.import_rejected(image, "TIME_ORDER")
        image = self.image()
        self.patch(image, [(ADC + 5 * 100, 35)])
        self.import_rejected(image, "CAPTURE_ID_MISMATCH")
        image = self.image()
        self.patch(image, [(68, 0x04050601)])
        self.import_rejected(image, "RANK_MAP")
        image = self.image()
        self.patch(image, [(76, 4096)])
        self.import_rejected(image, "RAW_IDENTITY")
        self.import_rejected(self.image(), "CONFIGURATION_MISMATCH", configuration=NOMINAL)
        image = self.image()
        self.patch(image, [(24, 1)])  # recomputed CRC, producer now BOARD
        self.import_rejected(image, "SOURCE_MISMATCH")

    # Estimator-level rejections (existing foc_calibration_adc_*) -------------------------

    def test_estimator_quality_failures(self):
        status, report, _ = self.orchestrate(self.image(adc=(2041, 2056, 2700)))
        self.assert_no_candidate(status, report, "RANGE")
        status, report, _ = self.orchestrate(self.image(adc=(1500, 2056, 2049)))
        self.assert_no_candidate(status, report, "RANGE")
        status, report, _ = self.orchestrate(self.image(noise=3))
        self.assert_no_candidate(status, report, "NOISE")
        status, report, _ = self.orchestrate(self.image(noise=11))
        self.assert_no_candidate(status, report, "INPUT_CHANGED")
        status, report, _ = self.orchestrate(self.image(), fault="short")
        self.assert_no_candidate(status, report, "SAMPLE_COUNT")
        status, report, _ = self.orchestrate(self.image(), fault="adc-init")
        self.assert_no_candidate(status, report, "INVALID_CONFIG")

    def test_trace_sequence_and_time_at_estimator(self):
        lines = self.prepared_csv()
        header, rows = lines[0], [line.split(",") for line in lines[1:]]

        def variant(edit):
            copy = [list(row) for row in rows]
            edit(copy)
            return self.edited_csv([header] + [",".join(row) for row in copy])

        cases = {
            "duplicate sequence": lambda r: r[5].__setitem__(2, r[4][2]),
            "reversed sequence": lambda r: r[5].__setitem__(2, "1"),
            "duplicate time": lambda r: r[5].__setitem__(3, r[4][3]),
            "gap beyond interval": lambda r: r[5].__setitem__(3, str((int(r[4][3]) + 5000) & 0xFFFFFFFF)),
        }
        for name, edit in cases.items():
            with self.subTest(case=name):
                status, result = self.adapter(variant(edit))
                self.assert_no_candidate(status, result, "TIME_ORDER")
        status, result = self.adapter(variant(lambda r: r[3].__setitem__(1, "35")))
        self.assertEqual((status, result["reason"]), (2, "CAPTURE_ID_MISMATCH"))
        # A sequence gap inside the interval bound is not an estimator failure; the
        # importer's slot-sequence health owns gap detection (see the import test).
        status, result = self.adapter(variant(lambda r: r.pop(10)))
        self.assertEqual((status, result["samples_collected"]), (0, 39), result)

    # Admission and isolated Runtime ---------------------------------------------------

    def test_identity_and_snapshot_failures_keep_formal_runtime(self):
        for fault, rejection in (("identity-cpr", 4), ("identity-phase", 4), ("identity-gain", 4),
                                 ("rotor-pole-pairs", 4), ("runtime-init", 9)):
            with self.subTest(fault=fault):
                status, report, _ = self.orchestrate(self.image(), fault=fault)
                candidate = report["candidate"]
                self.assertEqual((status, report["status"]), (4, "CANDIDATE_REJECTED_NOMINAL_FALLBACK"), report)
                self.assertEqual((candidate["isolated_configuration"], candidate["isolated_calibration"],
                                  candidate["isolated_rejection"]), (1, 3, rejection))
                self.assertTrue(candidate["formal_untouched"])
                if fault != "runtime-init":
                    self.assertTrue(candidate["candidate_qualified"])

    def test_swapped_current_ranks_are_not_detectable(self):
        # Rank 2 (IC) and rank 3 (IB) exchanged in the rows: every check still passes,
        # because no independent current reference exists in the capture.
        status, report, _ = self.orchestrate(self.image(adc=(TARGET[1], TARGET[0], TARGET[2])))
        candidate = self.assert_accepted(status, report, target=(TARGET[1], TARGET[0], TARGET[2]))
        print("swapped IC/IB ranks accepted with swapped offsets "
              f"{candidate['offset_a']}/{candidate['offset_b']}: not identifiable from this evidence")


if __name__ == "__main__":
    unittest.main(verbosity=2)
