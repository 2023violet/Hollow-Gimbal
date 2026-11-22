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
    "stop": 0x11, "arm": 0x12, "start": 0x13, "set-mode": 0x14, "lifecycle": 0x15,
    "fault": 0x20,
    "clear": 0x21, "param-read": 0x30, "param-write": 0x31,
    "save": 0x32, "telemetry": 0x40, "stream": 0x41,
}
RESULTS = [
    "OK", "ACCEPTED", "APPLIED", "BAD_FRAME", "BAD_CRC",
    "UNSUPPORTED_VERSION", "UNKNOWN_COMMAND", "INVALID_ARGUMENT", "BAD_SESSION",
    "REPLAY", "BUSY", "BAD_STATE", "OUTPUT_LOCKED", "NOT_SUPPORTED", "INTERNAL_ERROR",
]
MODES = {"current": 0, "velocity": 1, "position": 2}
LIFECYCLE_RESULTS = ["NONE", "PENDING", "APPLIED", "REJECTED", "CANCELLED"]
LIFECYCLE_ACTIONS = ["NONE", "ARM", "START", "CLEAR_FAULT", "SET_MODE"]
# UART V1 parameter IDs by name (protocol/README.md).
PARAMETERS = {
    "rx-budget": 1, "remote-current-cap": 2, "remote-position-cap": 3,
    "pole-pairs": 16, "electrical-offset": 17, "velocity-limit-ro": 18,
    "boot-configuration": 32, "calibration-input": 33, "phase-a-offset": 38,
    "phase-b-offset": 39, "phase-c-offset": 40, "measurement-qualified": 45,
    "tuning-revision": 60, "tuning-result": 61, "tuning-sequence": 62,
    "current-kp-d": 64, "current-kp-q": 65, "current-ki-d": 66, "current-ki-q": 67,
    "current-limit": 68, "velocity-kp": 69, "velocity-ki": 70, "position-kp": 71,
    "velocity-limit": 72, "iq-limit": 73, "command-lease-us": 74,
}
PARAMETER_TYPES = {74: "u32", 1: "u32"}
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
    return struct.pack("<Bff", MODES[mode], first, second) if mode == "current" else (
        struct.pack("<Bf", MODES[mode], first)
    )


def set_mode(mode):
    return struct.pack("<B", MODES[mode])


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
        # TELEMETRY is 75 bytes since M1; 63 from earlier firmware is still decoded.
        if len(body) not in ((63, 75) if frame.type == 0x40 else (30,)):
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
            if len(body) == 75:
                result.update(
                    id_target_a=struct.unpack_from("<f", body, 63)[0],
                    iq_target_a=struct.unpack_from("<f", body, 67)[0],
                    tuning_revision=struct.unpack_from("<I", body, 71)[0],
                )
    elif frame.type in (0x30, 0x31) and body[0] in (0, 1, 2):
        if len(body) != 18 or body[3] not in (1, 2):
            raise ValueError("BAD_RESPONSE")
        kind = "<I" if body[3] == 1 else "<f"
        identifier = struct.unpack_from("<H", body, 1)[0]
        result.update(parameter_id=identifier,
                      parameter=next((name for name, value in PARAMETERS.items()
                                      if value == identifier), None),
                      parameter_type="u32" if body[3] == 1 else "f32", unit=body[4],
                      writable=bool(body[5] & 1), applied=bool(body[5] & 2),
                      calibration_required=bool(body[5] & 4))
        for name, offset in (("minimum", 6), ("maximum", 10), ("value", 14)):
            result[name] = struct.unpack_from(kind, body, offset)[0]
    elif frame.type == 0x10 and body[0] == 1:
        if len(body) != 5:
            raise ValueError("BAD_RESPONSE")
        result["publication_sequence"] = struct.unpack_from("<I", body, 1)[0]
    elif frame.type in (0x12, 0x13, 0x14) and body[0] == 1:
        # Earlier virtual targets answered ARM/START with the result byte only.
        if len(body) == 1 and frame.type != 0x14:
            return result
        if len(body) != 5:
            raise ValueError("BAD_RESPONSE")
        result["lifecycle_sequence"] = struct.unpack_from("<I", body, 1)[0]
    elif frame.type == 0x15 and body[0] == 0:
        if len(body) != 25:
            raise ValueError("BAD_RESPONSE")
        result.update(
            lifecycle_result=LIFECYCLE_RESULTS[body[1]] if body[1] < len(LIFECYCLE_RESULTS) else body[1],
            lifecycle_action=LIFECYCLE_ACTIONS[body[2]] if body[2] < len(LIFECYCLE_ACTIONS) else body[2],
            requested_mode=body[3], lifecycle_sequence=struct.unpack_from("<I", body, 4)[0],
            state=body[8], faults=struct.unpack_from("<I", body, 9)[0],
            h0_us=struct.unpack_from("<I", body, 13)[0],
            pipeline_result=struct.unpack_from("<i", body, 17)[0],
            motion_session=struct.unpack_from("<I", body, 21)[0],
        )
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


SIL_STATE_DISABLED, SIL_STATE_ARMED, SIL_STATE_RUNNING, SIL_STATE_FAULT = 1, 2, 3, 4
SIL_SCENARIOS = ("current", "velocity", "position", "tuning", "recover")
# Scenario defaults: reference, virtual duration in µs, telemetry key, plant key, firmware key,
# tail tolerance and plant/firmware agreement tolerance.
SIL_DEFAULTS = {
    "current": (0.4, 20000, "iq_a", "sample_plant_iq_a", "firmware_iq_a", 0.05, 0.02),
    "velocity": (5.0, 600000, "velocity_rad_s", "plant_velocity_rad_s", "firmware_velocity_rad_s",
                 0.3, 0.5),
    "position": (2.0, 1500000, "position_rad", "plant_position_rad", "firmware_position_rad",
                 0.05, 0.01),
}


class SilSession:
    """Host side of one Board1 SIL connection: calls, lifecycle waits and records."""

    def __init__(self, client, transport):
        self.client, self.transport = client, transport
        self.metadata, self.log = [], []

    def call(self, name, payload=b"", quiet=False):
        frame = describe(self.client.call(name, payload))
        if not quiet:
            print(json.dumps(frame, allow_nan=False))
        self.metadata.extend(self.transport.drain_metadata())
        return frame

    def status(self):
        return self.call("status", quiet=True)

    def lifecycle(self, action, state, keep_alive=None, mode=None):
        """Publish ARM/START/SET_MODE and wait for H0's receipt. Returns the receipt name.
        H0 rejects ARM/START until fresh sensing follows boot or STOP; retry those."""
        receipt_name = "TIMEOUT"
        for _ in range(50):
            if keep_alive is not None:
                self.call("setpoint", keep_alive, quiet=True)
            payload = set_mode(mode) if action == "set-mode" else b""
            result = self.call(action, payload)["result"]
            if result == "BUSY":
                continue
            if result != "ACCEPTED":
                return result
            for _ in range(40):
                if keep_alive is not None:
                    self.call("setpoint", keep_alive, quiet=True)
                receipt = self.call("lifecycle", quiet=True)
                receipt_name = receipt["lifecycle_result"]
                if receipt_name not in ("PENDING", "NONE"):
                    self.log.append({"action": action, "receipt": receipt_name,
                                     "state": receipt["state"],
                                     "pipeline_result": receipt["pipeline_result"]})
                    break
            if receipt_name == "APPLIED" or action == "set-mode" or receipt_name == "PENDING":
                return receipt_name
            for _ in range(5):  # let fresh samples arrive before retrying
                if keep_alive is not None:
                    self.call("setpoint", keep_alive, quiet=True)
                self.status()
        return receipt_name

    def tune(self, name, value):
        """Write one controller parameter and wait for H0 to apply it."""
        identifier = PARAMETERS[name]
        kind = PARAMETER_TYPES.get(identifier, "f32")
        written = self.call("param-write", parameter_write(identifier, kind, value))
        if written["result"] != "ACCEPTED":
            return written["result"]
        for _ in range(40):
            self.status()  # lets H0 run in lockstep mode
            receipt = self.call("param-read", parameter_read(PARAMETERS["tuning-result"]), quiet=True)
            if receipt["value"] not in (0, 1):
                return LIFECYCLE_RESULTS[receipt["value"]]
        return "TIMEOUT"

    def read(self, name):
        return self.call("param-read", parameter_read(PARAMETERS[name]), quiet=True)["value"]

    def stop(self):
        self.call("stop")
        for _ in range(100):
            stopped = self.status()
            if stopped["state"] != SIL_STATE_RUNNING:
                return stopped
        return stopped

    def run(self, mode, reference, duration_us):
        """ARM, START and stream one target; returns telemetry samples while it ran."""
        payload = setpoint(mode, 0.0, reference) if mode == "current" else setpoint(mode, reference)
        steps = {"arm": self.lifecycle("arm", SIL_STATE_ARMED, payload)}
        steps["start"] = (self.lifecycle("start", SIL_STATE_RUNNING, payload)
                          if steps["arm"] == "APPLIED" else "SKIPPED")
        samples, started = [], None
        while steps["start"] == "APPLIED":
            self.call("setpoint", payload, quiet=True)
            telemetry = self.call("telemetry", quiet=True)
            started = telemetry["uptime_us_mod32"] if started is None else started
            elapsed = (telemetry["uptime_us_mod32"] - started) & 0xFFFFFFFF
            samples.append({"elapsed_us": elapsed, **{key: telemetry[key] for key in (
                "state", "mode", "faults", "iq_a", "id_a", "iq_target_a", "current_valid",
                "velocity_rad_s", "position_rad", "tuning_revision", "physical_applied")}})
            if elapsed >= duration_us or telemetry["state"] != SIL_STATE_RUNNING:
                break
        return steps, samples

    def sil_records(self):
        return [record for record in self.metadata if record.get("kind") == "sil"]


def _tracking(session, scenario, reference, duration_us, samples, steps):
    _, _, key, plant_key, firmware_key, tolerance, agreement_tolerance = SIL_DEFAULTS[scenario]
    running = [sample for sample in samples if sample["state"] == SIL_STATE_RUNNING]
    tail = running[len(running) * 3 // 4:]
    sil = session.sil_records()
    window = [record for record in sil if record["state"] == SIL_STATE_RUNNING]
    window = window[len(window) * 3 // 4:] if scenario != "current" else [
        record for record in sil if record["firmware_current_valid"]]
    agreement = [abs(record[plant_key] - record[firmware_key]) for record in window]
    metrics = {
        "running_samples": len(running),
        "tail_mean_abs_error": (sum(abs(reference - sample[key]) for sample in tail) / len(tail)
                                if tail else None),
        "final_value": running[-1][key] if running else None,
        "max_plant_firmware_delta": max(agreement) if agreement else None,
        "faults_while_running": sorted({sample["faults"] for sample in running}),
        "uart_current_valid": bool(tail) and all(sample["current_valid"] for sample in tail),
    }
    checks = {
        "lifecycle_applied": steps == {"arm": "APPLIED", "start": "APPLIED"},
        "tracks_reference": metrics["tail_mean_abs_error"] is not None
        and metrics["tail_mean_abs_error"] < tolerance,
        "firmware_matches_plant": metrics["max_plant_firmware_delta"] is not None
        and metrics["max_plant_firmware_delta"] < agreement_tolerance,
        "no_faults_while_running": metrics["faults_while_running"] == [0],
        "uart_current_valid": metrics["uart_current_valid"],
    }
    return metrics, checks


def _finish(session, checks, metrics):
    stopped = session.stop()
    time.sleep(0.1)
    session.metadata.extend(session.transport.drain_metadata())
    sil = session.sil_records()
    last = sil[-1] if sil else {}
    metrics.update({
        "board_applied_updates": last.get("board_applied"),
        "adc_h0_consumed": last.get("adc_h0_consumed"),
        "adc_missing_slots": last.get("adc_missing_slots"),
        "adc_timing_rejections": last.get("adc_timing_rejections"),
        "encoder_losses": last.get("encoder_losses"),
        "drive_after_stop": last.get("drive"),
        "state_after_stop": stopped["state"],
    })
    checks.update({
        "adc_slots_complete": metrics["adc_missing_slots"] == 0,
        "encoder_frames_complete": metrics["encoder_losses"] == 0,
        "stop_disables_drive": metrics["drive_after_stop"] == "HIGH_Z"
        and metrics["state_after_stop"] == SIL_STATE_DISABLED,
    })


def run_sil_demo(client, transport, reference=None, duration_us=None, json_path=None,
                 scenario="current"):
    """One Board1 SIL product scenario over the V1 wire. The device runs the production
    Board1 path; only the motor is virtual. Nothing here is physical evidence."""

    session = SilSession(client, transport)
    session.call("capability")
    boot = {name: session.read(name) for name in (
        "boot-configuration", "calibration-input", "measurement-qualified")}
    checks, metrics, samples = {}, {}, []

    if scenario in ("current", "velocity", "position"):
        default_reference, default_duration = SIL_DEFAULTS[scenario][:2]
        reference = default_reference if reference is None else reference
        duration_us = default_duration if duration_us is None else duration_us
        if scenario != "current":
            checks["set_mode_applied"] = session.lifecycle("set-mode", SIL_STATE_DISABLED,
                                                           mode=scenario) == "APPLIED"
            checks["mode_reported"] = session.status()["mode"] == MODES[scenario]
        steps, samples = session.run(scenario, reference, duration_us)
        tracking_metrics, tracking_checks = _tracking(session, scenario, reference, duration_us,
                                                      samples, steps)
        metrics.update(tracking_metrics)
        checks.update(tracking_checks)
        _finish(session, checks, metrics)

    elif scenario == "tuning":
        reference = 0.4 if reference is None else reference
        duration_us = 20000 if duration_us is None else duration_us
        revision = session.read("tuning-revision")
        checks["kp_applied"] = session.tune("current-kp-q", 2.0) == "APPLIED"
        checks["ki_applied"] = session.tune("current-ki-q", 800.0) == "APPLIED"
        checks["lease_applied"] = session.tune("command-lease-us", 20000) == "APPLIED"
        checks["readback_matches"] = (
            abs(session.read("current-kp-q") - 2.0) < 1e-6
            and abs(session.read("current-ki-q") - 800.0) < 1e-3
            and session.read("command-lease-us") == 20000)
        checks["revision_counts_applies"] = session.read("tuning-revision") == revision + 3
        bad = session.call("param-write", parameter_write(PARAMETERS["current-kp-q"], "f32", 99.0))
        checks["out_of_range_rejected"] = bad["result"] == "INVALID_ARGUMENT"
        over = session.call("param-write", parameter_write(PARAMETERS["current-limit"], "f32", 3.0))
        checks["boot_current_ceiling_kept"] = over["result"] == "INVALID_ARGUMENT"
        inner = session.call("param-write", parameter_write(PARAMETERS["iq-limit"], "f32", 2.6))
        checks["iq_limit_bounded_by_current_limit"] = inner["result"] == "INVALID_ARGUMENT"
        payload = setpoint("current", 0.0, reference)
        steps = {"arm": session.lifecycle("arm", SIL_STATE_ARMED, payload)}
        steps["start"] = session.lifecycle("start", SIL_STATE_RUNNING, payload)
        running_write = session.call("param-write",
                                     parameter_write(PARAMETERS["current-kp-q"], "f32", 1.0))
        checks["write_refused_while_running"] = running_write["result"] == "BAD_STATE"
        running_mode = session.call("set-mode", set_mode("velocity"))
        checks["mode_refused_while_running"] = running_mode["result"] == "BAD_STATE"
        session.stop()
        # The tuned controller still tracks.
        steps, samples = session.run("current", reference, duration_us)
        tracking_metrics, tracking_checks = _tracking(session, "current", reference, duration_us,
                                                      samples, steps)
        metrics.update(tracking_metrics)
        checks.update({f"tuned_{key}": value for key, value in tracking_checks.items()})
        metrics["tuning_revision"] = session.read("tuning-revision")
        _finish(session, checks, metrics)

    elif scenario == "recover":
        reference = 0.4 if reference is None else reference
        duration_us = 10000 if duration_us is None else duration_us
        steps, samples = session.run("current", reference, duration_us)
        checks["first_run_started"] = steps == {"arm": "APPLIED", "start": "APPLIED"}
        # The Host stops refreshing the target; status polls still move device time.
        faulted = None
        for _ in range(200):
            faulted = session.status()
            if faulted["state"] == SIL_STATE_FAULT:
                break
        checks["lease_expiry_faults"] = faulted is not None and faulted["state"] == SIL_STATE_FAULT
        metrics["fault_bits"] = faulted["faults"] if faulted else None
        clear = session.call("clear")
        recovered = None
        for _ in range(200):
            recovered = session.status()
            if recovered["state"] == SIL_STATE_DISABLED and recovered["faults"] == 0:
                break
        checks["clear_accepted"] = clear["result"] == "ACCEPTED"
        checks["recovered_to_disabled"] = (recovered is not None
                                           and recovered["state"] == SIL_STATE_DISABLED
                                           and recovered["faults"] == 0)
        session.metadata.clear()
        steps, samples = session.run("current", reference, duration_us)
        tracking_metrics, tracking_checks = _tracking(session, "current", reference, duration_us,
                                                      samples, steps)
        metrics.update(tracking_metrics)
        checks.update({f"rerun_{key}": value for key, value in tracking_checks.items()})
        _finish(session, checks, metrics)
    else:
        raise ValueError(f"unknown SIL scenario {scenario}")

    passed = bool(checks) and all(checks.values())
    summary = {"kind": "summary", "demo": f"board1-sil-{scenario}",
               "status": "SIL_PASS" if passed else "SIL_FAIL",
               "simulated": True, "physical_applied": False, "reference": reference,
               "duration_us": duration_us, "boot": boot, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "lifecycle": session.log, "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(
            {"summary": summary, "samples": samples, "metadata": session.metadata}, indent=2),
            encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def calibrate_host(args, parser):
    if not args.simulator or args.port or args.message:
        parser.error("calibrate requires the Host falconfoc-virtual-calibrate --simulator")
    product_mode = args.calibration_mode
    if product_mode:
        if any((args.bootstrap, args.reuse_session, args.stages, args.cogging, args.capabilities,
                args.reachability, args.reachability_torque)):
            parser.error("--calibration-mode selects one workflow; remove other workflow options")
        if (product_mode == "quick") != bool(args.load_config):
            parser.error("--calibration-mode quick requires --load-config; full and selective use measurements")
        args.reuse_session = product_mode == "selective"
        args.bootstrap = "known" if product_mode == "quick" else "unknown"
    if args.load_config and (args.save_config or args.cancel_after or args.cogging
                             or args.trace or args.flux_trace or args.startup_trace):
        parser.error("--load-config cannot be combined with --save-config or capture options")
    if args.save_config and not args.approve_candidate:
        parser.error("--save-config requires --approve-candidate")
    reports = [pathlib.Path(path).resolve() for path in
               (args.json, args.save_config, args.load_config, args.trace, args.flux_trace, args.boot_trace,
                args.startup_trace) if path]
    if len(reports) != len(set(reports)):
        parser.error("report and configuration paths must be distinct")
    command = [str(pathlib.Path(args.simulator).resolve()), "--profile-id", args.profile_id]
    if product_mode:
        command.append("--product-timing")
    if args.reuse_session:
        if any((args.json, args.approve_candidate, args.save_config, args.load_config,
                args.cancel_after, args.trace, args.flux_trace, args.startup_trace, args.capabilities,
                args.reachability, args.reachability_torque, args.stages,
                args.boot_scenario, args.boot_trace)):
            parser.error("Selective calibration takes commands on stdin; pass capture and review options as commands")
        command.append("--reuse-session")
        if args.bootstrap:
            command.extend(("--bootstrap", args.bootstrap))
        if args.cogging:
            command.append("--cogging")
        try:
            return subprocess.run(command, check=False).returncode
        except OSError:
            print(json.dumps({"error": "SIMULATOR_START_FAILED", "stage": "HOST_RUNNER"}))
            return 2
    if args.approve_candidate:
        command.extend(("--approve-candidate", args.approve_candidate))
    if args.cogging:
        command.append("--cogging")
    if args.capabilities:
        command.append("--capabilities")
    if args.stages:
        command.extend(("--stages", args.stages))
    if args.bootstrap:
        command.extend(("--bootstrap", args.bootstrap))
    for key, value in (("--save-config", args.save_config), ("--load-config", args.load_config),
                       ("--cancel-after", args.cancel_after), ("--trace", args.trace),
                       ("--flux-trace", args.flux_trace),
                       ("--boot-scenario", args.boot_scenario), ("--boot-trace", args.boot_trace),
                       ("--cq-startup-csv", args.startup_trace)):
        if value is not None:
            command.extend((key, value))
    for option, point in (("--reachability", args.reachability),
                          ("--reachability-torque", args.reachability_torque)):
        if point is not None:
            command.extend((option, *point))
    stderr = ""
    try:
        if product_mode and not args.json:
            return subprocess.run(command, timeout=60, check=False).returncode
        process = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
        stderr = process.stderr
    except OSError:
        events = [{"error": "SIMULATOR_START_FAILED", "stage": "HOST_RUNNER"}]
        exit_status = 2
    except subprocess.TimeoutExpired:
        events = [{"error": "SIMULATOR_TIMEOUT", "stage": "HOST_RUNNER"}]
        exit_status = 2
    else:
        exit_status = process.returncode
        try:
            events = [json.loads(line) for line in process.stdout.splitlines()]
            if not events or any(not isinstance(item, dict) or
                                 ("event" not in item and "error" not in item) for item in events):
                raise ValueError("missing calibration event")
        except ValueError:
            events = [{"error": "SIMULATOR_PROTOCOL_ERROR", "stage": "HOST_RUNNER",
                       "runner_exit_status": process.returncode}]
            exit_status = 2
    for event in events:
        print(json.dumps(event, allow_nan=False))
    if stderr:
        print(stderr, file=sys.stderr, end="")
    if args.json:
        with pathlib.Path(args.json).open("x", encoding="utf-8") as report:
            json.dump({"events": events, "exit_status": exit_status},
                      report, indent=2, allow_nan=False)
            report.write("\n")
    return exit_status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    connection = parser.add_mutually_exclusive_group()
    connection.add_argument("--port", help="Optional pyserial port. Connects when selected.")
    connection.add_argument("--simulator", help="Host foc-serial-sim executable")
    parser.add_argument(
        "action",
        choices=["encode", "decode", "vectors", "request", "demo", "virtual-demo", "sil-demo",
                 "calibrate"],
    )
    parser.add_argument("message", nargs="?", choices=sorted(MESSAGES))
    parser.add_argument("--hex", default="", help="Wire hex for decode, including delimiter")
    parser.add_argument("--sequence", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--session", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--mode", choices=["current", "velocity", "position"], default="current")
    parser.add_argument("--first", type=float, default=0)
    parser.add_argument("--second", type=float, default=0)
    parser.add_argument("--id", type=int, default=1)
    parser.add_argument("--param", choices=sorted(PARAMETERS),
                        help="parameter by name; overrides --id and selects its type")
    parser.add_argument("--type", choices=["u32", "f32"], default="u32")
    parser.add_argument("--value", type=float, default=0)
    parser.add_argument(
        "--virtual-scenario",
        choices=["current", "velocity", "position", "load", "fault"],
        default="current",
    )
    parser.add_argument("--advance-us", type=int, default=500)
    parser.add_argument("--sil-speed", type=float, default=0.02,
                        help="sil-demo: virtual/wall time ratio of the free-running device")
    parser.add_argument("--sil-scenario", choices=SIL_SCENARIOS, default="current",
                        help="sil-demo: product scenario to run")
    parser.add_argument("--sil-lockstep-us", type=int, default=0,
                        help="sil-demo: advance this much virtual time per frame instead")
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
    trace = parser.add_mutually_exclusive_group()
    trace.add_argument("--trace", help="Export the d/q R/L terminal voltage and ADC aperture CSV")
    trace.add_argument("--flux-trace", help="Export the accepted flux terminal intervals as a CSV")
    parser.add_argument("--startup-trace", help="Export the approved Host Current Startup V1 CSV")
    parser.add_argument("--stages", choices=("rs", "jb"), help="Review a single parameter group")
    parser.add_argument("--calibration-mode", choices=("quick", "full", "selective"),
                        help="Run a Host product calibration workflow with live stage timing")
    parser.add_argument("--bootstrap", choices=("inspect", "known", "offset", "unknown"),
                        help="Qualify or acquire an initial Host electrical reference")
    parser.add_argument("--boot-scenario", choices=("nominal", "load", "partial", "stalled", "encoder",
                                                    "current-limit", "phase", "direction", "weak-emf",
                                                    "stop", "fault", "timeout", "cancel",
                                                    "identity-change", "output-reject"),
                        help="Select a Host-only bootstrap failure fixture")
    parser.add_argument("--boot-trace", help="Export the synchronized Host bootstrap apertures")
    parser.add_argument("--reuse-session", action="store_true",
                        help="Run a Host reuse session with commands from stdin")
    diagnostic = parser.add_mutually_exclusive_group()
    diagnostic.add_argument("--capabilities", action="store_true", help="Inspect Host calibration capabilities")
    diagnostic.add_argument("--reachability", nargs=4, metavar=("MECH_RAD_S", "ID_A", "IQ_A", "VBUS_V"),
                            help="Evaluate a steady Current point from --load-config")
    diagnostic.add_argument("--reachability-torque", nargs=4,
                            metavar=("MECH_RAD_S", "ZERO_ID", "TORQUE_NM", "VBUS_V"),
                            help="Evaluate a steady Impedance torque point from --load-config")
    args = parser.parse_args()
    if args.action != "calibrate" and any(
        (args.capabilities, args.reachability, args.reachability_torque, args.startup_trace,
         args.flux_trace, args.calibration_mode,
         args.stages, args.reuse_session, args.bootstrap, args.boot_scenario, args.boot_trace)
    ):
        parser.error("Host calibration diagnostic options require calibrate")
    try:
        if args.action == "calibrate":
            return calibrate_host(args, parser)
        payload = b""
        if args.message == "setpoint":
            payload = setpoint(args.mode, args.first, args.second)
        elif args.message == "set-mode":
            payload = set_mode(args.mode)
        if args.param is not None:
            args.id = PARAMETERS[args.param]
            args.type = PARAMETER_TYPES.get(args.id, "f32")
        if args.message == "param-read":
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
            if args.action in {"virtual-demo", "sil-demo"} and not args.simulator:
                parser.error(f"{args.action} requires --simulator")
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
            if args.action == "sil-demo":
                simulator_arguments = (
                    ["--lockstep-us", str(args.sil_lockstep_us)] if args.sil_lockstep_us > 0
                    else ["--speed", str(args.sil_speed)]
                )
            transport = (
                SimulatorTransport(os.path.abspath(args.simulator), simulator_arguments)
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
                elif args.action == "sil-demo":
                    summary = run_sil_demo(
                        client,
                        transport,
                        args.reference,
                        args.duration_us,
                        args.json,
                        args.sil_scenario,
                    )
                    if summary["status"] != "SIL_PASS":
                        return 3
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
