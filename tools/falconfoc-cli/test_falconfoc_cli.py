"""Verify Python wire bytes against the fixed C vectors and the C UART server."""

import pathlib
import os
import subprocess
import sys
import unittest

import falconfoc_cli as foc


ROOT = pathlib.Path(__file__).resolve().parents[2]


class WireTests(unittest.TestCase):
    def test_vectors(self):
        self.assertEqual(foc.crc16(b"123456789"), 0x29B1)
        for line in (ROOT / "protocol/tests/vectors.tsv").read_text().splitlines():
            kind, flags, sequence, session, payload, wire = line.split()
            frame = foc.Frame(int(kind, 16), int(flags, 16), int(sequence, 16),
                              int(session, 16), bytes.fromhex(payload) if payload != "-" else b"")
            expected = bytes.fromhex(wire)
            self.assertEqual(foc.encode(frame), expected)
            self.assertEqual(foc.decode(expected[:-1]), frame)
            if frame.flags == 2:
                foc.describe(frame)
            parser = foc.Stream()
            received = []
            for byte in expected + expected:
                received.extend(parser.feed(bytes([byte])))
            self.assertEqual(received, [frame, frame])

    def test_corruption_and_recovery(self):
        frame = foc.Frame(foc.MESSAGES["status"], 1, 0, 1, b"")
        wire = foc.encode(frame)
        for offset in range(1, len(wire) - 1):
            damaged = bytearray(wire)
            damaged[offset] ^= 4
            parser = foc.Stream()
            received = parser.feed(damaged + wire)
            self.assertEqual(received, [frame])
            self.assertGreater(parser.errors, 0)
        parser = foc.Stream()
        self.assertEqual(parser.feed(b"\xff" * 256 + b"\0" + wire), [frame])
        self.assertEqual(foc.decode(foc.encode(foc.Frame(0x10, 1, 0, 1, b"\0" * 96))[:-1]).payload,
                         b"\0" * 96)


class DeviceTests(unittest.TestCase):
    def test_c_device(self):
        default = ROOT / "build/sw04-host" / ("foc-serial-sim.exe" if os.name == "nt" else "foc-serial-sim")
        executable = os.environ.get("FOC_SERIAL_SIM", str(default))
        with foc.SimulatorTransport(str(executable)) as transport:
            client = foc.Client(transport, sequence=0xFFFFFFFE)
            hello = foc.describe(client.hello())
            self.assertTrue(hello["output_locked"])
            self.assertEqual(hello["physical_modes"], 0)
            status = foc.describe(client.call("status"))
            self.assertTrue(status["output_locked"])
            self.assertFalse(status["physical_applied"])
            parameter = foc.describe(client.call("param-read", foc.parameter_read(1)))
            self.assertEqual(parameter["value"], 24)
            updated = foc.describe(client.call("param-write", foc.parameter_write(1, "u32", 16)))
            self.assertEqual(updated["result"], "APPLIED")
            self.assertEqual(updated["value"], 16)
            self.assertEqual(foc.describe(client.call("arm"))["result"], "OUTPUT_LOCKED")
            self.assertEqual(foc.describe(client.call("start"))["result"], "OUTPUT_LOCKED")
            self.assertEqual(foc.describe(client.call("save"))["result"], "NOT_SUPPORTED")
            self.assertEqual(foc.describe(client.call("stop"))["result"], "ACCEPTED")
            telemetry = foc.describe(client.call("telemetry"))
            self.assertFalse(telemetry["current_valid"])
            self.assertFalse(telemetry["physical_applied"])
            self.assertEqual(telemetry["crc_errors"], 0)

    def test_cli(self):
        tool = ROOT / "tools/falconfoc-cli/falconfoc_cli.py"
        result = subprocess.run([sys.executable, str(tool), "encode", "hello", "--sequence", "0"],
                                check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "06464601010101010101010101010101038ce700")


if __name__ == "__main__":
    unittest.main()
