"""M0 Board1 SIL acceptance over the V1 wire.

The device runs the production Board1 boot, ADC/H0, encoder/H1, Runtime and
UART code against a virtual motor (firmware/at32f415/sil). Nothing here is
physical evidence. FOC_BOARD1_SIL names the falconfoc-board1-sil executable.
"""

import contextlib
import io
import os
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import falconfoc_cli as cli  # noqa: E402

SIL = os.environ.get("FOC_BOARD1_SIL")


def call(client, name, payload=b""):
    return cli.describe(client.call(name, payload))


def start_current(client, reference):
    """ARM then START, waiting for each lifecycle step to apply at H0."""
    payload = cli.setpoint("current", 0.0, reference)
    for action, state in (("arm", 2), ("start", 3)):
        for _ in range(200):
            call(client, "setpoint", payload)
            if call(client, action)["result"] not in ("ACCEPTED", "BUSY"):
                raise AssertionError(f"{action} rejected")
            call(client, "setpoint", payload)
            if call(client, "status")["state"] == state:
                break
        else:
            raise AssertionError(f"{action} never applied")
    return payload


@unittest.skipUnless(SIL, "FOC_BOARD1_SIL is not set")
class Board1SilTest(unittest.TestCase):
    def scenario(self, name, *arguments):
        with cli.SimulatorTransport(SIL, ["--lockstep-us", "500"]) as transport,                 contextlib.redirect_stdout(io.StringIO()):
            client = cli.Client(transport, sequence=1)
            client.hello()
            return cli.run_sil_demo(client, transport, *arguments, scenario=name)

    def test_current_demo_lockstep(self):
        """UART SETPOINT -> production Board1 path -> virtual motor -> UART readback."""
        summary = self.scenario("current", 0.4, 20000)
        self.assertEqual(summary["status"], "SIL_PASS", summary)
        metrics = summary["metrics"]
        self.assertLess(metrics["tail_mean_abs_error"], 0.01)
        self.assertLess(metrics["max_plant_firmware_delta"], 0.005)
        self.assertGreater(metrics["adc_h0_consumed"], 50)
        self.assertGreater(metrics["board_applied_updates"], 50)
        self.assertEqual(summary["boot"],
                         {"boot-configuration": 1, "calibration-input": 1,
                          "measurement-qualified": 0})
        self.assertFalse(summary["physical_applied"])

    def test_velocity_mode_over_uart(self):
        """SET_MODE VELOCITY, its H0 receipt, then speed tracking."""
        summary = self.scenario("velocity")
        self.assertEqual(summary["status"], "SIL_PASS", summary)
        self.assertTrue(summary["checks"]["set_mode_applied"])
        self.assertLess(abs(summary["metrics"]["final_value"] - 5.0), 0.5)

    def test_position_mode_over_uart(self):
        summary = self.scenario("position")
        self.assertEqual(summary["status"], "SIL_PASS", summary)
        self.assertLess(abs(summary["metrics"]["final_value"] - 2.0), 0.05)

    def test_controller_tuning_over_uart(self):
        """Writes apply only while DISABLED, through H0, with a revision receipt."""
        summary = self.scenario("tuning")
        self.assertEqual(summary["status"], "SIL_PASS", summary)
        self.assertEqual(summary["metrics"]["tuning_revision"], 3)

    def test_fault_clear_and_rerun(self):
        """Lease expiry latches COMMAND_TIMEOUT; FAULT_CLEAR returns to DISABLED; it runs again."""
        summary = self.scenario("recover")
        self.assertEqual(summary["status"], "SIL_PASS", summary)
        self.assertEqual(summary["metrics"]["fault_bits"], 1)

    def test_on_mcu_calibration_of_an_unknown_motor(self):
        """CAL-1..5 on the MCU find the plant's hidden ADC zero, pole pairs, offset, Rs,
        Ld/Lq and flux and design the current PI; before, velocity control fails on that
        motor, after, it tracks. STOP aborts. The electrical stages rerun alone."""
        with cli.SimulatorTransport(SIL, ["--lockstep-us", "500"] + cli.SIL_CALIBRATE_ARGUMENTS)                 as transport, contextlib.redirect_stdout(io.StringIO()):
            client = cli.Client(transport, sequence=1)
            client.hello()
            summary = cli.run_sil_calibrate(client, transport)
        self.assertEqual(summary["status"], "SIL_PASS", summary["failed_checks"])
        calibration = summary["metrics"]["calibration"]
        self.assertEqual(calibration["pole_pairs"], 11)
        self.assertLess(summary["metrics"]["offset_error_rad"], 0.05)
        self.assertGreater(summary["metrics"]["uncalibrated_velocity_tail_error"], 1.0)
        # CAL-3/4/5 on the MCU: Rs, Ld/Lq, flux and Kt of the hidden plant, PI applied.
        errors = summary["metrics"]["electrical_relative_error"]
        self.assertLess(errors["rs"], 0.05)
        self.assertLess(errors["flux"], 0.05)
        self.assertTrue(summary["checks"]["pi_applied_to_live_firmware"])
        self.assertTrue(summary["checks"]["electrical_only_rerun_done"])

    def test_persistence_across_power_cycles(self):
        """Save -> new process on the same flash (power cycle) -> restored; damaged -> refused;
        erase -> boot configuration."""
        import tempfile
        with tempfile.TemporaryDirectory() as folder, contextlib.redirect_stdout(io.StringIO()):
            summary = cli.run_sil_persist(SIL, ["--lockstep-us", "500"],
                                          Path(folder) / "flash.bin")
        self.assertEqual(summary["status"], "SIL_PASS", summary)
        self.assertEqual([boot["store_state"] for boot in summary["boots"]],
                         ["EMPTY", "LOADED", "INVALID_RECORD", "EMPTY"])

    def test_free_running_host_silence_trips_command_timeout(self):
        """Time keeps moving without frames: the command timeout stops the drive."""
        with cli.SimulatorTransport(SIL, ["--speed", "0.02"]) as transport:
            client = cli.Client(transport, sequence=1)
            client.hello()
            payload = start_current(client, 0.4)
            deadline = time.monotonic() + 0.5
            while time.monotonic() < deadline:
                call(client, "setpoint", payload)
                running = call(client, "telemetry")
            self.assertEqual(running["state"], 3)
            self.assertTrue(running["current_valid"])
            time.sleep(0.5)  # about 10 ms of virtual time without any frame
            silent = call(client, "telemetry")
            time.sleep(0.1)
            sil = [m for m in transport.drain_metadata() if m.get("kind") == "sil"]
        self.assertGreater(silent["uptime_us_mod32"], running["uptime_us_mod32"] + 5000)
        self.assertEqual(silent["state"], 4)          # FAULT
        self.assertNotEqual(silent["faults"], 0)
        self.assertFalse(silent["current_valid"])
        self.assertEqual(sil[-1]["drive"], "HIGH_Z")

    def test_plant_adc_offset_is_not_known_to_firmware(self):
        """The firmware decodes with its own nominal offsets; the plant truth never leaks.
        An unmeasured phase-A offset shows up as decode error (the CAL-1 motivation)."""
        with cli.SimulatorTransport(SIL, ["--lockstep-us", "500", "--adc-offset", "2078,2048,2048"]) \
                as transport:
            client = cli.Client(transport, sequence=1)
            client.hello()
            offset_a = call(client, "param-read", cli.parameter_read(38))["value"]
            payload = start_current(client, 0.4)
            for _ in range(20):
                call(client, "setpoint", payload)
                call(client, "telemetry")
            call(client, "stop")
            time.sleep(0.1)
            sil = [m for m in transport.drain_metadata()
                   if m.get("kind") == "sil" and m["firmware_current_valid"]]
        self.assertEqual(offset_a, 2048.0)
        delta = max(abs(m["sample_plant_iq_a"] - m["firmware_iq_a"]) +
                    abs(m["sample_plant_id_a"] - m["firmware_id_a"]) for m in sil)
        self.assertGreater(delta, 0.02)


if __name__ == "__main__":
    unittest.main(verbosity=2)
