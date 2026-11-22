"""End-to-end Capture identity: formal Board1 boot -> production writer -> Host import -> Locked Timing.

FOC_CAPTURE_BOOT_FIXTURE runs the production foc_board_boot_select() (nominal or
the synthetic verification supply), derives the Capture Profile from the effective
Runtime/Encoder and seals a complete SRAM image with the production C writer.
Rows are Host synthetic data (producer HOST). Identity expectations below are
independent declarations, never read back from the image under test.
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
ENCODER = 128 + 42 * 100
OFFSET = {"encoder_cpr": 76, "encoder_direction": 80, "pole_pairs": 84, "phase_order": 88,
          "encoder_profile": 112, "version": 4}
SNAPSHOT = """FCT 1
FCT ENABLED 1
FCT SPAN h0 100 130 17 0
FCT SPAN h1 50 95 27 0
FCT LANE h0 12 0 0 0 100
FCT LANE h1 5 0 0 0 50
FCT LANE supervision 14 0 0 0 3
FCT LOCK 0 1 0 17 0
FCT END
"""
NOMINAL = {"profile": "NOMINAL", "source_direction": -1, "calibration_direction": 0,
           "runtime_direction": -1, "pole_pairs": 7, "coordinate": "RAW_NO_LUT",
           "supply_id": 0, "review_id": 0}
# Independent declaration of the reviewed synthetic supply (its review record, not the image).
CALIBRATED = {"profile": "CALIBRATED", "source_direction": -1, "calibration_direction": -1,
              "runtime_direction": 1, "pole_pairs": 11, "coordinate": "RAW_NO_LUT",
              "supply_id": 0x56464331, "review_id": 0x01210001}


class BootCaptureTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.path = Path(folder.name)
        self.count = 0

    def image(self, mode, capture_id=None):
        path = self.path / f"{mode}-{self.count}.bin"
        self.count += 1
        args = [str(FIXTURE), mode, str(path)] + ([str(capture_id)] if capture_id else [])
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

    def metadata(self, path, configuration=None, **overrides):
        raw = path.read_bytes()
        header_id = struct.unpack_from("<I", raw, 20)[0]
        meta = {"format_version": 1 if configuration is None else 2, "source": "HOST_VIRTUAL",
                "capture_id": header_id, "build_tag": 0, "firmware_sha256": None,
                "raw_sha256": hashlib.sha256(raw).hexdigest(),
                "identity": dict(device_id=11, motor_id=12, encoder_id=13, frame_id=14),
                "origin_note": "Host synthetic rows from a formally selected Board1 boot"}
        if configuration is not None:
            meta["configuration"] = configuration
        meta.update(overrides)
        return meta

    def run_import(self, path, meta, witness=None):
        meta_path = self.path / f"meta-{self.count}.json"
        out = self.path / f"archive-{self.count}"
        self.count += 1
        meta_path.write_text(json.dumps(meta), encoding="utf-8")
        args = [sys.executable, "-B", str(TOOL / "board_capture.py"), "import", str(path),
                "--metadata", str(meta_path), "--out", str(out)]
        if witness is not None:
            witness_path = self.path / f"witness-{self.count}.json"
            witness_path.write_text(json.dumps(witness), encoding="utf-8")
            args += ["--qualification", str(witness_path)]
        process = subprocess.run(args, capture_output=True, text=True, timeout=20)
        return process.returncode, json.loads(process.stdout), out

    def locked_timing(self, path, configuration=None, source="HOST_FIXTURE"):
        log = self.path / "snapshot.log"
        log.write_text(SNAPSHOT, encoding="utf-8")
        args = [sys.executable, "-B", str(TOOL / "locked_timing.py"), str(log), "--clock-hz", "150000000",
                "--source", source, "--variant", "CAPTURE_ON", "--capture", str(path)]
        if configuration is not None:
            declaration = self.path / f"configuration-{self.count}.json"
            self.count += 1
            declaration.write_text(json.dumps(configuration), encoding="utf-8")
            args += ["--capture-configuration", str(declaration)]
        process = subprocess.run(args, capture_output=True, text=True, timeout=20)
        return process.returncode, json.loads(process.stdout)

    def assert_import_rejected(self, path, meta, reason):
        status, output, _ = self.run_import(path, meta)
        self.assertEqual((status, output.get("reason")), (2, reason), output)

    # Positive end-to-end paths -------------------------------------------------------------

    def test_nominal_boot_reads_through_legacy_and_declared_contracts(self):
        image = self.image("nominal")
        header = struct.unpack_from("<32I", image.read_bytes())
        self.assertEqual((header[19], header[20], header[21], header[28]), (65536, 0xFFFFFFFF, 7, 1))
        status, output, _ = self.run_import(image, self.metadata(image))
        self.assertEqual((status, output["identity_check"]), (0, "FROZEN_NOMINAL"), output)
        self.assertTrue(output["health"]["complete"])
        status, output, _ = self.run_import(image, self.metadata(image, NOMINAL))
        self.assertEqual((status, output["identity_check"]), (0, "DECLARED_CONFIGURATION"), output)
        for configuration in (None, NOMINAL):
            status, report = self.locked_timing(image, configuration)
            self.assertEqual(status, 0, report)
            self.assertEqual(report["capture"]["capture_id"], 33)
            self.assertEqual(report["capture"]["configuration_profile"], 1)
            self.assertTrue(report["capture"]["health"]["complete"])
            self.assertEqual(report["provenance"], "PROVENANCE_DECLARED")

    def test_calibrated_boot_reads_through_declared_contract(self):
        image = self.image("calibrated")
        header = struct.unpack_from("<32I", image.read_bytes())
        self.assertEqual((header[19], header[20], header[21], header[28]), (65536, 1, 11, 2))
        status, output, archive = self.run_import(image, self.metadata(image, CALIBRATED))
        self.assertEqual((status, output["identity_check"]), (0, "DECLARED_CONFIGURATION"), output)
        self.assertEqual(output["provenance"], "PROVENANCE_DECLARED")
        report = json.loads((archive / "diagnostic.json").read_bytes())
        self.assertEqual(report["configuration"], CALIBRATED)
        self.assertEqual((report["header"]["encoder_direction"], report["header"]["pole_pairs"]), (1, 11))
        status, timing = self.locked_timing(image, CALIBRATED)
        self.assertEqual(status, 0, timing)
        self.assertEqual(timing["capture"]["capture_id"], 34)
        self.assertEqual(timing["capture"]["identity_check"], "DECLARED_CONFIGURATION")
        self.assertTrue(timing["capture"]["health"]["complete"])
        self.assertFalse(timing["target_wcet_verified"])
        print(f"calibrated boot capture: import status {status}, identity DECLARED_CONFIGURATION, "
              f"locked timing capture {timing['capture']['capture_id']} complete")

    def test_calibrated_trace_keeps_raw_board_direction(self):
        image = self.image("calibrated")
        start, end = struct.unpack_from("<2I", image.read_bytes(), 36)
        meta = self.metadata(image, CALIBRATED)
        witness = {"format_version": 1, "stage": "ADC_ZERO", "capture_id": 34,
                   "raw_sha256": meta["raw_sha256"], "firmware_sha256": None,
                   "covers_start_us": start, "covers_end_us": end, "witness_source": "HOST_VIRTUAL",
                   "independent_zero_current": True, "sampling_window_verified": True,
                   "amp_per_count": 0.001, "instrument": {"id": "fixture", "model": "deterministic fixture",
                                                          "calibration": "defined by the test"},
                   "evidence_reference": "Host fixture defines independent zero current."}
        status, output, archive = self.run_import(image, meta, witness)
        self.assertEqual((status, output["status"]), (0, "QUALIFIED_MEASUREMENT_TRACE"), output)
        manifest = json.loads((archive / "trace" / "manifest.json").read_bytes())
        self.assertEqual(manifest["encoder"], {"cpr": 65536, "raw_direction": -1, "phase_order": 0,
                                               "pole_pairs": 11})

    # Contract and mislabel rejections --------------------------------------------------

    def test_calibrated_image_requires_declaration(self):
        image = self.image("calibrated")
        self.assert_import_rejected(image, self.metadata(image), "CONFIGURATION_REQUIRED")
        status, report = self.locked_timing(image)
        self.assertEqual((status, report["reason"]), (2, "CONFIGURATION_REQUIRED"))
        process = subprocess.run([sys.executable, "-B", str(TOOL / "board_capture.py"), "describe", str(image)],
                                 capture_output=True, text=True, timeout=20)
        self.assertEqual((process.returncode, json.loads(process.stdout)["reason"]), (2, "CONFIGURATION_REQUIRED"))

    def test_mislabelled_configurations(self):
        calibrated, nominal = self.image("calibrated"), self.image("nominal")
        self.assert_import_rejected(calibrated, self.metadata(calibrated, NOMINAL), "CONFIGURATION_MISMATCH")
        self.assert_import_rejected(nominal, self.metadata(nominal, CALIBRATED), "CONFIGURATION_MISMATCH")
        status, report = self.locked_timing(nominal, CALIBRATED)
        self.assertEqual((status, report["reason"]), (2, "CONFIGURATION_MISMATCH"))
        status, report = self.locked_timing(calibrated, NOMINAL)
        self.assertEqual((status, report["reason"]), (2, "CONFIGURATION_MISMATCH"))

    def test_declaration_disagreeing_with_image(self):
        image = self.image("calibrated")
        cases = (
            (dict(pole_pairs=12), "RAW_IDENTITY"),
            (dict(runtime_direction=-1, calibration_direction=1), "RAW_IDENTITY"),
            (dict(source_direction=1, runtime_direction=-1), "CONFIGURATION_MISMATCH"),
            (dict(coordinate="UNKNOWN"), "COORDINATE_UNSUPPORTED"),
            (dict(coordinate="LUT_CORRECTED"), "COORDINATE_UNSUPPORTED"),
            (dict(runtime_direction=-1), "CONFIGURATION_INVALID"),
            (dict(review_id=0), "CONFIGURATION_INVALID"),
            (dict(supply_id=0), "CONFIGURATION_INVALID"),
            (dict(calibration_direction=0, runtime_direction=-1), "CONFIGURATION_INVALID"),
        )
        for change, reason in cases:
            with self.subTest(change=change):
                self.assert_import_rejected(image, self.metadata(image, {**CALIBRATED, **change}), reason)

    def test_recomputed_crc_does_not_hide_identity_mismatch(self):
        cases = (
            ("pole_pairs", 12, "RAW_IDENTITY"),
            ("encoder_direction", -1, "RAW_IDENTITY"),
            ("encoder_cpr", 4096, "RAW_IDENTITY"),
            ("phase_order", 1, "RAW_IDENTITY"),
            ("encoder_profile", 3, "RAW_IDENTITY"),
            ("encoder_profile", 1, "CONFIGURATION_MISMATCH"),
        )
        for field, value, reason in cases:
            with self.subTest(field=field, value=value):
                image = self.image("calibrated")
                self.patch(image, [(OFFSET[field], value)])
                self.assert_import_rejected(image, self.metadata(image, CALIBRATED), reason)
        nominal = self.image("nominal")
        self.patch(nominal, [(OFFSET["pole_pairs"], 11), (OFFSET["encoder_direction"], 1)])
        self.assert_import_rejected(nominal, self.metadata(nominal), "RAW_IDENTITY")

    def test_integrity_layout_and_provenance(self):
        image = self.image("calibrated")
        self.patch(image, [(OFFSET["pole_pairs"], 12)], recompute=False)
        self.assert_import_rejected(image, self.metadata(image, CALIBRATED), "CRC_MISMATCH")
        image = self.image("calibrated")
        self.patch(image, [(OFFSET["version"], 2)])
        self.assert_import_rejected(image, self.metadata(image, CALIBRATED), "RAW_VERSION")
        image = self.image("calibrated")
        image.write_bytes(image.read_bytes()[:-4])
        self.assert_import_rejected(image, self.metadata(image, CALIBRATED), "RAW_SIZE")
        image = self.image("calibrated")
        self.assert_import_rejected(image, self.metadata(image, CALIBRATED, capture_id=35), "CAPTURE_ID_MISMATCH")
        image = self.image("calibrated")
        self.patch(image, [(ENCODER, 33)])
        self.assert_import_rejected(image, self.metadata(image, CALIBRATED), "CAPTURE_ID_MISMATCH")
        image = self.image("calibrated")
        self.assert_import_rejected(image, self.metadata(image, CALIBRATED, source="BOARD_RECORDED"),
                                    "SOURCE_MISMATCH")
        status, report = self.locked_timing(image, CALIBRATED, source="BOARD_DECLARED")
        self.assertEqual((status, report["reason"]), (2, "SOURCE_MISMATCH"))
        meta = self.metadata(image, CALIBRATED)
        meta["format_version"] = 1
        self.assert_import_rejected(image, meta, "METADATA_INVALID")


if __name__ == "__main__":
    unittest.main(verbosity=2)
