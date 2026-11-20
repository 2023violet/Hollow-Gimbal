"""Test offline locked-image observation reporting with explicit Host fixtures."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from locked_timing import parse_snapshot, stack_summary
from replay import PackError


SNAPSHOT = """FCT 1
FCT ENABLED 1
FCT SPAN h0 4294967280 14 17 0
FCT SPAN h1 50 95 27 0
FCT SPAN adc_trigger 100 130 17 0
FCT LANE h0 12 1 2 0 100
FCT LANE h1 5 0 0 0 50
FCT LANE supervision 14 1 0 0 3
FCT LOCK 0 1 0 17 0
FCT END
"""


class TimingTests(unittest.TestCase):
    def test_cycle_wrap_and_outputs(self):
        report = parse_snapshot(SNAPSHOT, 150_000_000)
        self.assertEqual(report["spans"]["h0"]["cycles"], 30)
        self.assertEqual(report["spans"]["h0"]["elapsed_ns"], 200)
        self.assertEqual(report["lanes"]["h0"]["missed"], 1)
        self.assertTrue(report["lock"]["software_inhibit_confirmed"])
        self.assertIn("adc_irq", report["unobserved_spans"])
        self.assertFalse(report["target_wcet_verified"])

    def test_disabled_probe(self):
        with self.assertRaisesRegex(PackError, "CYCCNT"):
            parse_snapshot(SNAPSHOT.replace("ENABLED 1", "ENABLED 0"), 150_000_000)

    def test_rejects_interrupted_or_mixed_snapshot(self):
        for text in (SNAPSHOT.replace("FCT END", ""), SNAPSHOT + SNAPSHOT,
                     SNAPSHOT.replace("FCT LOCK", "FCT UNKNOWN")):
            with self.subTest(text=text), self.assertRaises(PackError):
                parse_snapshot(text, 150_000_000)

    def test_rejects_ambiguity_and_duplicate(self):
        for text in (SNAPSHOT.replace("50 95", "50 49"), SNAPSHOT.replace("h1 50 95", "h0 50 95")):
            with self.subTest(text=text), self.assertRaises(PackError):
                parse_snapshot(text, 150_000_000)

    def test_rejects_confirmed_non_inhibit(self):
        with self.assertRaisesRegex(PackError, "non-INHIBIT"):
            parse_snapshot(SNAPSHOT.replace("FCT LOCK 0", "FCT LOCK 1"), 150_000_000)

    def test_stack_painted_region(self):
        start, end, top = 0x20001000, 0x20001100, 0x20001200
        self.assertIsNone(stack_summary(b"\xa5" * 256, start, end, top)["minimum_observed_depth_bytes"])
        data = b"\xa5" * 128 + bytes(128)
        self.assertEqual(stack_summary(data, start, end, top)["minimum_observed_depth_bytes"], 384)
        with self.assertRaises(PackError):
            stack_summary(data[:-1], start, end, top)


if __name__ == "__main__":
    unittest.main(verbosity=2)