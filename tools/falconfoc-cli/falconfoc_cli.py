"""FalconFOC V1 wire codec, JSON CLI and optional serial connection."""

import argparse
import binascii
import csv
import dataclasses
import json
import math
import os
import pathlib
import queue
import secrets
import struct
import subprocess
import sys
import threading
import time


MESSAGES = {
    "hello": 1, "capability": 2, "status": 3, "setpoint": 0x10,
    "stop": 0x11, "arm": 0x12, "start": 0x13, "fault": 0x20,
    "clear": 0x21, "param-read": 0x30, "param-write": 0x31,
    "save": 0x32, "telemetry": 0x40, "stream": 0x41,
}
RESULTS = [
    "OK", "ACCEPTED", "APPLIED", "BAD_FRAME", "BAD_CRC",
    "UNSUPPORTED_VERSION", "UNKNOWN_COMMAND", "INVALID_ARGUMENT", "BAD_SESSION",
    "REPLAY", "BUSY", "BAD_STATE", "OUTPUT_LOCKED", "NOT_SUPPORTED", "INTERNAL_ERROR",
]
WIRE_MAX = 116
HEADER = struct.Struct("<2sBBBBIIH")


@dataclasses.dataclass(frozen=True)
class Frame:
    type: int
    flags: int
    sequence: int
    session: int
    payload: bytes = b""


def crc16(data):
    return binascii.crc_hqx(data, 0xFFFF)


def cobs_encode(data):
    result = bytearray([0])
    start = 0
    code = 1
    for byte in data:
        if byte == 0:
            result[start] = code
            start = len(result)
            result.append(0)
            code = 1
        else:
            result.append(byte)
            code += 1
            if code == 255:
                result[start] = code
                start = len(result)
                result.append(0)
                code = 1
    result[start] = code
    return bytes(result)


def cobs_decode(data):
    result = bytearray()
    position = 0
    while position < len(data):
        code = data[position]
        end = position + code
        if code == 0 or end > len(data) or 0 in data[position + 1:end]:
            raise ValueError("BAD_FRAME")
        result.extend(data[position + 1:end])
        position = end
        if code < 255 and position < len(data):
            result.append(0)
    return bytes(result)


def encode(frame):
    if frame.flags not in (1, 2, 4) or len(frame.payload) > 96:
        raise ValueError("BAD_FRAME")
    raw = HEADER.pack(b"FF", 1, frame.type, frame.flags, 0,
                      frame.sequence, frame.session, len(frame.payload)) + frame.payload
    return cobs_encode(raw + struct.pack("<H", crc16(raw))) + b"\0"


def decode(wire):
    if not wire or len(wire) >= WIRE_MAX:
        raise ValueError("BAD_FRAME")
    raw = cobs_decode(wire)
    if len(raw) < 18 or len(raw) > 114:
        raise ValueError("BAD_FRAME")
    if crc16(raw[:-2]) != struct.unpack("<H", raw[-2:])[0]:
        raise ValueError("BAD_CRC")
    magic, version, kind, flags, reserved, sequence, session, length = HEADER.unpack(raw[:16])
    if magic != b"FF" or reserved or flags not in (1, 2, 4) or length > 96 or length != len(raw) - 18:
        raise ValueError("BAD_FRAME")
    if version != 1:
        raise ValueError("UNSUPPORTED_VERSION")
    if kind not in MESSAGES.values():
        raise ValueError("UNKNOWN_COMMAND")
    return Frame(kind, flags, sequence, session, raw[16:-2])


class Stream:
    def __init__(self):
        self.data = bytearray()
        self.dropping = False
        self.errors = 0

    def feed(self, data):
        frames = []
        for byte in data:
            if byte:
                if len(self.data) == WIRE_MAX - 1:
                    self.dropping = True
                elif not self.dropping:
                    self.data.append(byte)
            else:
                try:
                    if self.dropping:
                        raise ValueError("BAD_FRAME")
                    if self.data:
                        frames.append(decode(self.data))
                except ValueError:
                    self.errors += 1
                self.data.clear()
                self.dropping = False
        return frames


def parameter_read(identifier):
    return struct.pack("<H", identifier)


def parameter_write(identifier, kind, value):
    if kind == "u32":
        integer = int(value)
        if integer != value:
            raise ValueError("u32 parameter requires an integer")
        return struct.pack("<HBI", identifier, 1, integer)
    if not math.isfinite(value):
        raise ValueError("Parameter must be finite")
    return struct.pack("<HBf", identifier, 2, value)


def setpoint(mode, first, second=0.0):
    if not math.isfinite(first) or not math.isfinite(second):
        raise ValueError("Setpoint must be finite")
    modes = {"current": 0, "velocity": 1, "position": 2}
    return struct.pack("<Bff", modes[mode], first, second) if mode == "current" else (
        struct.pack("<Bf", modes[mode], first)
    )


def describe(frame):
    result = {
        "message": next((name for name, value in MESSAGES.items() if value == frame.type), frame.type),
        "sequence": frame.sequence, "session": frame.session, "flags": frame.flags,
        "payload_hex": frame.payload.hex(),
    }
    if frame.flags != 2:
        return result
    body = frame.payload
    if not body or body[0] >= len(RESULTS):
        raise ValueError("BAD_RESPONSE")
    result["result"] = RESULTS[body[0]]
    if body[0] > 2:
        if len(body) != 1:
            raise ValueError("BAD_RESPONSE")
        return result
    if frame.type in (1, 2) and body[0] == 0:
        if len(body) != 9:
            raise ValueError("BAD_RESPONSE")
        result.update(protocol=body[1], firmware=".".join(map(str, body[2:5])),
                      algorithm_modes=body[5], physical_modes=body[6],
                      output_locked=bool(body[7] & 1), capabilities=int.from_bytes(body[7:9], "little"))
    elif frame.type in (3, 0x20, 0x40) and body[0] == 0:
        if len(body) != (63 if frame.type == 0x40 else 30):
            raise ValueError("BAD_RESPONSE")
        flags = body[3]
        result.update(
            state=body[1], mode=body[2], output_locked=bool(flags & 1),
            software_output_acknowledged=bool(flags & 2), sensor_valid=bool(flags & 4),
            command_observed=bool(flags & 8), inhibit_pending=bool(flags & 16),
            dirty=bool(flags & 32), fast_seen=bool(flags & 64), sample_seen=bool(flags & 128),
            action=body[4], faults=struct.unpack_from("<I", body, 5)[0],
            uptime_us_mod32=struct.unpack_from("<I", body, 9)[0],
            pipeline_result=struct.unpack_from("<i", body, 13)[0],
            clear_result=body[17], sample_us=struct.unpack_from("<I", body, 18)[0],
            command_us=struct.unpack_from("<I", body, 22)[0],
            publication_sequence=struct.unpack_from("<I", body, 26)[0], physical_applied=False,
        )
        if frame.type == 0x40:
            result.update(
                id_a=struct.unpack_from("<f", body, 30)[0], iq_a=struct.unpack_from("<f", body, 34)[0],
                position_rad=struct.unpack_from("<f", body, 38)[0],
                velocity_rad_s=struct.unpack_from("<f", body, 42)[0],
                sensor_us=struct.unpack_from("<I", body, 46)[0],
                crc_errors=struct.unpack_from("<I", body, 50)[0],
                dropped_frames=struct.unpack_from("<I", body, 54)[0],
                rx_loss_events=struct.unpack_from("<I", body, 58)[0],
                current_valid=bool(body[62] & 2), physical_applied=bool(body[62] & 1),
                sensor_calibrated=bool(body[62] & 4),
            )
    elif frame.type in (0x30, 0x31) and body[0] in (0, 2):
        if len(body) != 18 or body[3] not in (1, 2):
            raise ValueError("BAD_RESPONSE")
        kind = "<I" if body[3] == 1 else "<f"
        result.update(parameter_id=struct.unpack_from("<H", body, 1)[0],
                      parameter_type="u32" if body[3] == 1 else "f32", unit=body[4],
                      writable=bool(body[5] & 1), applied=bool(body[5] & 2),
                      calibration_required=bool(body[5] & 4))
        for name, offset in (("minimum", 6), ("maximum", 10), ("value", 14)):
            result[name] = struct.unpack_from(kind, body, offset)[0]
    elif frame.type == 0x10 and body[0] == 1:
        if len(body) != 5:
            raise ValueError("BAD_RESPONSE")
        result["publication_sequence"] = struct.unpack_from("<I", body, 1)[0]
    elif len(body) != 1:
        raise ValueError("BAD_RESPONSE")
    return result


class SimulatorTransport:
    """Run the Host C device with binary pipes and bounded receive waiting."""

    def __init__(self, executable, arguments=None):
        command = [executable] + ([] if arguments is None else list(arguments))
        self.process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        self.received = queue.Queue(maxsize=512)
        self.metadata = queue.Queue(maxsize=512)
        self.reader = threading.Thread(target=self._receive, daemon=True)
        self.metadata_reader = threading.Thread(target=self._receive_metadata, daemon=True)
        self.reader.start()
        self.metadata_reader.start()

    def _receive(self):
        while True:
            byte = self.process.stdout.read(1)
            self.received.put(byte)
            if not byte:
                return

    def _receive_metadata(self):
        while True:
            line = self.process.stderr.readline()
            if not line:
                return
            text = line.decode("utf-8", errors="replace").strip()
            prefix = "FALCONF0C_META "
            if text.startswith(prefix):
                try:
                    self.metadata.put_nowait(json.loads(text[len(prefix):]))
                except (ValueError, queue.Full):
                    pass

    def write(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def read(self):
        try:
            return self.received.get(timeout=1.0)
        except queue.Empty as error:
            raise TimeoutError("Device response timed out") from error

    def drain_metadata(self):
        items = []
        while True:
            try:
                items.append(self.metadata.get_nowait())
            except queue.Empty:
                return items

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=1)
        self.reader.join(timeout=1)
        self.metadata_reader.join(timeout=1)
        self.process.stdout.close()
        self.process.stderr.close()


class SerialTransport:
    """Optional pyserial adapter. Import is deferred until a port is selected."""

    def __init__(self, port):
        import serial
        self.connection = serial.Serial(port, baudrate=115200, bytesize=8,
                                        parity="N", stopbits=1, timeout=1, write_timeout=1)

    def write(self, data):
        if self.connection.write(data) != len(data):
            raise OSError("Incomplete serial write")

    def read(self):
        return self.connection.read(1)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.connection.close()


class Client:
    def __init__(self, transport, sequence=None):
        self.transport = transport
        self.sequence = secrets.randbits(32) if sequence is None else sequence
        self.session = 0
        self.stream = Stream()

    def call(self, name, payload=b""):
        sequence = self.sequence
        self.sequence = (sequence + 1) & 0xFFFFFFFF
        request = Frame(MESSAGES[name], 1, sequence, self.session, payload)
        self.transport.write(encode(request))
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            byte = self.transport.read()
            if not byte:
                raise TimeoutError("Device closed or timed out")
            for response in self.stream.feed(byte):
                if (response.flags == 2 and response.sequence == sequence and response.type == request.type
                        and (name == "hello" or response.session == self.session)):
                    describe(response)
                    return response
        raise TimeoutError("No matching response")

    def hello(self):
        self.session = 0
        response = self.call("hello")
        if response.payload[0] != 0 or response.session == 0:
            raise ValueError("HELLO failed")
        self.session = response.session
        return response


def _virtual_metrics(records, scenario, duration_us, timeline=None):
    timeline = timeline or {}
    virtual = [record for record in records if record.get("kind") == "virtual"]
    if not virtual:
        return {"status": "NOT_MEASURED", "reason": "no virtual telemetry records"}
    mode = virtual[-1].get("control_mode", virtual[-1].get("mode", 0))
    if mode == 0:
        actual_key, target_key, tolerance, unit = "iq_actual_a", "iq_target_a", 0.1, "A"
    elif mode == 1:
        actual_key, target_key, tolerance, unit = (
            "velocity_actual_rad_s", "velocity_target_rad_s", 0.5, "rad/s"
        )
    else:
        actual_key, target_key, tolerance, unit = (
            "position_actual_rad", "position_target_rad", 0.05, "rad"
        )

    def errors_for(window):
        return [
            float(record.get(target_key, 0.0)) - float(record.get(actual_key, 0.0))
            for record in window
        ]

    def rmse(errors):
        return math.sqrt(sum(error * error for error in errors) / len(errors)) if errors else None

    def settling(window, errors):
        for index in range(len(errors)):
            if all(abs(candidate) <= tolerance for candidate in errors[index:]):
                return window[index].get("virtual_time_us")
        return None

    def tracking_error(window, errors):
        outside = [record.get("virtual_time_us") for record, error in zip(window, errors)
                   if abs(error) > tolerance]
        return {
            "final": abs(errors[-1]) if errors else None,
            "peak": max(map(abs, errors), default=None),
            "out_of_tolerance_samples": len(outside),
            "first_out_of_tolerance_us": outside[0] if outside else None,
            "last_out_of_tolerance_us": outside[-1] if outside else None,
        }

    full_errors = errors_for(virtual)
    running = [
        record for record in virtual
        if record.get("state") == 3 and record.get("output_action") == 1
    ]
    running_errors = errors_for(running)
    tail_start = len(running) * 3 // 4
    running_tail = running[tail_start:] or running
    running_tail_errors = errors_for(running_tail)
    full_tail_errors = full_errors[len(full_errors) * 3 // 4:] or full_errors
    full_settling_us = settling(virtual, full_errors)
    running_settling_us = settling(running, running_errors)
    final_running = running[-1] if running else virtual[-1]
    reference = float(virtual[-1].get(target_key, 0.0))
    actual = [float(record.get(actual_key, 0.0)) for record in virtual]
    overshoot = max(
        [
            abs(value - reference)
            for value in actual
            if (reference >= 0.0 and value > reference) or
            (reference < 0.0 and value < reference)
        ]
        or [0.0]
    )
    saturation_count = max(record.get("saturation_count", 0) for record in virtual)
    integration_pass = bool(
        running and max(record.get("virtual_update_accepted", 0) for record in running) > 0
    )
    quality_pass = bool(full_errors and abs(full_errors[-1]) <= tolerance and full_settling_us is not None)
    if scenario == "fault":
        quality_status = "NOT_MEASURED"
        quality_reason = "fault termination ends the tracking window before quality acceptance"
    elif quality_pass:
        quality_status = "CONTROL_QUALITY_PASS"
        quality_reason = "fixed research tolerance met"
    elif scenario in {"velocity", "position", "load"}:
        quality_status = "EXPECTED_DEGRADED"
        quality_reason = (
            "VM-01b research profile and finite voltage/current envelope are retained; "
            "this is not a product-quality pass"
        )
    else:
        quality_status = "CONTROL_QUALITY_FAIL"
        quality_reason = "fixed research tolerance was not met"
    metrics = {
        "metrics_schema_version": 2,
        "status": "INTEGRATION_PASS" if integration_pass else "INTEGRATION_FAIL",
        "control_quality": quality_status,
        "quality_reason": quality_reason,
        "mode": mode,
        "unit": unit,
        "duration_us": duration_us,
        "rmse": rmse(full_errors),
        "full_window_rmse": rmse(full_errors),
        "tail_rmse": rmse(full_tail_errors),
        "running_window_rmse": rmse(running_errors),
        "running_tail_rmse": rmse(running_tail_errors),
        "transient_peak_error": max((abs(error) for error in running_errors[:tail_start]), default=0.0),
        "steady_state_error": (
            sum(abs(error) for error in running_tail_errors) / len(running_tail_errors)
            if running_tail_errors else None
        ),
        "peak_error": max(abs(error) for error in full_errors),
        "final_error": abs(full_errors[-1]),
        "overshoot": overshoot,
        "settling_us": full_settling_us,
        "settling_time_us": running_settling_us,
        "tolerance": tolerance,
        "saturation_count": saturation_count,
        "saturation_duration_us": saturation_count * 200,
        "update_accepted": final_running.get("virtual_update_accepted", 0),
        "update_rejected": final_running.get("virtual_update_rejected", 0),
        "final_state": virtual[-1].get("state"),
        "final_faults": virtual[-1].get("faults", 0),
        "final_replay_hash": virtual[-1].get("replay_hash"),
        "final_position_rad": final_running.get("position_actual_rad"),
        "final_velocity_rad_s": final_running.get("velocity_actual_rad_s"),
    }
    metrics["measurement_window"] = {
        "full": {
            "start_us": virtual[0].get("virtual_time_us"),
            "end_us": virtual[-1].get("virtual_time_us"),
            "sample_count": len(virtual),
            "sample_rule": "all virtual records; historical metric retained",
            "tracking_error": tracking_error(virtual, full_errors),
        },
        "running": {
            "start_us": running[0].get("virtual_time_us") if running else None,
            "end_us": running[-1].get("virtual_time_us") if running else None,
            "sample_count": len(running),
            "sample_rule": "state=RUNNING and output_action=UPDATE",
            "tracking_error": tracking_error(running, running_errors),
        },
        "running_tail": {
            "start_us": running_tail[0].get("virtual_time_us") if running_tail else None,
            "end_us": running_tail[-1].get("virtual_time_us") if running_tail else None,
            "sample_count": len(running_tail),
            "sample_rule": "last 25 percent of valid RUNNING samples",
        },
    }
    if scenario == "load":
        load_on = timeline.get("load_on_us", 0)
        load_off = timeline.get("load_off_us", 0)
        before = [record for record in virtual if record.get("virtual_time_us", 0) <= load_on]
        disturbed = [
            record for record in virtual
            if load_on <= record.get("virtual_time_us", 0) <= load_off
        ]
        recovery = [record for record in virtual if record.get("virtual_time_us", 0) >= load_off]
        baseline_speed = before[-1].get("velocity_actual_rad_s") if before else None
        minimum_speed = min(
            [record.get("velocity_actual_rad_s", 0.0) for record in disturbed]
            or [baseline_speed if baseline_speed is not None else 0.0]
        )
        recovery_time = None
        if baseline_speed is not None:
            for record in recovery:
                if abs(record.get("velocity_actual_rad_s", 0.0) - baseline_speed) <= tolerance:
                    recovery_time = record.get("virtual_time_us") - load_off
                    break
        metrics.update({
            "load_on_us": load_on,
            "load_off_us": load_off,
            "load_torque_nm": timeline.get("load_torque_nm", 0.0),
            "speed_before_load": baseline_speed,
            "speed_min_after_load": minimum_speed,
            "speed_drop": None if baseline_speed is None else baseline_speed - minimum_speed,
            "speed_recovery_us": recovery_time,
        })
    if scenario == "position":
        positions = [record.get("position_actual_rad", 0.0) for record in virtual]
        metrics.update({
            "max_position_rad": max(positions),
            "crossed_single_turn": max(positions) >= 2.0 * math.pi,
        })
    if scenario == "fault":
        def first_event(name):
            values = [record.get(name) for record in virtual if record.get(name, -1) >= 0]
            return values[0] if values else None

        def event_record(name):
            return next((record for record in virtual if record.get(name, -1) >= 0), None)

        def last_event_value(name):
            values = [record.get(name) for record in virtual if record.get(name, -1) >= 0]
            return values[-1] if values else None

        fault_time = first_event("fault_injection_us")
        sensor_failure = first_event("sensor_failure_observed_us")
        h1_invalid = first_event("h1_invalid_observation_us")
        h0_failure = first_event("h0_failure_observed_us")
        inhibit_requested = first_event("inhibit_requested_us")
        inhibit_accepted = first_event("inhibit_accepted_us")
        fault_latched = first_event("fault_latched_us")
        final_updates = virtual[-1].get("virtual_update_accepted", 0)
        last_update = last_event_value("last_accepted_update_us")

        def event_count(name, marker):
            record = event_record(marker)
            return 0 if record is None else record.get(name, 0)

        updates_at_injection = event_count("updates_at_injection", "fault_injection_us")
        updates_at_sensor_failure = event_count("updates_at_sensor_failure", "sensor_failure_observed_us")
        updates_at_h1_invalid = event_count("updates_at_h1_invalid", "h1_invalid_observation_us")
        updates_at_h0_failure = event_count("updates_at_h0_failure", "h0_failure_observed_us")
        updates_at_inhibit_requested = event_count("updates_at_inhibit_requested", "inhibit_requested_us")
        updates_at_inhibit = event_count("updates_at_inhibit_accepted", "inhibit_accepted_us")
        updates_at_fault_latched = event_count("updates_at_fault_latched", "fault_latched_us")
        metrics.update({
            "fault_injection_time_us": fault_time,
            "sensor_failure_observed_us": sensor_failure,
            "h1_invalid_observation_us": h1_invalid,
            "h0_failure_observed_us": h0_failure,
            "inhibit_requested_us": inhibit_requested,
            "inhibit_accepted_us": inhibit_accepted,
            "fault_latched_us": fault_latched,
            "last_accepted_update_us": last_update,
            "software_inhibit_latency_us": (
                None if fault_time is None or inhibit_accepted is None else inhibit_accepted - fault_time
            ),
            "fault_latch_latency_us": (
                None if fault_time is None or fault_latched is None else fault_latched - fault_time
            ),
            "sensor_fault_us": fault_time,
            "fault_response_us": (
                None if fault_time is None or inhibit_accepted is None else inhibit_accepted - fault_time
            ),
            "updates_before_fault": updates_at_injection,
            "updates_at_sensor_failure": updates_at_sensor_failure,
            "updates_at_h1_invalid": updates_at_h1_invalid,
            "updates_at_h0_failure": updates_at_h0_failure,
            "updates_at_inhibit_requested": updates_at_inhibit_requested,
            "updates_at_inhibit": updates_at_inhibit,
            "updates_at_fault_latched": updates_at_fault_latched,
            "updates_after_inhibit": final_updates - updates_at_inhibit,
            "updates_after_fault_latched": final_updates - updates_at_fault_latched,
            "no_update_after_inhibit": final_updates == updates_at_inhibit,
        })
    return metrics


def _load_counterfactual_metrics(loaded_records, unloaded_records):
    """Compare two same-timeline Plant traces without reusing controller outputs."""
    loaded = {
        record.get("virtual_time_us"): record
        for record in loaded_records
        if record.get("kind") == "virtual"
    }
    unloaded = {
        record.get("virtual_time_us"): record
        for record in unloaded_records
        if record.get("kind") == "virtual"
    }
    times = sorted(set(loaded).intersection(unloaded))
    if not times:
        return {"status": "NOT_MEASURED", "sample_count": 0}
    velocity = [
        abs(float(loaded[t].get("velocity_actual_rad_s", 0.0)) -
            float(unloaded[t].get("velocity_actual_rad_s", 0.0)))
        for t in times
    ]
    position = [
        abs(float(loaded[t].get("position_actual_rad", 0.0)) -
            float(unloaded[t].get("position_actual_rad", 0.0)))
        for t in times
    ]
    iq = [
        abs(float(loaded[t].get("iq_actual_a", 0.0)) -
            float(unloaded[t].get("iq_actual_a", 0.0)))
        for t in times
    ]
    return {
        "status": "LOAD_EFFECT_CONFIRMED" if max(velocity + position + iq) > 1.0e-4
        else "LOAD_EFFECT_NOT_DEMONSTRATED",
        "sample_count": len(times),
        "start_us": times[0],
        "end_us": times[-1],
        "max_abs_delta_velocity_rad_s": max(velocity),
        "max_abs_delta_position_rad": max(position),
        "max_abs_delta_iq_a": max(iq),
        "mean_abs_delta_velocity_rad_s": sum(velocity) / len(velocity),
        "mean_abs_delta_position_rad": sum(position) / len(position),
        "mean_abs_delta_iq_a": sum(iq) / len(iq),
    }


def _write_virtual_outputs(records, summary, csv_path=None, json_path=None):
    virtual = [record for record in records if record.get("kind") == "virtual"]
    fields = [
        "profile_id", "virtual_time_us", "control_mode", "state", "command_sequence", "motion_session",
        "iq_target_a", "id_actual_a", "iq_actual_a", "velocity_target_rad_s",
        "velocity_actual_rad_s", "position_target_rad", "position_actual_rad", "vbus_v",
        "voltage_saturated", "virtual_update_accepted", "virtual_update_rejected",
        "faults", "output_action", "load_torque_nm", "fault_injection_us",
        "sensor_failure_observed_us", "h1_invalid_observation_us", "h0_failure_observed_us",
        "inhibit_requested_us", "inhibit_accepted_us", "fault_latched_us",
    ]
    if csv_path:
        with open(csv_path, "w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            for record in virtual:
                writer.writerow({field: record.get(field) for field in fields})
    if json_path:
        pathlib.Path(json_path).write_text(
            json.dumps(summary, indent=2, sort_keys=True, allow_nan=False) + "\n",
            encoding="utf-8",
        )


def run_virtual_demo(
    client,
    transport,
    scenario,
    duration_us,
    advance_us,
    csv_path=None,
    json_path=None,
    timeline=None,
    horizon="short",
    profile_id="GENERIC_RESEARCH_MOTOR",
    reference=None,
    research_parameters=None,
):
    """Run one same-process Host Virtual workflow using the existing V1 messages."""

    records = []

    def call(name, payload=b""):
        response = client.call(name, payload)
        print(json.dumps(describe(response), allow_nan=False))
        for metadata in transport.drain_metadata():
            records.append(metadata)
            print(json.dumps(metadata, allow_nan=False))
        return response

    call("capability")
    if reference is None:
        reference = {"current": 0.4, "velocity": 3.0, "position": 7.0,
                     "load": 3.0, "fault": 3.0}[scenario]
    if scenario == "current":
        payload = setpoint("current", 0.0, reference)
    elif scenario in {"velocity", "load", "fault"}:
        payload = setpoint("velocity", reference)
    else:
        payload = setpoint("position", reference)

    # The Runtime requires a fresh target before ARM/START. Refresh it after
    # ARM as well so the command cannot expire while the mailbox is consumed.
    call("setpoint", payload)
    call("arm")
    call("setpoint", payload)
    call("start")
    elapsed = 0
    while elapsed < duration_us:
        call("setpoint", payload)
        call("telemetry")
        elapsed += 2 * advance_us
    call("stop")
    call("status")
    call("telemetry")
    time.sleep(0.05)
    for metadata in transport.drain_metadata():
        records.append(metadata)
        print(json.dumps(metadata, allow_nan=False))
    summary = {
        "scenario": scenario,
        "horizon": horizon,
        "profile_id": profile_id,
        "research_parameters": research_parameters or {},
        "simulated": True,
        "physical_applied": False,
        "metrics": _virtual_metrics(records, scenario, duration_us, timeline),
        "records": len(records),
        "timeline": timeline or {},
    }
    _write_virtual_outputs(records, summary, csv_path, json_path)
    print(json.dumps({"kind": "summary", **summary}, allow_nan=False))
    return summary


def calibrate_host(args, parser):
    if not args.simulator or args.port or args.message:
        parser.error("calibrate requires the Host falconfoc-virtual-calibrate --simulator")
    if args.load_config and (args.save_config or args.cancel_after or args.cogging or args.trace):
        parser.error("--load-config cannot be combined with --save-config or capture options")
    if args.save_config and not args.approve_candidate:
        parser.error("--save-config requires --approve-candidate")
    reports = [pathlib.Path(path).resolve() for path in
               (args.json, args.save_config, args.load_config, args.trace) if path]
    if len(reports) != len(set(reports)):
        parser.error("report and configuration paths must be distinct")
    command = [str(pathlib.Path(args.simulator).resolve()), "--profile-id", args.profile_id]
    if args.approve_candidate:
        command.extend(("--approve-candidate", args.approve_candidate))
    if args.cogging:
        command.append("--cogging")
    for key, value in (("--save-config", args.save_config), ("--load-config", args.load_config),
                       ("--cancel-after", args.cancel_after), ("--trace", args.trace)):
        if value is not None:
            command.extend((key, value))
    process = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
    events = [json.loads(line) for line in process.stdout.splitlines()]
    for event in events:
        print(json.dumps(event, allow_nan=False))
    if process.stderr:
        print(process.stderr, file=sys.stderr, end="")
    if args.json:
        with pathlib.Path(args.json).open("x", encoding="utf-8") as report:
            json.dump({"events": events, "exit_status": process.returncode},
                      report, indent=2, allow_nan=False)
            report.write("\n")
    return process.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    connection = parser.add_mutually_exclusive_group()
    connection.add_argument("--port", help="Optional pyserial port. Connects when selected.")
    connection.add_argument("--simulator", help="Host foc-serial-sim executable")
    parser.add_argument(
        "action", choices=["encode", "decode", "vectors", "request", "demo", "virtual-demo", "calibrate"]
    )
    parser.add_argument("message", nargs="?", choices=sorted(MESSAGES))
    parser.add_argument("--hex", default="", help="Wire hex for decode, including delimiter")
    parser.add_argument("--sequence", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--session", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--mode", choices=["current", "velocity", "position"], default="current")
    parser.add_argument("--first", type=float, default=0)
    parser.add_argument("--second", type=float, default=0)
    parser.add_argument("--id", type=int, default=1)
    parser.add_argument("--type", choices=["u32", "f32"], default="u32")
    parser.add_argument("--value", type=float, default=0)
    parser.add_argument(
        "--virtual-scenario",
        choices=["current", "velocity", "position", "load", "fault"],
        default="current",
    )
    parser.add_argument("--advance-us", type=int, default=500)
    parser.add_argument("--duration-us", type=int, default=None)
    parser.add_argument("--horizon", choices=["short", "long"], default="short")
    parser.add_argument("--profile-id", default="GENERIC_RESEARCH_MOTOR")
    parser.add_argument("--reference", type=float, default=None)
    parser.add_argument("--current-kp", type=float, default=None)
    parser.add_argument("--current-ki", type=float, default=None)
    parser.add_argument("--velocity-kp", type=float, default=None)
    parser.add_argument("--velocity-ki", type=float, default=None)
    parser.add_argument("--position-kp", type=float, default=None)
    parser.add_argument("--current-limit-a", type=float, default=None)
    parser.add_argument("--voltage-limit-v", type=float, default=None)
    parser.add_argument("--vbus-v", type=float, default=None)
    parser.add_argument("--motor-rs-ohm", type=float, default=None)
    parser.add_argument("--motor-ld-h", type=float, default=None)
    parser.add_argument("--motor-lq-h", type=float, default=None)
    parser.add_argument("--motor-psi-f-wb", type=float, default=None)
    parser.add_argument("--motor-inertia", type=float, default=None)
    parser.add_argument("--motor-damping", type=float, default=None)
    parser.add_argument("--csv", default=None, help="Virtual demo CSV output path")
    parser.add_argument("--json", default=None, help="Virtual demo JSON summary path")
    parser.add_argument("--load-on-us", type=int, default=5000)
    parser.add_argument("--load-off-us", type=int, default=15000)
    parser.add_argument("--load-torque-nm", type=float, default=0.03)
    parser.add_argument("--sensor-fault-us", type=int, default=5000)
    parser.add_argument("--approve-candidate", help="Exact review_id returned by the Host candidate preview")
    parser.add_argument("--save-config", help="Save a qualified Host calibration package")
    parser.add_argument("--load-config", help="Restore an approved Host calibration package")
    parser.add_argument("--cancel-after", help="Cancel a virtual calibration after the named stage")
    parser.add_argument("--cogging", action="store_true", help="Capture and apply a virtual cogging LUT")
    parser.add_argument("--trace", help="Export the d/q terminal voltage and ADC aperture CSV")
    args = parser.parse_args()
    try:
        if args.action == "calibrate":
            return calibrate_host(args, parser)
        payload = b""
        if args.message == "setpoint":
            payload = setpoint(args.mode, args.first, args.second)
        elif args.message == "param-read":
            payload = parameter_read(args.id)
        elif args.message == "param-write":
            payload = parameter_write(args.id, args.type, args.value)
        if args.action == "encode":
            if args.message is None:
                parser.error("encode requires a message")
            print(encode(Frame(MESSAGES[args.message], 1, args.sequence, args.session, payload)).hex())
        elif args.action == "decode":
            stream = Stream()
            frames = stream.feed(bytes.fromhex(args.hex))
            if stream.errors or stream.data or stream.dropping or not frames:
                raise ValueError("Invalid or incomplete wire input")
            for frame in frames:
                print(json.dumps(describe(frame), allow_nan=False))
        elif args.action == "vectors":
            path = pathlib.Path(__file__).resolve().parents[2] / "protocol/tests/vectors.tsv"
            for line in path.read_text().splitlines():
                kind, flags, seq, session, body, _ = line.split()
                frame = Frame(int(kind, 16), int(flags, 16), int(seq, 16), int(session, 16),
                              bytes.fromhex(body) if body != "-" else b"")
                print(encode(frame).hex())
        else:
            if not args.port and not args.simulator:
                parser.error("request/demo/virtual-demo requires --simulator or --port")
            if args.action == "request" and args.message is None:
                parser.error("request requires a message")
            if args.action == "virtual-demo" and not args.simulator:
                parser.error("virtual-demo requires --simulator")
            simulator_arguments = (
                [
                    "--mode",
                    "velocity" if args.virtual_scenario in {"load", "fault"}
                    else args.virtual_scenario,
                    "--advance-us",
                    str(args.advance_us),
                ]
                if args.action == "virtual-demo"
                else None
            )
            if args.action == "virtual-demo":
                simulator_arguments += ["--profile-id", args.profile_id]
                if args.reference is not None:
                    simulator_arguments += ["--reference", str(args.reference)]
                for option, value in (
                    ("--current-kp", args.current_kp),
                    ("--current-ki", args.current_ki),
                    ("--velocity-kp", args.velocity_kp),
                    ("--velocity-ki", args.velocity_ki),
                    ("--position-kp", args.position_kp),
                    ("--current-limit-a", args.current_limit_a),
                    ("--voltage-limit-v", args.voltage_limit_v),
                    ("--vbus-v", args.vbus_v),
                    ("--motor-rs-ohm", args.motor_rs_ohm),
                    ("--motor-ld-h", args.motor_ld_h),
                    ("--motor-lq-h", args.motor_lq_h),
                    ("--motor-psi-f-wb", args.motor_psi_f_wb),
                    ("--motor-inertia", args.motor_inertia),
                    ("--motor-damping", args.motor_damping),
                ):
                    if value is not None:
                        simulator_arguments += [option, str(value)]
                if args.virtual_scenario == "load":
                    simulator_arguments += [
                        "--load-on-us", str(args.load_on_us),
                        "--load-off-us", str(args.load_off_us),
                        "--load-torque-nm", str(args.load_torque_nm),
                    ]
                if args.virtual_scenario == "fault":
                    simulator_arguments += ["--sensor-fault-us", str(args.sensor_fault_us)]
            transport = (
                SimulatorTransport(args.simulator, simulator_arguments)
                if args.simulator
                else SerialTransport(args.port)
            )
            with transport:
                client = Client(transport)
                hello = client.hello()
                if args.action == "virtual-demo":
                    default_duration = {
                        "current": 10000,
                        "velocity": 100000 if args.horizon == "long" else 10000,
                        "position": 2000000 if args.horizon == "long" else 1200000,
                        "load": 30000,
                        "fault": 20000,
                    }[args.virtual_scenario]
                    run_virtual_demo(
                        client,
                        transport,
                        args.virtual_scenario,
                        args.duration_us if args.duration_us is not None else default_duration,
                        args.advance_us,
                        args.csv,
                        args.json,
                        {
                            "load_on_us": args.load_on_us,
                            "load_off_us": args.load_off_us,
                            "load_torque_nm": args.load_torque_nm,
                            "sensor_fault_us": args.sensor_fault_us,
                        },
                        args.horizon,
                        args.profile_id,
                        args.reference,
                        {
                            key: value for key, value in {
                                "current_kp": args.current_kp,
                                "current_ki": args.current_ki,
                                "velocity_kp": args.velocity_kp,
                                "velocity_ki": args.velocity_ki,
                                "position_kp": args.position_kp,
                                "current_limit_a": args.current_limit_a,
                                "voltage_limit_v": args.voltage_limit_v,
                                "vbus_v": args.vbus_v,
                                "motor_rs_ohm": args.motor_rs_ohm,
                                "motor_ld_h": args.motor_ld_h,
                                "motor_lq_h": args.motor_lq_h,
                                "motor_psi_f_wb": args.motor_psi_f_wb,
                                "motor_inertia": args.motor_inertia,
                                "motor_damping": args.motor_damping,
                            }.items() if value is not None
                        },
                    )
                elif args.action == "demo":
                    print(json.dumps(describe(hello), allow_nan=False))
                    for name in ("capability", "status", "fault", "telemetry", "arm", "save", "stop"):
                        print(json.dumps(describe(client.call(name)), allow_nan=False))
                else:
                    response = hello if args.message == "hello" else client.call(args.message, payload)
                    print(json.dumps(describe(response), allow_nan=False))
    except (ValueError, OSError, TimeoutError, OverflowError, struct.error, ImportError, subprocess.TimeoutExpired) as error:
        print(json.dumps({"error": str(error)}), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
