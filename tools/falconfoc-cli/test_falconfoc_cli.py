"""Verify Python wire bytes against the fixed C vectors and the C UART server."""

import pathlib
import os
import json
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
            # Startup readback: default supply gives NOMINAL (1) + NOT_AVAILABLE (1).
            boot = foc.describe(client.call("param-read", foc.parameter_read(32)))
            self.assertEqual((boot["result"], boot["value"], boot["writable"]), ("OK", 1, False))
            source = foc.describe(client.call("param-read", foc.parameter_read(33)))
            self.assertEqual(source["value"], 1)
            offset = foc.describe(client.call("param-read", foc.parameter_read(38)))
            self.assertEqual((offset["parameter_type"], offset["unit"], offset["value"]),
                             ("f32", 4, 2048.0))
            qualified = foc.describe(client.call("param-read", foc.parameter_read(45)))
            self.assertEqual(qualified["value"], 0)
            denied = foc.describe(client.call("param-write", foc.parameter_write(38, "f32", 2000.0)))
            self.assertEqual(denied["result"], "NOT_SUPPORTED")

    def test_cli(self):
        tool = ROOT / "tools/falconfoc-cli/falconfoc_cli.py"
        result = subprocess.run([sys.executable, str(tool), "encode", "hello", "--sequence", "0"],
                                check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "06464601010101010101010101010101038ce700")

    def test_virtual_demo(self):
        executable = os.environ.get("FOC_VIRTUAL_DEVICE")
        if not executable:
            self.skipTest("FOC_VIRTUAL_DEVICE is not configured")
        tool = ROOT / "tools/falconfoc-cli/falconfoc_cli.py"
        result = subprocess.run(
            [
                sys.executable,
                str(tool),
                "--simulator",
                executable,
                "virtual-demo",
                "--virtual-scenario",
                "velocity",
                "--advance-us",
                "500",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        records = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        lifecycle = [record for record in records if record.get("kind") == "lifecycle"]
        virtual = [record for record in records if record.get("kind") == "virtual"]
        self.assertTrue(any(item["result"] == "APPLIED" and item["action"] == 1 for item in lifecycle))
        self.assertTrue(any(item["result"] == "APPLIED" and item["action"] == 2 for item in lifecycle))
        self.assertGreater(max(item["virtual_update_accepted"] for item in virtual), 0)
        self.assertTrue(all(item["physical_applied"] is False for item in virtual))
        status = [record for record in records if record.get("message") == "status"][-1]
        self.assertEqual(status["state"], 1)

    def test_virtual_fault_and_exports(self):
        executable = os.environ.get("FOC_VIRTUAL_DEVICE")
        if not executable:
            self.skipTest("FOC_VIRTUAL_DEVICE is not configured")
        tool = ROOT / "tools/falconfoc-cli/falconfoc_cli.py"
        csv_path = ROOT / "build/vm01a-simulation/test-vm01d-fault.csv"
        json_path = ROOT / "build/vm01a-simulation/test-vm01d-fault.json"
        result = subprocess.run(
            [
                sys.executable,
                str(tool),
                "--simulator",
                executable,
                "virtual-demo",
                "--virtual-scenario",
                "fault",
                "--advance-us",
                "500",
                "--duration-us",
                "10000",
                "--csv",
                str(csv_path),
                "--json",
                str(json_path),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        summaries = [
            json.loads(line)
            for line in result.stdout.splitlines()
            if line.strip() and json.loads(line).get("kind") == "summary"
        ]
        self.assertEqual(len(summaries), 1)
        metrics = summaries[0]["metrics"]
        self.assertEqual(metrics["final_state"], 4)
        self.assertTrue(metrics["no_update_after_inhibit"])
        self.assertTrue(csv_path.exists() and json_path.exists())
        self.assertIn("virtual_time_us", csv_path.read_text(encoding="utf-8").splitlines()[0])
        self.assertEqual(metrics["software_inhibit_latency_us"], 0)
        self.assertGreaterEqual(metrics["fault_latch_latency_us"], 0)
        self.assertEqual(metrics["updates_after_inhibit"], 0)

    def test_control_quality_windows_exclude_non_running_samples(self):
        records = [
            {"kind": "virtual", "virtual_time_us": 0, "state": 1, "output_action": 0,
             "control_mode": 0, "iq_target_a": 0.4, "iq_actual_a": 0.0,
             "virtual_update_accepted": 0, "virtual_update_rejected": 0,
             "saturation_count": 0, "faults": 0, "replay_hash": 1},
            {"kind": "virtual", "virtual_time_us": 100, "state": 3, "output_action": 1,
             "control_mode": 0, "iq_target_a": 0.4, "iq_actual_a": 0.25,
             "virtual_update_accepted": 1, "virtual_update_rejected": 0,
             "saturation_count": 0, "faults": 0, "replay_hash": 2},
            {"kind": "virtual", "virtual_time_us": 200, "state": 3, "output_action": 1,
             "control_mode": 0, "iq_target_a": 0.4, "iq_actual_a": 0.4,
             "virtual_update_accepted": 2, "virtual_update_rejected": 0,
             "saturation_count": 0, "faults": 0, "replay_hash": 3},
            {"kind": "virtual", "virtual_time_us": 300, "state": 1, "output_action": 0,
             "control_mode": 0, "iq_target_a": 0.4, "iq_actual_a": 0.1,
             "virtual_update_accepted": 2, "virtual_update_rejected": 0,
             "saturation_count": 0, "faults": 0, "replay_hash": 4},
        ]
        metrics = foc._virtual_metrics(records, "current", 300)
        self.assertGreater(metrics["full_window_rmse"], metrics["running_window_rmse"])
        self.assertEqual(metrics["measurement_window"]["running"]["sample_count"], 2)
        self.assertEqual(metrics["measurement_window"]["full"]["sample_count"], 4)
        self.assertEqual(metrics["control_quality"], "CONTROL_QUALITY_FAIL")
        self.assertIsNone(metrics["settling_us"])
        self.assertEqual(metrics["settling_time_us"], 200)
        full = metrics["measurement_window"]["full"]["tracking_error"]
        running = metrics["measurement_window"]["running"]["tracking_error"]
        self.assertAlmostEqual(full["final"], 0.3)
        self.assertEqual(full["out_of_tolerance_samples"], 3)
        self.assertEqual(full["first_out_of_tolerance_us"], 0)
        self.assertEqual(full["last_out_of_tolerance_us"], 300)
        self.assertEqual(running["final"], 0.0)
        self.assertAlmostEqual(running["peak"], 0.15)
        self.assertEqual(running["out_of_tolerance_samples"], 1)
        self.assertEqual(running["first_out_of_tolerance_us"], 100)
        self.assertEqual(running["last_out_of_tolerance_us"], 100)

        # A pause in actuation cannot hide an actual end-of-run tracking error.
        records[2]["iq_actual_a"] = 0.25
        failed = foc._virtual_metrics(records, "current", 300)
        running = failed["measurement_window"]["running"]["tracking_error"]
        self.assertAlmostEqual(running["final"], 0.15)
        self.assertEqual(running["out_of_tolerance_samples"], 2)
        self.assertEqual(running["last_out_of_tolerance_us"], 200)
        self.assertIsNone(failed["settling_time_us"])

    def test_load_counterfactual_oracle(self):
        executable = os.environ.get("FOC_VIRTUAL_DEVICE")
        if not executable:
            self.skipTest("FOC_VIRTUAL_DEVICE is not configured")
        tool = ROOT / "tools/falconfoc-cli/falconfoc_cli.py"

        def run(torque):
            result = subprocess.run(
                [
                    sys.executable, str(tool), "--simulator", executable, "virtual-demo",
                    "--virtual-scenario", "load", "--advance-us", "500",
                    "--duration-us", "30000", "--load-torque-nm", str(torque),
                ], check=True, capture_output=True, text=True,
            )
            return [
                json.loads(line) for line in result.stdout.splitlines()
                if line.strip() and json.loads(line).get("kind") == "virtual"
            ]

        loaded = run(0.03)
        unloaded = run(0.0)
        comparison = foc._load_counterfactual_metrics(loaded, unloaded)
        self.assertEqual(comparison["status"], "LOAD_EFFECT_CONFIRMED")
        self.assertGreater(comparison["max_abs_delta_velocity_rad_s"], 0.01)
        self.assertGreater(comparison["max_abs_delta_iq_a"], 0.001)


if __name__ == "__main__":
    unittest.main()
