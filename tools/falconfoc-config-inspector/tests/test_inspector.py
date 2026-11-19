"""Inspect SOFT-03 Host storage images through the public CLI."""

import hashlib
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
import zlib


CLI, GENERATOR = (Path(arg).resolve() for arg in sys.argv[1:3])
del sys.argv[1:3]
PROFILE = ("--profile", "host-fixture-v1")


def change_header_u16(image, slot, field, value):
    """Edit a test image and restore the documented envelope CRC."""
    edited = bytearray(image)
    start = 256 * slot
    struct.pack_into("<H", edited, start + field, value)
    payload_size = struct.unpack_from("<H", edited, start + 16)[0]
    crc = zlib.crc32(edited[start + 4 : start + 52] + edited[start + 56 : start + 56 + payload_size])
    struct.pack_into("<I", edited, start + 52, crc)
    return edited


class InspectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not CLI.is_file():
            raise AssertionError(f"Inspector executable is missing: {CLI}")
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        cls.directory = Path(directory.name)
        cls.image = cls.directory / "normal.bin"
        cls.other = cls.directory / "other-encoder.bin"
        cls.pending = cls.directory / "interrupted.bin"
        cls.ack_lost = cls.directory / "ack-lost.bin"
        for path in (cls.image, cls.other, cls.pending, cls.ack_lost):
            with path.open("xb") as output:
                subprocess.run([GENERATOR, path.stem], stdout=output, check=True, timeout=10)

    def invoke(self, *args, expected=0):
        result = subprocess.run(
            [CLI, *args], capture_output=True, text=True, encoding="utf-8", timeout=10
        )
        self.assertEqual(result.returncode, expected, result.stderr)
        return result

    def read_json(self, command, *args):
        result = self.invoke(command, "--json", *args)
        self.assertEqual(result.stderr, "")
        report = json.loads(result.stdout)
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["command"], command)
        return report

    def test_inspect_reports_selection_and_runtime_unknown(self):
        report = self.read_json("inspect", *PROFILE, self.image)
        self.assertEqual(report["source"], "HOST_STORAGE_IMAGE")
        self.assertIsNone(report["runtime_applied"])
        self.assertEqual(report["recovery"]["selected_slot"], "B")
        self.assertTrue(report["recovery"]["available"])
        self.assertEqual([slot["sequence"] for slot in report["slots"]], [0, 1])
        self.assertEqual([slot["selection"] for slot in report["slots"]], ["OLDER", "SELECTED"])
        for slot in report["slots"]:
            self.assertEqual(slot["crc"], "PASS")
            self.assertEqual(slot["commit"], "COMPLETE")
            self.assertEqual(slot["format_version"], 1)
            self.assertEqual(slot["schema_version"], 2)
            self.assertTrue(slot["applicable"])
        terminal = self.invoke("inspect", *PROFILE, self.image).stdout
        self.assertIn("HOST_STORAGE_IMAGE", terminal)
        self.assertIn("selected=B", terminal)
        self.assertIn("runtime_applied=UNKNOWN", terminal)

    def test_integrity_only_does_not_claim_policy_eligibility(self):
        report = self.read_json("inspect", self.image)
        self.assertIsNone(report["profile"])
        self.assertIsNone(report["recovery"]["available"])
        self.assertTrue(report["slots"][1]["integrity_valid"])
        self.assertIsNone(report["slots"][1]["identity"])
        self.assertIsNone(report["slots"][1]["applicable"])

    def test_identity_mismatch_has_full_integrity_and_no_diff_values(self):
        report = self.read_json("inspect", *PROFILE, self.other)
        self.assertFalse(report["recovery"]["available"])
        slot = report["slots"][0]
        self.assertTrue(slot["integrity_valid"])
        self.assertEqual(slot["reason"], "IDENTITY_MISMATCH")
        self.assertIn("encoder_cpr", slot["identity_mismatches"])
        self.assertIsNone(report["slots"][1]["sequence"])
        self.assertEqual(report["slots"][1]["reason"], "RECORD_NOT_FOUND")
        diff = self.read_json("diff", *PROFILE, self.image, self.other)
        self.assertTrue(diff["left"]["available"])
        self.assertFalse(diff["right"]["available"])
        for field in diff["fields"]:
            self.assertEqual(field["classification"], "IDENTITY_MISMATCH")
            self.assertIsNone(field["right"])

    def test_interrupted_and_acknowledgment_lost_images(self):
        interrupted = self.read_json("inspect", *PROFILE, self.pending)
        self.assertEqual(interrupted["recovery"]["selected_slot"], "A")
        self.assertEqual(interrupted["slots"][1]["reason"], "INCOMPLETE_COMMIT")
        acknowledged = self.read_json("inspect", *PROFILE, self.ack_lost)
        self.assertEqual(acknowledged["recovery"]["selected_slot"], "B")
        self.assertTrue(acknowledged["recovery"]["available"])

    def test_bad_crc_falls_back_to_old_slot_without_modification(self):
        corrupted = bytearray(self.image.read_bytes())
        corrupted[256 + 56] ^= 1
        path = self.directory / "bad-crc.bin"
        path.write_bytes(corrupted)
        before = hashlib.sha256(path.read_bytes()).digest()
        report = self.read_json("inspect", *PROFILE, path)
        self.assertEqual(report["slots"][1]["crc"], "FAIL")
        self.assertEqual(report["slots"][1]["reason"], "BAD_CRC")
        self.assertFalse(report["slots"][1]["integrity_valid"])
        self.assertEqual(report["recovery"]["selected_slot"], "A")
        self.invoke("inspect", path)
        self.read_json("diff", *PROFILE, path)
        self.assertEqual(before, hashlib.sha256(path.read_bytes()).digest())

    def test_future_schema_blocks_restore_of_the_older_eligible_slot(self):
        path = self.directory / "future-schema.bin"
        path.write_bytes(change_header_u16(self.image.read_bytes(), 1, 10, 3))
        report = self.read_json("inspect", *PROFILE, path)
        self.assertTrue(report["slots"][0]["applicable"])
        slot = report["slots"][1]
        self.assertEqual(slot["crc"], "PASS")
        self.assertEqual(slot["schema_version"], 3)
        self.assertEqual(slot["schema"], "FAIL")
        self.assertEqual(report["recovery"]["selected_slot"], "B")
        self.assertFalse(report["recovery"]["available"])
        self.assertEqual(report["recovery"]["reason"], "UNSUPPORTED_SCHEMA")

    def test_foreign_format_keeps_format_specific_metadata_unavailable(self):
        path = self.directory / "foreign-format.bin"
        path.write_bytes(change_header_u16(self.image.read_bytes(), 1, 8, 2))
        report = self.read_json("inspect", *PROFILE, path)
        slot = report["slots"][1]
        self.assertEqual(slot["format_version"], 2)
        self.assertEqual(slot["crc"], "PASS")
        self.assertIsNone(slot["sequence"])
        self.assertIsNone(slot["record_count"])
        self.assertIsNone(slot["schema_version"])
        self.assertIsNone(slot["recorded_identity"])
        self.assertIsNone(report["recovery"]["selected_slot"])
        self.assertEqual(report["recovery"]["reason"], "UNSUPPORTED_FORMAT")

    def test_bad_magic_remains_distinct_from_an_erased_slot(self):
        corrupted = bytearray(self.other.read_bytes())
        corrupted[4] ^= 1
        path = self.directory / "bad-magic.bin"
        path.write_bytes(corrupted)
        report = self.read_json("inspect", path)
        self.assertEqual(report["slots"][0]["reason"], "BAD_MAGIC")
        self.assertEqual(report["slots"][1]["reason"], "RECORD_NOT_FOUND")
        self.assertIsNone(report["slots"][0]["recorded_identity"])
        self.assertIsNone(report["recovery"]["selected_slot"])
        self.assertEqual(report["recovery"]["reason"], "NO_VALID_RECORD")

    def test_diff_reports_protection_fields_and_f32_bits(self):
        report = self.read_json("diff", *PROFILE, self.image)
        self.assertEqual(report["left"]["slot"], "A")
        self.assertEqual(report["right"]["slot"], "B")
        fields = {field["parameter_id"]: field for field in report["fields"]}
        field = fields[0x100C]
        self.assertEqual(field["type"], "F32")
        self.assertEqual(field["unit"], "rad")
        self.assertAlmostEqual(field["left"], 0.2)
        self.assertAlmostEqual(field["right"], 0.3)
        self.assertEqual(field["left_f32_bits"], "0x3e4ccccd")
        self.assertEqual(field["right_f32_bits"], "0x3e99999a")
        self.assertEqual(field["classification"], "VALUE_CHANGED")
        self.assertTrue(field["motion_protection"])
        self.assertTrue(field["requires_reinitialization"])
        defaults = self.read_json("diff", *PROFILE, "--against-default", self.image)
        self.assertEqual(defaults["left"]["slot"], "B")
        self.assertEqual(defaults["right"]["source"], "DEFAULT_CONFIG")
        enabled = next(field for field in defaults["fields"] if field["parameter_id"] == 0x100B)
        self.assertEqual((enabled["left"], enabled["right"]), (1, 0))
        self.assertIn("Motion protection", self.invoke("diff", *PROFILE, self.image).stdout)

    def test_invalid_arguments_and_image_sizes_fail_clearly(self):
        for size in (0, 511, 513, 8192):
            with self.subTest(size=size):
                path = self.directory / f"size-{size}.bin"
                path.write_bytes(bytes(size))
                result = self.invoke("inspect", "--json", path, expected=2)
                self.assertEqual(result.stdout, "")
                self.assertIn("512", result.stderr)
                self.assertEqual(path.stat().st_size, size)
        self.assertIn("open", self.invoke("inspect", self.directory / "missing", expected=2).stderr)
        self.assertIn("profile", self.invoke("inspect", "--profile", "guess", self.image, expected=2).stderr)
        self.assertIn("profile", self.invoke("diff", self.image, expected=2).stderr)
        self.invoke("diff", *PROFILE, "--against-default", self.image, self.other, expected=2)
        self.invoke("inspect", "--json", "--json", self.image, expected=2)
        self.invoke("inspect", "--bad-option", self.image, expected=2)

    def test_explain_returns_stable_restore_and_runtime_reasons(self):
        for reason in (
            "BAD_CRC",
            "OLDER",
            "UNSUPPORTED_SCHEMA",
            "MIGRATION_REQUIRED",
            "REINITIALIZATION_REQUIRED",
            "NOT_APPLIED",
            "OUTPUT_LOCKED",
        ):
            with self.subTest(reason=reason):
                report = self.read_json("explain", reason)
                self.assertEqual(report["reason"], reason)
                self.assertGreater(len(report["explanation"]), 20)
        self.invoke("explain", "unknown-reason", expected=2)


if __name__ == "__main__":
    unittest.main()
