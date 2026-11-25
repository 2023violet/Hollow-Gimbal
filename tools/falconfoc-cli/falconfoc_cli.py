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

import falconfoc_can


MESSAGES = {
    "hello": 1, "capability": 2, "status": 3, "setpoint": 0x10,
    "stop": 0x11, "arm": 0x12, "start": 0x13, "set-mode": 0x14, "lifecycle": 0x15,
    "fault": 0x20,
    "clear": 0x21, "fault-snapshot": 0x22, "param-read": 0x30, "param-write": 0x31,
    "save": 0x32, "erase-config": 0x33, "telemetry": 0x40, "stream": 0x41,
    "calibrate": 0x50, "calibrate-read": 0x51, "calibrate-abort": 0x52,
    "commutation-write": 0x53, "set-zero": 0x54,
}
RESULTS = [
    "OK", "ACCEPTED", "APPLIED", "BAD_FRAME", "BAD_CRC",
    "UNSUPPORTED_VERSION", "UNKNOWN_COMMAND", "INVALID_ARGUMENT", "BAD_SESSION",
    "REPLAY", "BUSY", "BAD_STATE", "OUTPUT_LOCKED", "NOT_SUPPORTED", "INTERNAL_ERROR",
    "NOT_CALIBRATED",
]
MODES = {"current": 0, "velocity": 1, "position": 2, "impedance": 3}
LIFECYCLE_RESULTS = ["NONE", "PENDING", "APPLIED", "REJECTED", "CANCELLED"]
LIFECYCLE_ACTIONS = ["NONE", "ARM", "START", "CLEAR_FAULT", "SET_MODE"]
# Parameter IDs and wire types come from the generated table (firmware/at32f415/parameters.json).
from falconfoc_params import PARAMETER_INFO, PARAMETER_TYPES, PARAMETERS  # noqa: E402,F401
# Persisted by store format 3 (motor profile, derived PI, limits, protection windows).
PROFILE_PARAMETERS = ("model-rs", "model-ld", "model-lq", "model-flux", "model-inertia",
                      "model-damping", "rated-current", "peak-current", "max-speed",
                      "gear-ratio", "current-bandwidth", "current-kp-q", "current-ki-q",
                      "current-limit", "velocity-limit", "trip-sustained-ms")
# Coast check outcome (ID 91).
COAST_STATUS = ["NOT_RUN", "USED", "TOO_FEW", "INCONSISTENT"]
# Safety fault bits (g3/safe/include/foc_safe.h); bits 9-15 are motor protection, 16 the
# gate driver nFAULT line.
FAULT_BITS = ["COMMAND_TIMEOUT", "SAMPLE_TIMEOUT", "SAMPLE_INVALID", "FAST_HEARTBEAT",
              "FOREGROUND_HEARTBEAT", "INVALID_STATE", "WDT", "FLASH", "FATAL_RESET_REQUIRED",
              "OVER_CURRENT", "OVER_VOLTAGE", "UNDER_VOLTAGE", "OVER_SPEED",
              "SUSTAINED_CURRENT", "STALL", "TRACKING_DIVERGENCE", "DRIVER_FAULT"]


def fault_names(mask):
    return [name for bit, name in enumerate(FAULT_BITS) if mask & (1 << bit)] + [
        bit for bit in range(len(FAULT_BITS), 32) if mask & (1 << bit)]
CAL_STATES = ["IDLE", "ADC_ZERO", "DRIVE_MODE", "DRIVE_ARM", "DRIVE_START", "ALIGN", "SWEEP",
              "DRIVE_STOP", "APPLY", "DONE", "FAILED", "RL_D", "RL_Q", "FLUX", "TUNE", "RESTORE",
              "INERTIA", "COAST"]
CAL_FAILURES = ["NONE", "STATE", "ADC", "LIFECYCLE", "DRIVE", "CURRENT", "SENSOR", "ROTOR",
                "OFFSET", "ADMISSION", "APPLY", "ABORTED", "TIMEOUT", "RL", "FLUX", "TUNE",
                "CAPTURE", "INERTIA"]
CAL_STAGES = {"all": 3, "rotor": 1, "electrical": 2, "zero": 4, "mechanical": 8,
              "electrical-mechanical": 10, "all-mechanical": 11}
CAL_SOURCES = ["BOOT", "RUN", "STORAGE", "HOST", "SUPPLY"]
CAL_RESULT_BITS = (("rotor", 1), ("rl", 2), ("flux", 4), ("pi", 8), ("inertia", 16))
STORE_STATES = ["UNAVAILABLE", "EMPTY", "LOADED", "INVALID_RECORD", "IDENTITY_MISMATCH",
                "VALUE_REJECTED", "IO_ERROR"]
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


def commutation_write(pole_pairs, direction, offset_rad):
    """COMMUTATION_WRITE payload: pole pairs u16, encoder direction i8 (+1/-1), offset f32."""
    if direction not in (1, -1) or not math.isfinite(offset_rad):
        raise ValueError("direction must be +1 or -1 and the offset finite")
    return struct.pack("<Hbf", pole_pairs, direction, offset_rad)


def commission(client, pole_pairs=None, direction=None, offset_rad=None, polls=4000):
    """Write a commutation group (by default the one the board booted with) and wait until
    the firmware has measured this boot's ADC zero. Returns the COMMUTATION_WRITE result.
    This is the Host import of an offline calibration; the zero is measured on the MCU."""
    read = lambda identifier: describe(client.call("param-read", parameter_read(identifier)))
    if pole_pairs is None:
        pole_pairs = read(PARAMETERS["pole-pairs"])["value"]
    if offset_rad is None:
        offset_rad = read(PARAMETERS["electrical-offset"])["value"]
    if direction is None:
        direction = 1 if read(42)["value"] == 1 else -1
    result = describe(client.call("commutation-write",
                                  commutation_write(pole_pairs, direction, offset_rad)))["result"]
    if result != "APPLIED":
        return result
    for _ in range(polls):
        describe(client.call("status"))
        if read(PARAMETERS["adc-zero-state"])["value"] == 1:
            return result
    return "ZERO_TIMEOUT"


def parameter_write(identifier, kind, value):
    if kind == "u32":
        integer = int(value)
        if integer != value:
            raise ValueError("u32 parameter requires an integer")
        return struct.pack("<HBI", identifier, 1, integer)
    if not math.isfinite(value):
        raise ValueError("Parameter must be finite")
    return struct.pack("<HBf", identifier, 2, value)


def setpoint(mode, first, second=0.0, *extra):
    """SETPOINT payload. impedance takes position, velocity, kp, kd, torque (output side)."""
    values = (first, second) + extra
    if not all(math.isfinite(value) for value in values):
        raise ValueError("Setpoint must be finite")
    if mode == "impedance":
        if len(values) != 5:
            raise ValueError("impedance needs position, velocity, kp, kd and torque")
        return struct.pack("<B5f", MODES[mode], *values)
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
        # TELEMETRY is 83 bytes since P1 (75 since M1); earlier lengths still decode.
        if len(body) not in ((63, 75, 83) if frame.type == 0x40 else (30,)):
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
        result["fault_names"] = fault_names(result["faults"])
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
            if len(body) >= 75:
                result.update(
                    id_target_a=struct.unpack_from("<f", body, 63)[0],
                    iq_target_a=struct.unpack_from("<f", body, 67)[0],
                    tuning_revision=struct.unpack_from("<I", body, 71)[0],
                )
            if len(body) == 83:
                # Bytes 38/42 are output-side (gear ratio, zero offset); these are the rotor.
                result.update(
                    rotor_position_rad=struct.unpack_from("<f", body, 75)[0],
                    rotor_velocity_rad_s=struct.unpack_from("<f", body, 79)[0],
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
    elif frame.type == 0x51 and body[0] == 0:
        if len(body) not in (39, 71, 93):
            raise ValueError("BAD_RESPONSE")
        name = lambda table, index: table[index] if index < len(table) else index
        result.update(
            calibration_state=name(CAL_STATES, body[1]),
            calibration_failure=name(CAL_FAILURES, body[2]),
            failed_state=name(CAL_STATES, body[3]), admission_failure=body[4],
            samples=struct.unpack_from("<H", body, 5)[0],
            pole_pairs=struct.unpack_from("<H", body, 7)[0],
            calibration_direction=struct.unpack_from("<b", body, 9)[0],
            runtime_direction=struct.unpack_from("<b", body, 10)[0],
            electrical_offset_rad=struct.unpack_from("<f", body, 11)[0],
            rotor_residual_rad=struct.unpack_from("<f", body, 15)[0],
            adc_offset_count=list(struct.unpack_from("<3f", body, 19)),
            adc_noise_rms_count=struct.unpack_from("<f", body, 31)[0],
            applied_runs=struct.unpack_from("<I", body, 35)[0],
        )
        if len(body) >= 71:
            result.update(dict(zip(
                ("rs_ohm", "ld_h", "lq_h", "flux_wb", "kt_nm_per_a", "kp_d_v_per_a",
                 "kp_q_v_per_a", "ki_v_per_a_s"), struct.unpack_from("<8f", body, 39))))
        if len(body) == 93:
            result.update(
                committed_stages=[stage for stage, bit in (("rotor", 1), ("electrical", 2),
                                                            ("mechanical", 8))
                                  if body[71] & bit],
                results_present=[kind for kind, bit in CAL_RESULT_BITS if body[72] & bit],
                active_source=name(CAL_SOURCES, body[73]), ring_high_water=body[74],
                active_pole_pairs=struct.unpack_from("<H", body, 75)[0],
                active_electrical_offset_rad=struct.unpack_from("<f", body, 77)[0],
                active_adc_offset_count=list(struct.unpack_from("<3f", body, 81)),
            )
    elif frame.type == 0x22 and body[0] == 0:
        if len(body) != 39:
            raise ValueError("BAD_RESPONSE")
        faults = struct.unpack_from("<I", body, 3)[0]
        result.update(dict(
            snapshot_valid=bool(body[1]), snapshot_mode=body[2], snapshot_faults=faults,
            snapshot_fault_names=fault_names(faults), snapshot_h0_us=struct.unpack_from("<I", body, 7)[0],
            **dict(zip(("speed_rad_s", "speed_ref_rad_s", "measured_id_a", "measured_iq_a",
                        "iq_ref_a", "phase_peak_a", "vbus_v"), struct.unpack_from("<7f", body, 11)))))
    elif frame.type == 0x54 and body[0] == 2:
        if len(body) != 5:
            raise ValueError("BAD_RESPONSE")
        result["zero_rad"] = struct.unpack_from("<f", body, 1)[0]
    elif frame.type == 0x32 and body[0] == 2:
        if len(body) != 5:
            raise ValueError("BAD_RESPONSE")
        result["stored_sequence"] = struct.unpack_from("<I", body, 1)[0]
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
SIL_SCENARIOS = ("current", "velocity", "position", "tuning", "recover", "persist", "calibrate",
                 "calibrate-persist", "calibrate-faults", "protect", "profile", "swap", "frame",
                 "trajectory", "inertia", "rotor-range", "driver-fault", "coast",
                 "dead-time", "dead-time-comp", "can", "can-multi", "mit")
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
        busy = 0
        for _ in range(50):
            if keep_alive is not None:
                self.call("setpoint", keep_alive, quiet=True)
            payload = set_mode(mode) if action == "set-mode" else b""
            result = self.call(action, payload)["result"]
            # BUSY covers a pending ADC zero (about 60 ms after boot) or calibration.
            while result == "BUSY" and busy < 2000:
                busy += 1
                self.status()
                result = self.call(action, payload, quiet=True)["result"]
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
    # The nominal placeholder is not runnable: import the boot group (the SIL motor is
    # that motor) and let the MCU measure this boot's ADC zero.
    checks["commissioned"] = commission(client) == "APPLIED"

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


def run_sil_persist(simulator, base_arguments, flash_path, json_path=None):
    """Board1 SIL persistence over the V1 wire across simulated power cycles: each boot is
    a new device process on the same flash file. Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    if flash_path.exists():
        flash_path.unlink()
    checks, boots = {}, []

    def boot(note):
        transport = SimulatorTransport(simulator, list(base_arguments) + ["--flash", str(flash_path)])
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        record = {"boot": note, "store_state": STORE_STATES[session.read("store-state")],
                  "loaded_sequence": session.read("store-loaded-sequence"),
                  "stored_sequence": session.read("store-stored-sequence"),
                  "current_kp_q": session.read("current-kp-q"),
                  "command_lease_us": session.read("command-lease-us"),
                  "rx_budget": session.read("rx-budget"),
                  "capabilities": session.call("capability", quiet=True)["capabilities"]}
        boots.append(record)
        print(json.dumps(record, allow_nan=False))
        return transport, session, record

    transport, session, first = boot("fresh")
    checks["fresh_storage_empty"] = first["store_state"] == "EMPTY" and first["stored_sequence"] == 0
    # Import the boot group so the drive may run; the save then stores it (step 3).
    checks["commissioned"] = commission(session.client) == "APPLIED"
    checks["persistence_capability"] = bool(first["capabilities"] & 64)
    checks["tuning_applied"] = (session.tune("current-kp-q", 2.2) == "APPLIED"
                                and session.tune("command-lease-us", 30000) == "APPLIED")
    rx = session.call("param-write", parameter_write(PARAMETERS["rx-budget"], "u32", 16))
    checks["ram_policy_applied"] = rx["result"] == "APPLIED"
    saved = session.call("save")
    checks["save_applied"] = saved["result"] == "APPLIED" and saved.get("stored_sequence") == 1
    payload = setpoint("current", 0.0, 0.4)
    session.lifecycle("arm", SIL_STATE_ARMED, payload)
    session.lifecycle("start", SIL_STATE_RUNNING, payload)
    checks["save_refused_while_running"] = session.call("save")["result"] == "BAD_STATE"
    checks["erase_refused_while_running"] = session.call("erase-config")["result"] == "BAD_STATE"
    session.stop()
    transport.__exit__()

    transport, session, second = boot("after-save")
    checks["restored_after_power_cycle"] = (
        second["store_state"] == "LOADED" and second["loaded_sequence"] == 1
        and abs(second["current_kp_q"] - 2.2) < 1e-6 and second["command_lease_us"] == 30000
        and second["rx_budget"] == 16)
    steps, samples = session.run("current", 0.4, 20000)
    metrics, tracking = _tracking(session, "current", 0.4, 20000, samples, steps)
    checks.update({f"restored_{key}": value for key, value in tracking.items()})
    session.stop()
    saved = session.call("save")
    checks["second_save_advances_sequence"] = (saved["result"] == "APPLIED"
                                               and saved.get("stored_sequence") == 2)
    transport.__exit__()

    # Damage both slots: the device must refuse them and keep the boot configuration.
    image = bytearray(flash_path.read_bytes())
    slot_bytes = len(image) // 2
    for slot in (0, 1):
        image[slot * slot_bytes + 60] ^= 0x01
    flash_path.write_bytes(bytes(image))
    transport, session, third = boot("damaged")
    checks["damaged_record_refused"] = (third["store_state"] == "INVALID_RECORD"
                                        and abs(third["current_kp_q"] - 1.35) < 1e-6
                                        and third["command_lease_us"] == 2500)
    checks["erase_applied"] = session.call("erase-config")["result"] == "APPLIED"
    transport.__exit__()

    transport, session, fourth = boot("after-erase")
    checks["erased_storage_empty"] = (fourth["store_state"] == "EMPTY"
                                      and fourth["stored_sequence"] == 0
                                      and abs(fourth["current_kp_q"] - 1.35) < 1e-6)
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-persist",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "boots": boots, "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


# A motor that differs from the Board1 nominal configuration (7 pole pairs, zero
# offset, 2048-count ADC zero). Only the plant knows these values; the firmware
# is never told them. The checks below compare its findings against them.
SIL_CALIBRATE_PLANT = {"pole_pairs": 11, "offset_rad": 1.1, "adc_offset": (2078, 2030, 2060),
                       "rs_ohm": 0.45, "ld_h": 0.0009, "lq_h": 0.0013, "flux_wb": 0.008}
SIL_CALIBRATE_ARGUMENTS = [
    "--motor-pole-pairs", str(SIL_CALIBRATE_PLANT["pole_pairs"]),
    "--motor-offset-rad", str(SIL_CALIBRATE_PLANT["offset_rad"]),
    "--adc-offset", ",".join(map(str, SIL_CALIBRATE_PLANT["adc_offset"])),
    "--motor-rs-ohm", str(SIL_CALIBRATE_PLANT["rs_ohm"]),
    "--motor-ld-h", str(SIL_CALIBRATE_PLANT["ld_h"]),
    "--motor-lq-h", str(SIL_CALIBRATE_PLANT["lq_h"]),
    "--motor-psi-f-wb", str(SIL_CALIBRATE_PLANT["flux_wb"]),
]


def _relative_error(found, true):
    return abs(found - true) / abs(true)


def _calibrate_wait(session, stop_at=None, limit=120000):
    """Poll CALIBRATE_READ (which also moves lockstep time) until DONE or FAILED."""
    seen = []
    for _ in range(limit):
        view = session.call("calibrate-read", quiet=True)
        if not seen or seen[-1] != view["calibration_state"]:
            seen.append(view["calibration_state"])
        if stop_at is not None and view["calibration_state"] == stop_at:
            return view, seen
        if view["calibration_state"] in ("DONE", "FAILED"):
            return view, seen
    return view, seen


def _wait_still(session, limit=20000):
    """Calibration requires a still shaft: poll telemetry until the speed is near zero."""
    for _ in range(limit):
        telemetry = session.call("telemetry", quiet=True)
        if abs(telemetry["velocity_rad_s"]) < 0.2:
            return True
    return False


def run_sil_calibrate(client, transport, json_path=None):
    """On-MCU calibration (CAL-1 ADC zero, CAL-2 direction/pole pairs/offset) of a motor
    whose parameters the firmware does not know, then control with the result.
    Nothing here is physical evidence."""

    session = SilSession(client, transport)
    checks, metrics, stages = {}, {}, {}
    plant = SIL_CALIBRATE_PLANT
    checks["calibration_capability"] = bool(session.call("capability")["capabilities"] & 128)
    # A Host streaming setpoints over a real link raises the lease (ID 74; default 2.5 ms).
    checks["host_lease_applied"] = session.tune("command-lease-us", 50000) == "APPLIED"
    metrics["nominal_pole_pairs"] = session.read("pole-pairs")

    # 1. The nominal placeholder is not runnable: ARM is refused until a group exists.
    checks["set_mode_velocity"] = session.lifecycle("set-mode", SIL_STATE_DISABLED,
                                                    mode="velocity") == "APPLIED"
    refused = session.lifecycle("arm", SIL_STATE_ARMED, setpoint("velocity", 5.0))
    metrics["uncalibrated_arm"] = refused
    checks["uncalibrated_arm_refused"] = refused == "NOT_CALIBRATED"
    checks["shaft_settles"] = _wait_still(session)

    # 2. STOP during the drive aborts and keeps the previous configuration.
    started = session.call("calibrate")
    checks["calibration_accepted"] = started["result"] == "ACCEPTED"
    checks["controls_refused_while_calibrating"] = (
        session.call("set-mode", set_mode("current"))["result"] == "BUSY")
    view, _ = _calibrate_wait(session, stop_at="SWEEP")
    for _ in range(400):  # let the sweep move the shaft before STOP
        session.status()
    session.stop()
    view, _ = _calibrate_wait(session)
    metrics["abort"] = {key: view[key] for key in ("calibration_state", "calibration_failure",
                                                   "failed_state")}
    checks["stop_aborts_calibration"] = (view["calibration_state"] == "FAILED"
                                         and view["calibration_failure"] == "ABORTED")
    checks["abort_keeps_configuration"] = session.read("pole-pairs") == metrics["nominal_pole_pairs"]
    for _ in range(200):  # STOP is applied by H0: wait for DISABLED with INHIBIT confirmed
        status = session.status()
        if status["state"] == SIL_STATE_DISABLED and status["software_output_acknowledged"]:
            break
    _wait_still(session)

    # 3. Full run.
    started = session.call("calibrate")
    checks["full_run_accepted"] = started["result"] == "ACCEPTED"
    view, seen = _calibrate_wait(session)
    stages["sequence"] = seen
    metrics["calibration"] = {key: view[key] for key in (
        "calibration_state", "calibration_failure", "failed_state", "admission_failure", "samples",
        "pole_pairs", "calibration_direction", "runtime_direction", "electrical_offset_rad",
        "rotor_residual_rad", "adc_offset_count", "adc_noise_rms_count", "applied_runs",
        "rs_ohm", "ld_h", "lq_h", "flux_wb", "kt_nm_per_a", "kp_d_v_per_a", "kp_q_v_per_a",
        "ki_v_per_a_s")}
    offset_error = abs((view["electrical_offset_rad"] - plant["offset_rad"] + math.pi)
                       % (2 * math.pi) - math.pi)
    adc_error = max(abs(found - true) for found, true in zip(view["adc_offset_count"],
                                                           plant["adc_offset"]))
    metrics["offset_error_rad"], metrics["adc_offset_error_count"] = offset_error, adc_error
    checks["calibration_done"] = view["calibration_state"] == "DONE"
    checks["pole_pairs_found"] = view["pole_pairs"] == plant["pole_pairs"]
    checks["electrical_offset_found"] = offset_error < 0.05
    checks["adc_zero_found"] = adc_error <= 2.0
    checks["tuning_survives_calibration"] = session.read("command-lease-us") == 50000
    # CAL-3/4/5: electrical model and the current PI designed from it.
    kt_true = 1.5 * plant["pole_pairs"] * plant["flux_wb"]
    errors = {
        "rs": _relative_error(view["rs_ohm"], plant["rs_ohm"]),
        "ld": _relative_error(view["ld_h"], plant["ld_h"]),
        "lq": _relative_error(view["lq_h"], plant["lq_h"]),
        "flux": _relative_error(view["flux_wb"], plant["flux_wb"]),
        "kt": _relative_error(view["kt_nm_per_a"], kt_true),
    }
    metrics["electrical_relative_error"] = errors
    checks["rs_found"] = errors["rs"] < 0.05
    checks["ld_found"] = errors["ld"] < 0.10
    checks["lq_found"] = errors["lq"] < 0.10
    checks["flux_found"] = errors["flux"] < 0.05
    checks["kt_found"] = errors["kt"] < 0.05
    checks["pi_applied_to_live_firmware"] = (
        view["kp_q_v_per_a"] > 0.0
        and abs(session.read("current-kp-q") - view["kp_q_v_per_a"]) < 1e-6
        and abs(session.read("current-ki-q") - view["ki_v_per_a_s"]) < 1e-3)
    checks["applied_to_live_firmware"] = (session.read("pole-pairs") == plant["pole_pairs"]
                                          and abs(session.read("phase-a-offset")
                                                  - view["adc_offset_count"][0]) < 1e-3)

    # 4. The calibrated firmware controls this motor.
    checks["set_mode_velocity_after"] = session.lifecycle("set-mode", SIL_STATE_DISABLED,
                                                          mode="velocity") == "APPLIED"
    steps, samples = session.run("velocity", 5.0, 600000)
    velocity_metrics, velocity_checks = _tracking(session, "velocity", 5.0, 600000, samples, steps)
    metrics["velocity"] = velocity_metrics
    checks.update({f"calibrated_velocity_{k}": v for k, v in velocity_checks.items()})
    session.stop()
    checks["set_mode_current_after"] = session.lifecycle("set-mode", SIL_STATE_DISABLED,
                                                         mode="current") == "APPLIED"
    session.metadata.clear()
    steps, samples = session.run("current", 0.4, 20000)
    current_metrics, current_checks = _tracking(session, "current", 0.4, 20000, samples, steps)
    metrics["current"] = current_metrics
    checks.update({f"calibrated_current_{k}": v for k, v in current_checks.items()})
    session.stop()

    # 5. The electrical stages alone, on the rotor mapping applied above.
    _wait_still(session)
    rerun = session.call("calibrate", bytes([CAL_STAGES["electrical"]]))
    view, seen = _calibrate_wait(session)
    stages["electrical_only"] = seen
    checks["mode_restored_to_current"] = session.status()["mode"] == MODES["current"]
    checks["electrical_only_rerun_done"] = (rerun["result"] == "ACCEPTED"
                                            and view["calibration_state"] == "DONE"
                                            and "ADC_ZERO" not in seen and "SWEEP" not in seen)
    metrics["electrical_only_flux_wb"] = view["flux_wb"]
    session.metadata.clear()
    steps, samples = session.run("current", 0.4, 20000)
    _, rerun_checks = _tracking(session, "current", 0.4, 20000, samples, steps)
    checks["current_after_electrical_rerun"] = rerun_checks["tracks_reference"]
    _finish(session, checks, metrics)

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-calibrate",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "plant_truth_for_checks_only": {
                   "pole_pairs": plant["pole_pairs"], "offset_rad": plant["offset_rad"],
                   "adc_offset": list(plant["adc_offset"])},
               "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "stages": stages, "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_calibrate_persist(simulator, base_arguments, flash_path, json_path=None):
    """Calibrate a motor the firmware does not know, save, power-cycle (new device process
    on the same flash) and run without recalibrating; erase restores the defaults.
    Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    if flash_path.exists():
        flash_path.unlink()
    plant = SIL_CALIBRATE_PLANT
    arguments = list(base_arguments) + SIL_CALIBRATE_ARGUMENTS + ["--flash", str(flash_path)]
    checks, metrics, boots = {}, {}, []

    def boot(note):
        transport = SimulatorTransport(simulator, arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        record = {"boot": note, "store_state": STORE_STATES[session.read("store-state")],
                  "calibration_restored": session.read("store-calibration"),
                  "pole_pairs": session.read("pole-pairs"),
                  "electrical_offset_rad": session.read("electrical-offset"),
                  "phase_a_offset": session.read("phase-a-offset"),
                  "current_kp_q": session.read("current-kp-q")}
        boots.append(record)
        print(json.dumps(record, allow_nan=False))
        return transport, session, record

    def velocity_run(session, duration_us=600000):
        session.lifecycle("set-mode", SIL_STATE_DISABLED, mode="velocity")
        steps, samples = session.run("velocity", 5.0, duration_us)
        result = _tracking(session, "velocity", 5.0, duration_us, samples, steps)
        session.stop()
        _wait_still(session)
        return result

    # 1. Calibrate (all stages) and save.
    transport, session, first = boot("fresh")
    checks["fresh_storage_empty"] = first["store_state"] == "EMPTY"
    checks["host_lease_applied"] = session.tune("command-lease-us", 50000) == "APPLIED"
    session.call("calibrate")
    view, _ = _calibrate_wait(session)
    checks["calibration_done"] = view["calibration_state"] == "DONE"
    calibrated = {"pole_pairs": view["pole_pairs"], "offset": view["electrical_offset_rad"],
                  "adc_a": view["adc_offset_count"][0], "kp_q": view["kp_q_v_per_a"]}
    metrics["calibrated"] = calibrated
    saved = session.call("save")
    checks["save_applied"] = saved["result"] == "APPLIED"
    transport.__exit__()

    # 2. Power cycle: restored before any producer; the motor runs without recalibrating.
    transport, session, second = boot("after-save")
    checks["calibration_restored"] = (second["store_state"] == "LOADED"
                                      and second["calibration_restored"] == 1)
    checks["rotor_mapping_restored"] = (second["pole_pairs"] == plant["pole_pairs"]
                                        and abs(second["electrical_offset_rad"]
                                                - calibrated["offset"]) < 1e-6)
    checks["adc_zero_restored"] = abs(second["phase_a_offset"] - calibrated["adc_a"]) < 1e-3
    checks["current_pi_restored"] = abs(second["current_kp_q"] - calibrated["kp_q"]) < 1e-6
    velocity_metrics, velocity_checks = velocity_run(session)
    metrics["velocity_after_restore"] = velocity_metrics
    checks.update({f"restored_velocity_{k}": v for k, v in velocity_checks.items()})

    # 3. After a reboot the electrical stages run alone on the restored rotor mapping.
    rerun = session.call("calibrate", bytes([CAL_STAGES["electrical"]]))
    view, seen = _calibrate_wait(session)
    metrics["electrical_after_restore"] = {key: view[key] for key in (
        "calibration_state", "calibration_failure", "admission_failure", "rs_ohm", "flux_wb")}
    checks["electrical_rerun_after_restore"] = (rerun["result"] == "ACCEPTED"
                                                and view["calibration_state"] == "DONE"
                                                and _relative_error(view["rs_ohm"],
                                                                    plant["rs_ohm"]) < 0.05)
    checks["erase_applied"] = session.call("erase-config")["result"] == "APPLIED"
    transport.__exit__()

    # 4. Erased: Board1 defaults again, and this motor no longer follows a speed target.
    transport, session, third = boot("after-erase")
    checks["defaults_after_erase"] = (third["store_state"] == "EMPTY"
                                      and third["calibration_restored"] == 0
                                      and third["pole_pairs"] == 7)
    session.tune("command-lease-us", 50000)
    velocity_metrics, _ = velocity_run(session, 300000)
    tail_error = velocity_metrics["tail_mean_abs_error"]
    metrics["velocity_after_erase_tail_error"] = tail_error
    checks["uncalibrated_again_after_erase"] = tail_error is None or tail_error > 1.0
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-calibrate-persist",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "boots": boots, "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_calibrate_faults(simulator, base_arguments, flash_path, json_path=None):
    """Negative calibration paths (M3-R): every failure returns the Runtime to CURRENT
    mode, reports what the run committed and keeps nothing it did not commit.
    Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    plant = SIL_CALIBRATE_PLANT
    checks, metrics = {}, {}

    def plant_arguments(**override):
        values = dict(plant, **override)
        return [
            "--motor-pole-pairs", str(values["pole_pairs"]),
            "--motor-offset-rad", str(values["offset_rad"]),
            "--adc-offset", ",".join(map(str, values["adc_offset"])),
            "--motor-rs-ohm", str(values["rs_ohm"]),
            "--motor-ld-h", str(values["ld_h"]),
            "--motor-lq-h", str(values["lq_h"]),
            "--motor-psi-f-wb", str(values["flux_wb"]),
        ]

    def boot(arguments):
        transport = SimulatorTransport(simulator, list(base_arguments) + arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        return transport, session

    def outcome(session, view):
        keys = ("calibration_state", "calibration_failure", "failed_state", "committed_stages",
                "results_present", "active_source", "active_pole_pairs")
        result = {key: view.get(key) for key in keys}
        mode = session.status()["mode"]
        result["runtime_mode"] = {value: name for name, value in MODES.items()}.get(mode, mode)
        print(json.dumps(result, allow_nan=False))
        return result

    # 1. A noisy ADC zero is refused before the motor is driven; nothing changes.
    transport, session = boot(plant_arguments() + ["--adc-noise", "6"])
    session.call("calibrate")
    view, seen = _calibrate_wait(session)
    noisy = metrics["noisy_adc"] = outcome(session, view)
    checks["noisy_adc_refused_before_drive"] = (noisy["calibration_failure"] == "ADC"
                                                and noisy["failed_state"] == "ADC_ZERO"
                                                and "ALIGN" not in seen)
    checks["noisy_adc_commits_nothing"] = (noisy["committed_stages"] == []
                                           and noisy["active_source"] == "BOOT"
                                           and session.read("pole-pairs") == 7)
    transport.__exit__()

    # 2. CAL-1/2 pass, CAL-3 fails (Rs / L = 2600 /s: the 200 us interval does not resolve
    #    the pole, so the R/L fit is refused): the rotor phase stays
    #    committed and reported, the Runtime is back in CURRENT mode, PARAM_SAVE stores
    #    the committed rotor calibration and a power cycle restores it.
    if flash_path.exists():
        flash_path.unlink()
    arguments = plant_arguments(rs_ohm=1.3, ld_h=0.0005, lq_h=0.0007) + ["--flash", str(flash_path)]
    transport, session = boot(arguments)
    session.call("calibrate")
    view, seen = _calibrate_wait(session)
    partial = metrics["rl_failure_after_rotor"] = outcome(session, view)
    checks["unresolved_rl_rotor_phase_committed"] = (partial["committed_stages"] == ["rotor"]
                                               and partial["active_source"] == "RUN"
                                               and partial["active_pole_pairs"] == 11)
    checks["unresolved_rl_fails_in_rl"] = (partial["calibration_failure"] == "RL"
                                     and partial["failed_state"] in ("RL_D", "RL_Q"))
    checks["mode_current_after_failure"] = partial["runtime_mode"] == "current"
    checks["pi_not_applied_after_failure"] = (
        "pi" not in partial["results_present"]
        and abs(session.read("current-kp-q") - 1.35) < 1e-6)
    checks["save_after_partial_applied"] = session.call("save")["result"] == "APPLIED"
    transport.__exit__()
    transport, session = boot(arguments)
    view = session.call("calibrate-read", quiet=True)
    restored = metrics["after_reboot"] = outcome(session, view)
    checks["committed_rotor_restored"] = (session.read("store-calibration") == 1
                                          and restored["active_source"] == "STORAGE"
                                          and restored["active_pole_pairs"] == 11
                                          and restored["committed_stages"] == [])
    transport.__exit__()

    # 3. Abort during CAL-3 on a good motor: FAILED/ABORTED, mode CURRENT, rotor kept.
    transport, session = boot(plant_arguments())
    session.call("calibrate")
    _calibrate_wait(session, stop_at="RL_D")
    session.call("calibrate-abort")
    view, _ = _calibrate_wait(session)
    aborted = metrics["abort_in_rl"] = outcome(session, view)
    checks["abort_reported"] = (aborted["calibration_failure"] == "ABORTED"
                                and aborted["committed_stages"] == ["rotor"])
    checks["mode_current_after_abort"] = aborted["runtime_mode"] == "current"
    session.lifecycle("set-mode", SIL_STATE_DISABLED, mode="velocity")
    steps, samples = session.run("velocity", 5.0, 600000)
    velocity_metrics, velocity_checks = _tracking(session, "velocity", 5.0, 600000, samples, steps)
    metrics["velocity_after_abort"] = velocity_metrics
    checks["host_controls_after_abort"] = velocity_checks.get("tracks_reference", False)
    session.stop()
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-calibrate-faults",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_protect(simulator, base_arguments, json_path=None):
    """Motor protection (first step of the parameter contract, section 12): commutation
    mismatches, one injection per rule, fault clear and a deliberate reversal. The
    nominal placeholder (7 pole pairs, offset 0) driving a different SIL motor is the
    same commutation error as a stored profile on a replaced motor. Nothing here is
    physical evidence."""

    checks, metrics = {}, {}

    def boot(arguments):
        transport = SimulatorTransport(simulator, list(base_arguments) + arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        assert commission(client) == "APPLIED"
        return transport, session

    def outcome(session, samples):
        status = session.status()
        snapshot = session.call("fault-snapshot", quiet=True)
        last = samples[-1] if samples else {}
        result = {"ran_us": last.get("elapsed_us"), "faults": status["fault_names"],
                  "state": status["state"],
                  "snapshot": {key: snapshot.get(key) for key in (
                      "snapshot_valid", "snapshot_fault_names", "snapshot_mode", "speed_rad_s",
                      "speed_ref_rad_s", "measured_id_a", "measured_iq_a", "iq_ref_a",
                      "phase_peak_a", "vbus_v")}}
        print(json.dumps(result, allow_nan=False))
        return result

    def snapshot_complete(result, expected):
        snapshot = result["snapshot"]
        return (snapshot["snapshot_valid"] and snapshot["snapshot_fault_names"] == result["faults"]
                and result["faults"] == [expected] and snapshot["vbus_v"] > 0.0)

    # 1. Commutation mismatches in VELOCITY mode, 1.5 s each.
    matrix = {
        "same_motor": ((7, 0.0), set()),
        "offset_17deg": ((7, 0.3), set()),
        "offset_86deg": ((7, 1.5), {"SUSTAINED_CURRENT", "STALL", "TRACKING_DIVERGENCE"}),
        "offset_172deg": ((7, 3.0), {"OVER_SPEED", "TRACKING_DIVERGENCE"}),
        "pole_pairs_11": ((11, 0.0), {"TRACKING_DIVERGENCE", "SUSTAINED_CURRENT", "STALL"}),
        "pole_pairs_14": ((14, 0.0), {"TRACKING_DIVERGENCE", "SUSTAINED_CURRENT", "STALL"}),
    }
    for name, ((pole_pairs, offset), allowed) in matrix.items():
        transport, session = boot(["--motor-pole-pairs", str(pole_pairs),
                                   "--motor-offset-rad", str(offset)])
        session.lifecycle("set-mode", SIL_STATE_DISABLED, mode="velocity")
        steps, samples = session.run("velocity", 5.0, 1500000)
        result = metrics[f"mismatch_{name}"] = outcome(session, samples)
        tripped = set(result["faults"])
        checks[f"mismatch_{name}"] = (steps == {"arm": "APPLIED", "start": "APPLIED"} and (
            not tripped if not allowed else (len(tripped) == 1 and tripped <= allowed
                                              and result["snapshot"]["snapshot_valid"])))
        session.stop()
        transport.__exit__()

    # 2. One injection per rule, CURRENT mode unless stated.
    def inject(name, arguments, expected, prepare=(), mode="current", reference=0.3,
               duration_us=800000):
        transport, session = boot(arguments)
        for parameter, value in prepare:
            assert session.tune(parameter, value) == "APPLIED", (name, parameter)
        session.lifecycle("set-mode", SIL_STATE_DISABLED, mode=mode)
        steps, samples = session.run(mode, reference, duration_us)
        result = metrics[f"inject_{name}"] = outcome(session, samples)
        checks[f"inject_{name}"] = snapshot_complete(result, expected)
        return transport, session, result

    # After commissioning (settle and ADC zero, about 0.2 s of device time).
    transport, session, result = inject("overcurrent", ["--adc-step", "800000,2000"],
                                        "OVER_CURRENT")
    checks["overcurrent_snapshot_peak"] = result["snapshot"]["phase_peak_a"] > 3.0
    transport.__exit__()
    transport, session, result = inject("overvoltage", ["--vbus-v", "23"], "OVER_VOLTAGE")
    transport.__exit__()
    transport, session, result = inject("undervoltage", ["--vbus-v", "4.5"], "UNDER_VOLTAGE")
    # The bus is still low: FAULT_CLEAR is refused.
    clear = session.call("clear")
    for _ in range(100):
        after = session.status()
    checks["undervoltage_clear_refused"] = (after["state"] == SIL_STATE_FAULT
                                            and "UNDER_VOLTAGE" in after["fault_names"])
    metrics["undervoltage_clear"] = clear["result"]
    transport.__exit__()
    transport, session, result = inject(
        "overspeed", [], "OVER_SPEED",
        prepare=(("velocity-limit", 2.0), ("trip-overspeed", 2.5)), reference=0.4,
        duration_us=1500000)
    # A protection latch clears: snapshot gone, the drive runs again.
    clear = session.call("clear")
    recovered = None
    for _ in range(200):
        recovered = session.status()
        if recovered["state"] == SIL_STATE_DISABLED and recovered["faults"] == 0:
            break
    snapshot = session.call("fault-snapshot", quiet=True)
    checks["protection_fault_clears"] = (clear["result"] == "ACCEPTED"
                                         and recovered["state"] == SIL_STATE_DISABLED
                                         and recovered["faults"] == 0
                                         and not snapshot["snapshot_valid"])
    steps, samples = session.run("current", 0.2, 20000)
    checks["runs_after_clear"] = steps == {"arm": "APPLIED", "start": "APPLIED"}
    session.stop()
    transport.__exit__()
    transport, session, result = inject(
        "sustained_current", [], "SUSTAINED_CURRENT",
        prepare=(("iq-limit", 0.6), ("current-limit", 0.6), ("trip-sustained-ms", 100)),
        reference=0.6)
    transport.__exit__()
    transport, session, result = inject(
        "stall", ["--motor-damping", "10"], "STALL", mode="velocity", reference=5.0)
    transport.__exit__()

    # 3. Ordering is enforced on write.
    transport, session = boot([])
    checks["overcurrent_above_limit_required"] = session.call(
        "param-write", parameter_write(PARAMETERS["trip-overcurrent"], "f32", 2.5))[
            "result"] == "INVALID_ARGUMENT"
    checks["undervoltage_below_overvoltage_required"] = session.call(
        "param-write", parameter_write(PARAMETERS["trip-undervoltage"], "f32", 23.0))[
            "result"] == "INVALID_ARGUMENT"
    transport.__exit__()

    # 4. No false trip: +5 rad/s, then a deliberate reversal to -5 rad/s.
    transport, session = boot([])
    session.lifecycle("set-mode", SIL_STATE_DISABLED, mode="velocity")
    steps, samples = session.run("velocity", 5.0, 600000)
    reversal = setpoint("velocity", -5.0)
    started = None
    while samples and samples[-1]["state"] == SIL_STATE_RUNNING:
        session.call("setpoint", reversal, quiet=True)
        telemetry = session.call("telemetry", quiet=True)
        started = telemetry["uptime_us_mod32"] if started is None else started
        samples.append({key: telemetry[key] for key in ("state", "faults", "velocity_rad_s")})
        if ((telemetry["uptime_us_mod32"] - started) & 0xFFFFFFFF) >= 1000000 or \
                telemetry["state"] != SIL_STATE_RUNNING:
            break
    metrics["reversal_final_rad_s"] = samples[-1]["velocity_rad_s"]
    checks["reversal_no_trip"] = (samples[-1]["state"] == SIL_STATE_RUNNING
                                  and samples[-1]["faults"] == 0
                                  and abs(samples[-1]["velocity_rad_s"] + 5.0) < 0.5)
    session.stop()
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-protect",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_profile(simulator, base_arguments, flash_path, json_path=None):
    """Motor profile, unified refresh and store format 3 (parameter contract section 13):
    the model and bandwidth derive the current PI, the peak current and maximum speed
    bound the limits, the rated current sets the sustained-current protection, the profile
    survives a power cycle, and an on-MCU calibration writes the identified model.
    Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    checks, metrics = {}, {}

    def boot(arguments):
        transport = SimulatorTransport(simulator, list(base_arguments) + arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        return transport, session

    def close(value, expected, tolerance=1e-4):
        return abs(value - expected) <= tolerance * max(1.0, abs(expected))

    def gains(session):
        return {name: session.read(name) for name in (
            "current-kp-d", "current-kp-q", "current-ki-d", "current-ki-q")}

    if flash_path.exists():
        flash_path.unlink()
    arguments = ["--flash", str(flash_path)]
    transport, session = boot(arguments)
    checks["commissioned"] = commission(session.client) == "APPLIED"

    # 1. Model and bandwidth derive the current PI (SIL motor: 0.6 Ω, 1.2/1.5 mH).
    for name, value in (("model-rs", 0.6), ("model-ld", 0.0012)):
        assert session.tune(name, value) == "APPLIED", name
    metrics["gains_incomplete_model"] = gains(session)
    checks["incomplete_model_keeps_gains"] = close(
        metrics["gains_incomplete_model"]["current-kp-q"], 1.35)
    assert session.tune("model-lq", 0.0015) == "APPLIED"
    omega = 2.0 * math.pi * 150.0
    derived = metrics["gains_150hz"] = gains(session)
    checks["pi_derived_at_150hz"] = (close(derived["current-kp-d"], omega * 0.0012)
                                     and close(derived["current-kp-q"], omega * 0.0015)
                                     and close(derived["current-ki-q"], omega * 0.6))
    assert session.tune("current-bandwidth", 100.0) == "APPLIED"
    omega = 2.0 * math.pi * 100.0
    derived = metrics["gains_100hz"] = gains(session)
    checks["pi_follows_bandwidth"] = (close(derived["current-kp-q"], omega * 0.0015)
                                      and close(derived["current-ki-d"], omega * 0.6))
    checks["bandwidth_above_h0_limit_refused"] = (
        session.tune("current-bandwidth", 300.0) == "INVALID_ARGUMENT")
    checks["model_the_design_refuses_is_refused"] = (
        session.tune("model-rs", 5.0) == "INVALID_ARGUMENT" and close(session.read("model-rs"), 0.6))

    # 2. Flux gives Kt; inertia, damping and gear ratio are stored.
    for name, value in (("model-flux", 0.01), ("model-inertia", 0.002),
                        ("model-damping", 0.006), ("gear-ratio", 9.0)):
        assert session.tune(name, value) == "APPLIED", name
    checks["kt_from_flux"] = close(session.read("model-kt"), 1.5 * 7 * 0.01)

    # 3. Peak current and maximum speed bound the limits.
    assert session.tune("peak-current", 1.5) == "APPLIED"
    checks["peak_tightens_limits"] = (close(session.read("current-limit"), 1.5)
                                      and close(session.read("iq-limit"), 1.5))
    checks["limit_above_peak_refused"] = session.tune("current-limit", 2.0) == "INVALID_ARGUMENT"
    assert session.tune("max-speed", 8.0) == "APPLIED"
    checks["max_speed_tightens_speed_limit"] = close(session.read("velocity-limit"), 8.0)
    checks["speed_limit_above_max_refused"] = (
        session.tune("velocity-limit", 10.0) == "INVALID_ARGUMENT")

    # 4. The rated current sets the sustained-current protection.
    assert session.tune("rated-current", 0.5) == "APPLIED"
    assert session.tune("trip-sustained-ms", 100) == "APPLIED"
    steps, samples = session.run("current", 0.6, 600000)
    status = session.status()
    metrics["rated_current_trip"] = status["fault_names"]
    checks["rated_current_drives_sustained_trip"] = status["fault_names"] == ["SUSTAINED_CURRENT"]
    session.call("clear")
    for _ in range(200):
        if session.status()["state"] == SIL_STATE_DISABLED:
            break
    before = {name: session.read(name) for name in PROFILE_PARAMETERS}
    checks["save_applied"] = session.call("save")["result"] == "APPLIED"
    transport.__exit__()

    # 5. The profile, the derived PI and the thresholds survive a power cycle.
    transport, session = boot(arguments)
    after = {name: session.read(name) for name in PROFILE_PARAMETERS}
    metrics["restored"] = after
    checks["profile_restored"] = (session.read("store-state") == 2 and all(
        close(after[name], before[name]) for name in PROFILE_PARAMETERS))
    transport.__exit__()

    # 6. An on-MCU calibration writes the identified model into the profile.
    plant = SIL_CALIBRATE_PLANT
    transport, session = boot(list(SIL_CALIBRATE_ARGUMENTS))
    session.call("calibrate")
    view, _ = _calibrate_wait(session)
    model = {name: session.read(name) for name in (
        "model-rs", "model-ld", "model-lq", "model-flux", "model-kt", "current-kp-q",
        "current-ki-q")}
    metrics["calibrated_model"] = model
    checks["calibration_writes_profile"] = (
        view["calibration_state"] == "DONE" and close(model["model-rs"], view["rs_ohm"])
        and close(model["model-ld"], view["ld_h"]) and close(model["model-lq"], view["lq_h"])
        and close(model["model-flux"], view["flux_wb"])
        and close(model["model-kt"], 1.5 * plant["pole_pairs"] * view["flux_wb"], 1e-3)
        and close(model["current-kp-q"], view["kp_q_v_per_a"])
        and close(model["current-ki-q"], view["ki_v_per_a_s"]))
    checks["calibrated_model_near_plant"] = (abs(model["model-rs"] - plant["rs_ohm"]) < 0.05
                                             and abs(model["model-flux"] - plant["flux_wb"]) < 0.001)
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-profile",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def _write_wait(client, name, value, polls=400):
    """PARAM_WRITE one parameter; for H0-applied parameters wait for the tuning receipt."""
    identifier = PARAMETERS[name]
    written = describe(client.call("param-write", parameter_write(
        identifier, PARAMETER_TYPES.get(identifier, "f32"), value)))["result"]
    if written != "ACCEPTED":
        return written
    for _ in range(polls):
        describe(client.call("status"))
        receipt = describe(client.call("param-read", parameter_read(PARAMETERS["tuning-result"])))
        if receipt["value"] not in (0, 1):
            return LIFECYCLE_RESULTS[receipt["value"]]
    return "TIMEOUT"


def motor_setup(client, rated_current_a=None, peak_current_a=None, max_speed_rad_s=None,
                bandwidth_hz=None, save=True, polls=200000, mechanical=False):
    """Host motor replacement workflow (parameter contract step 3): on-MCU calibration of
    the connected motor, the user profile from its datasheet, then PARAM_SAVE. Every step
    runs on the MCU; the Host only sequences them and reports."""
    summary = {"kind": "motor-setup", "steps": {}}
    steps = summary["steps"]
    started = "BUSY"
    for _ in range(2000):
        # CAL-7 (mechanical) only on request: the shaft turns with its load attached.
        started = describe(client.call(
            "calibrate", bytes([CAL_STAGES["all-mechanical"]]) if mechanical else b""))["result"]
        if started != "BUSY":
            break
        describe(client.call("status"))
    steps["calibrate"] = started
    view = None
    if started == "ACCEPTED":
        for _ in range(polls):
            view = describe(client.call("calibrate-read"))
            if view["calibration_state"] in ("DONE", "FAILED"):
                break
        steps["calibration_state"] = view["calibration_state"]
        steps["calibration_failure"] = view["calibration_failure"]
        summary["calibration"] = {key: view.get(key) for key in (
            "pole_pairs", "electrical_offset_rad", "rs_ohm", "ld_h", "lq_h", "flux_wb",
            "kp_q_v_per_a", "ki_v_per_a_s")}
    calibrated = view is not None and view["calibration_state"] == "DONE"
    if calibrated:
        for name, value in (("peak-current", peak_current_a), ("rated-current", rated_current_a),
                            ("max-speed", max_speed_rad_s), ("current-bandwidth", bandwidth_hz)):
            if value is not None:
                steps[name] = _write_wait(client, name, value)
        if save:
            steps["save"] = describe(client.call("save"))["result"]
    summary["status"] = "OK" if calibrated and all(
        result in ("APPLIED", "ACCEPTED", "DONE", "NONE")
        for key, result in steps.items() if key != "calibration_failure") else "FAILED"
    return summary


def run_sil_inertia(simulator, base_arguments, flash_path, json_path=None):
    """CAL-7 inertia identification and J-derived control (parameter contract section 18):
    the MCU identifies J and B of motors it does not know, the unified refresh derives the
    outer gains at the velocity bandwidth (ID 108), the rotor stays within the travel
    (ID 109), and J*a + B*v feedforward removes the trajectory overshoot; the result
    survives a power cycle. Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    checks, metrics = {}, {}
    kt = 1.5 * 7 * 0.01
    model = (("model-rs", 0.6), ("model-ld", 0.0012), ("model-lq", 0.0015), ("model-flux", 0.01))

    def boot(inertia, damping, flash=False):
        arguments = list(base_arguments) + ["--motor-inertia", str(inertia),
                                            "--motor-damping", str(damping)]
        if flash:
            arguments += ["--flash", str(flash_path)]
        transport = SimulatorTransport(simulator, arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        return transport, session

    def calibrate(session, stages):
        _wait_still(session)
        origin = session.call("telemetry", quiet=True)["position_rad"]
        started = session.call("calibrate", bytes([stages]))["result"]
        view, excursion = None, 0.0
        for _ in range(600000):
            view = session.call("calibrate-read", quiet=True)
            if view["calibration_state"] in ("DONE", "FAILED"):
                break
            if view["calibration_state"] == "INERTIA":
                position = session.call("telemetry", quiet=True)["position_rad"]
                excursion = max(excursion, abs(position - origin))
        values = {name: session.read(name) for name in (
            "model-inertia", "model-damping", "velocity-kp", "velocity-ki", "position-kp")}
        return {"started": started, "state": view["calibration_state"],
                "failure": view["calibration_failure"], "detail": view["admission_failure"],
                "results": view.get("results_present"), "excursion_rad": excursion, **values}

    def identified(run, inertia, damping, travel=math.pi):
        omega = 2.0 * math.pi * 10.0
        kp = run["model-inertia"] * omega / kt
        return (run["started"] == "ACCEPTED" and run["state"] == "DONE"
                and abs(run["model-inertia"] / inertia - 1.0) < 0.1
                and abs(run["model-damping"] / damping - 1.0) < 0.2
                and abs(run["velocity-kp"] / kp - 1.0) < 1e-3
                and abs(run["velocity-ki"] / (0.25 * kp * omega) - 1.0) < 1e-3
                and abs(run["position-kp"] - 0.25 * omega) < 1e-3
                and run["excursion_rad"] <= travel + 0.05)

    if flash_path.exists():
        flash_path.unlink()

    # 1. The SIL motor (J 0.002, B 0.006): CAL-3/4/5 and CAL-7 in one run (mask 10).
    transport, session = boot(0.002, 0.006, flash=True)
    checks["commissioned"] = commission(session.client) == "APPLIED"
    checks["mechanical_needs_a_model"] = session.call("calibrate", bytes([8]))["result"] == "BAD_STATE"
    run = metrics["electrical_and_inertia"] = calibrate(session, 10)
    checks["inertia_identified_with_electrical"] = identified(run, 0.002, 0.006)
    checks["velocity_bandwidth_above_fifth_refused"] = (
        session.tune("velocity-bandwidth", 40.0) == "INVALID_ARGUMENT")

    # 2. Feedforward: the 6 rad trapezoid of the trajectory scenario now tracks the plan.
    assert session.tune("accel", 20.0) == "APPLIED" and session.tune("decel", 40.0) == "APPLIED"
    start = session.call("telemetry", quiet=True)["position_rad"]
    target = start + 6.0
    session.lifecycle("set-mode", SIL_STATE_DISABLED, mode="position")
    steps, samples = session.run("position", target, 2000000)
    status = session.status()
    session.stop()
    _wait_still(session)
    tail = samples[-len(samples) // 10:] if samples else []
    move = metrics["feedforward_move"] = {
        "overshoot_rad": max((s["position_rad"] - target for s in samples), default=None),
        "final_error_rad": (sum(abs(target - s["position_rad"]) for s in tail) / len(tail)
                            if tail else None)}
    checks["feedforward_removes_overshoot"] = (
        steps == {"arm": "APPLIED", "start": "APPLIED"} and not status["fault_names"]
        and move["overshoot_rad"] < 0.005 and move["final_error_rad"] < 0.001)
    checks["saved"] = session.call("save")["result"] == "APPLIED"
    saved = {name: run[name] for name in ("model-inertia", "model-damping", "velocity-kp")}
    transport.__exit__()

    # 3. Power cycle: J, B and the derived gains restore.
    transport, session = boot(0.002, 0.006, flash=True)
    restored = {name: session.read(name) for name in saved}
    metrics["restored"] = restored
    checks["inertia_restored"] = all(abs(restored[name] - saved[name]) < 1e-9 for name in saved)
    transport.__exit__()

    # 4. A light gimbal rotor (J 2e-5): CAL-3/4/5 and CAL-7 from nothing (mask 10). A heavy
    # one (J 0.01) with a 1 rad travel: CAL-7 alone (mask 8) on the model written from a
    # previous electrical calibration.
    for name, inertia, damping, travel, stages in (("light", 2e-5, 5e-5, math.pi, 10),
                                                   ("heavy", 0.01, 0.02, 1.0, 8)):
        transport, session = boot(inertia, damping)
        commission(session.client)
        for parameter, value in model if stages == 8 else ():
            assert session.tune(parameter, value) == "APPLIED", parameter
        if travel != math.pi:
            assert session.tune("inertia-travel", travel) == "APPLIED"
        run = metrics[f"{name}_rotor"] = calibrate(session, stages)
        checks[f"{name}_rotor_identified"] = identified(run, inertia, damping, travel)
        transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-inertia",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_rotor_range(simulator, base_arguments, json_path=None):
    """CAL-3/4/5 across rotor inertia (parameter contract sections 19 and 20): on a light
    rotor the q-axis R/L pulses turn the shaft, so the fit takes R from the still d axis and
    the back-EMF (ke x the rotor angle change, interpolated to each current aperture) from
    the motion; on a heavy rotor each CAL-4 level waits until its speed is steady in the
    level's direction. Rs, Ld, Lq and flux are identified from a bare gimbal rotor to a
    heavy load; a rotor too heavy to settle within 10 s per level reports TIMEOUT.
    Nothing here is physical evidence."""

    checks, metrics = {}, {}
    nominal = {"rs": 0.6, "ld": 0.0012, "lq": 0.0015, "psi": 0.01}
    high_r = {"rs": 3.0, "ld": 0.004, "lq": 0.0045, "psi": 0.006}
    # Name, motor, inertia, current bandwidth (a high-L motor needs a lower one for CAL-5).
    cases = (("bare_1e-6", nominal, 1e-6, None), ("light_5e-6", nominal, 5e-6, None),
             ("gimbal_2e-5", nominal, 2e-5, None), ("loaded_2e-4", nominal, 2e-4, None),
             ("nominal_2e-3", nominal, 2e-3, None), ("heavy_0.02", nominal, 0.02, None),
             ("heavy_0.05", nominal, 0.05, None), ("high_r_3e-6", high_r, 3e-6, 100.0),
             ("too_heavy_0.2", nominal, 0.2, None))

    for name, motor, inertia, bandwidth in cases:
        arguments = list(base_arguments) + [
            "--motor-rs-ohm", str(motor["rs"]), "--motor-ld-h", str(motor["ld"]),
            "--motor-lq-h", str(motor["lq"]), "--motor-psi-f-wb", str(motor["psi"]),
            "--motor-inertia", str(inertia)]
        transport = SimulatorTransport(simulator, arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        if bandwidth is not None:
            assert session.tune("current-bandwidth", bandwidth) == "APPLIED"
        commissioned = commission(client)
        _wait_still(session)
        started = session.call("calibrate", bytes([CAL_STAGES["electrical"]]))["result"]
        view, peak = None, 0.0
        for _ in range(1000000):
            view = session.call("calibrate-read", quiet=True)
            if view["calibration_state"] in ("DONE", "FAILED"):
                break
            if view["calibration_state"] == "RL_Q":
                speed = session.call("telemetry", quiet=True)["rotor_velocity_rad_s"]
                peak = max(peak, abs(speed))
        values = {key: session.read(f"model-{key}") for key in ("rs", "ld", "lq", "flux")}
        transport.__exit__()
        truth = {"rs": motor["rs"], "ld": motor["ld"], "lq": motor["lq"], "flux": motor["psi"]}
        errors = {key: values[key] / truth[key] - 1.0 for key in truth}
        metrics[name] = {"inertia": inertia, "state": view["calibration_state"],
                         "failure": view["calibration_failure"],
                         "detail": view["admission_failure"], "q_peak_rad_s": peak,
                         "relative_error": errors}
        if name.startswith("too_heavy"):
            # CAL-4 cannot settle within 10 s per level: an explicit TIMEOUT, no model.
            checks[f"{name}_timeout"] = (
                started == "ACCEPTED" and view["calibration_state"] == "FAILED"
                and view["calibration_failure"] == "TIMEOUT")
            continue
        checks[f"{name}_identified"] = (
            commissioned == "APPLIED" and started == "ACCEPTED"
            and view["calibration_state"] == "DONE" and abs(errors["rs"]) < 0.02
            and abs(errors["ld"]) < 0.02 and abs(errors["lq"]) < 0.03
            and abs(errors["flux"]) < 0.02)
        if inertia <= 2e-5:
            # The former standstill fit refused any q-axis speed above 1 rad/s.
            checks[f"{name}_shaft_turned"] = peak > 1.0

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-rotor-range",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_driver_fault(simulator, base_arguments, json_path=None):
    """DRV8311 nFAULT (PB2): two H0 samples in a row with the line low latch DRIVER_FAULT
    (bit 16) and inhibit the output, also while the driver would retry on its own;
    FAULT_CLEAR is refused while the line is low and recovers once it is released. A
    one-sample glitch does not latch. Nothing here is physical evidence."""

    checks, metrics = {}, {}
    on_us, off_us = 1500000, 3000000

    def boot(arguments):
        transport = SimulatorTransport(simulator, list(base_arguments) + arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        assert commission(client) == "APPLIED"
        session.lifecycle("set-mode", SIL_STATE_DISABLED, mode="velocity")
        return transport, session

    def wait_until(session, uptime_us):
        status = session.status()
        for _ in range(200000):
            if status["uptime_us_mod32"] >= uptime_us:
                break
            status = session.status()
        return status

    # 1. A driver fault while running: latched, output inhibited.
    transport, session = boot(["--driver-fault-us", f"{on_us},{off_us}"])
    steps, samples = session.run("velocity", 5.0, 4000000)
    status = session.status()
    running = [sample for sample in samples if sample["state"] == SIL_STATE_RUNNING]
    metrics["trip"] = {"steps": steps, "running_samples": len(running),
                       "state": status["state"], "faults": status["fault_names"],
                       "uptime_us": status["uptime_us_mod32"]}
    checks["runs_before_fault"] = (steps == {"arm": "APPLIED", "start": "APPLIED"}
                                   and len(running) > 0
                                   and abs(running[-1]["velocity_rad_s"] - 5.0) < 1.0)
    checks["driver_fault_latched"] = (status["state"] == SIL_STATE_FAULT
                                      and status["fault_names"] == ["DRIVER_FAULT"]
                                      and status["uptime_us_mod32"] >= on_us)

    # 2. FAULT_CLEAR while nFAULT is still low: refused, the latch stays.
    clear = session.call("clear")
    for _ in range(100):
        refused = session.status()
    metrics["clear_while_low"] = {"result": clear["result"], "clear_result": refused["clear_result"],
                                  "faults": refused["fault_names"]}
    checks["clear_refused_while_low"] = (refused["state"] == SIL_STATE_FAULT
                                         and refused["fault_names"] == ["DRIVER_FAULT"]
                                         and refused["clear_result"] == 3)

    # 3. After the driver releases nFAULT: the clear applies and the drive runs again.
    wait_until(session, off_us + 10000)
    clear = session.call("clear")
    recovered = None
    for _ in range(200):
        recovered = session.status()
        if recovered["state"] == SIL_STATE_DISABLED and recovered["faults"] == 0:
            break
    metrics["clear_after_release"] = {"result": clear["result"],
                                      "clear_result": recovered["clear_result"],
                                      "state": recovered["state"]}
    checks["clear_applies_after_release"] = (clear["result"] == "ACCEPTED"
                                             and recovered["state"] == SIL_STATE_DISABLED
                                             and recovered["faults"] == 0
                                             and recovered["clear_result"] == 2)
    steps, samples = session.run("velocity", 5.0, 300000)
    final = session.status()
    checks["runs_after_clear"] = (steps == {"arm": "APPLIED", "start": "APPLIED"}
                                  and final["faults"] == 0)
    session.stop()
    transport.__exit__()

    # 4. A 100 us glitch (at most one H0 sample) does not latch.
    transport, session = boot(["--driver-fault-us", f"{on_us},{on_us + 100}"])
    steps, samples = session.run("velocity", 5.0, 2500000)
    status = session.status()
    metrics["glitch"] = {"steps": steps, "state": status["state"], "faults": status["fault_names"],
                         "uptime_us": status["uptime_us_mod32"]}
    checks["glitch_ignored"] = (steps == {"arm": "APPLIED", "start": "APPLIED"}
                                and status["state"] == SIL_STATE_RUNNING
                                and status["faults"] == 0
                                and status["uptime_us_mod32"] > on_us + 100000)
    session.stop()
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-driver-fault",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_coast(simulator, base_arguments, json_path=None):
    """Coast check after CAL-4 (parameter contract section 22): the drive spins up, the bridge
    turns off and the phase-voltage dividers (PA1-PA3) see the EMF alone, free of Rs, dead
    time and switch drops. The flux fitted from the line-to-line voltage of the two highest
    phases (the lowest may sit on a body-diode clamp) replaces CAL-4's, and the d share of
    the EMF reports the electrical angle error. The plant takes the worst-case clamp.
    Nothing here is physical evidence."""

    checks, metrics = {}, {}
    flux = 0.01

    def calibrate(arguments, stages=2, trim=None):
        transport = SimulatorTransport(simulator, list(base_arguments) + arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        commissioned = commission(client)
        if trim is not None:
            offset = session.read("electrical-offset")
            written = session.call("param-write", parameter_write(
                PARAMETERS["electrical-offset"], "f32", (offset + trim) % (2.0 * math.pi)))
            assert written["result"] == "APPLIED", written
        _wait_still(session)
        started = session.call("calibrate", bytes([stages]))["result"]
        view, states = None, []
        for _ in range(600000):
            view = session.call("calibrate-read", quiet=True)
            if not states or states[-1] != view["calibration_state"]:
                states.append(view["calibration_state"])
            if view["calibration_state"] in ("DONE", "FAILED"):
                break
        values = {name: session.read(name) for name in (
            "coast-status", "coast-flux", "coast-angle-error", "driven-flux", "model-flux")}
        transport.__exit__()
        values["coast-status"] = COAST_STATUS[values["coast-status"]]
        return {"commissioned": commissioned, "started": started,
                "state": view["calibration_state"], "states": states, **values}

    def used(run, angle=0.0):
        return (run["started"] == "ACCEPTED" and run["state"] == "DONE"
                and "COAST" in run["states"] and run["coast-status"] == "USED"
                and abs(run["coast-flux"] / flux - 1.0) < 0.01
                and abs(run["coast-angle-error"] - angle) < 0.01
                and run["model-flux"] == run["coast-flux"])

    # 1. Ideal inverter: coast and driven flux agree.
    run = metrics["ideal"] = calibrate([])
    checks["ideal_coast_used"] = used(run) and abs(run["driven-flux"] / flux - 1.0) < 0.01

    # 2. A 0.08 V dead-time error per phase: the coast EMF does not see it, and CAL-4 now
    # takes out the dead time CAL-3 identified (it read +15 % before, section 23).
    run = metrics["dead_time"] = calibrate(["--dead-time-v", "0.08"])
    checks["dead_time_driven_corrected"] = abs(run["driven-flux"] / flux - 1.0) < 0.02
    checks["dead_time_coast_used"] = used(run)

    # 3. A commutation offset 0.1 rad off: the coast EMF reports the angle error.
    run = metrics["offset_error"] = calibrate([], trim=0.1)
    checks["offset_error_reported"] = used(run, angle=0.1)

    # 4. With CAL-7 the drive continues into it: no coast.
    run = metrics["with_inertia"] = calibrate([], stages=CAL_STAGES["electrical-mechanical"])
    checks["no_coast_with_cal7"] = (run["state"] == "DONE" and "COAST" not in run["states"]
                                    and run["coast-status"] == "NOT_RUN")

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-coast",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_dead_time(simulator, base_arguments, json_path=None):
    """Inverter dead-time error in CAL-3 (parameter contract section 23): each phase loses
    Vdt x sign(i). CAL-3 fits Vdt on the still d axis next to R and L against the phase-current
    sign vector, then takes it out of the q-axis and CAL-4 voltages, so Rs, the flux and the
    J derived from it stay unbiased; ID 95 reports Vdt. Nothing here is physical evidence."""

    checks, metrics = {}, {}
    truth = {"rs": 0.6, "ld": 0.0012, "lq": 0.0015, "flux": 0.01, "inertia": 0.002}

    def calibrate(arguments, stages):
        transport = SimulatorTransport(simulator, list(base_arguments) + arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        commissioned = commission(client)
        _wait_still(session)
        started = session.call("calibrate", bytes([stages]))["result"]
        view = None
        for _ in range(800000):
            view = session.call("calibrate-read", quiet=True)
            if view["calibration_state"] in ("DONE", "FAILED"):
                break
        values = {name: session.read(name) for name in (
            "model-rs", "model-ld", "model-lq", "driven-flux", "model-inertia", "dead-time-v")}
        transport.__exit__()
        errors = {"rs": values["model-rs"] / truth["rs"] - 1.0,
                  "ld": values["model-ld"] / truth["ld"] - 1.0,
                  "lq": values["model-lq"] / truth["lq"] - 1.0,
                  "flux": values["driven-flux"] / truth["flux"] - 1.0}
        if stages & CAL_STAGES["mechanical"]:
            errors["inertia"] = values["model-inertia"] / truth["inertia"] - 1.0
        return {"commissioned": commissioned, "started": started,
                "state": view["calibration_state"], "dead_time_v": values["dead-time-v"],
                "relative_error": errors}

    def identified(run):
        errors = run["relative_error"]
        return (run["started"] == "ACCEPTED" and run["state"] == "DONE"
                and abs(errors["rs"]) < 0.03 and abs(errors["ld"]) < 0.05
                and abs(errors["lq"]) < 0.05 and abs(errors["flux"]) < 0.02)

    # 1. Ideal inverter: no dead time is found, nothing changes.
    run = metrics["ideal"] = calibrate([], CAL_STAGES["electrical"])
    checks["ideal_unchanged"] = identified(run) and abs(run["dead_time_v"]) < 0.005

    # 2. 0.08 V per phase: Vdt identified, Rs and the CAL-4 flux unbiased (were +28 %, +15 %).
    run = metrics["dead_time"] = calibrate(["--dead-time-v", "0.08"], CAL_STAGES["electrical"])
    checks["dead_time_identified"] = identified(run) and abs(run["dead_time_v"] / 0.08 - 1.0) < 0.15

    # 3. With CAL-7: J follows the corrected flux.
    run = metrics["dead_time_inertia"] = calibrate(["--dead-time-v", "0.08"],
                                                   CAL_STAGES["electrical-mechanical"])
    checks["dead_time_inertia"] = (identified(run)
                                   and abs(run["relative_error"]["inertia"]) < 0.1)

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-dead-time",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_dead_time_comp(simulator, base_arguments, flash_path, json_path=None):
    """Runtime dead-time compensation (parameter contract section 24): the current loop adds
    ID 87 per phase from the reference current. It is off by default; CAL-3 only reports the
    identified dead time (ID 95) and the Host enables it by writing ID 87. Through a 0.2 V dead
    time with a 50 Hz current loop and a load, the q-current tracking error falls against the
    same run with ID 87 at 0; the speed ripple is reported, not improved. The value survives
    a power cycle. Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    checks, metrics = {}, {}
    speed = 2.0

    def boot():
        arguments = list(base_arguments) + ["--dead-time-v", "0.2",
                                            "--load-torque-nm", "0.03,1500000",
                                            "--flash", str(flash_path)]
        transport = SimulatorTransport(simulator, arguments)
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        return transport, session

    def low_speed(session):
        session.lifecycle("set-mode", SIL_STATE_DISABLED, mode="velocity")
        steps, samples = session.run("velocity", speed, 1500000)
        session.stop()
        _wait_still(session)
        running = [s for s in samples if s["state"] == SIL_STATE_RUNNING]
        tail = running[len(running) // 2:]
        if not tail:
            return steps, {"samples": 0}
        error = [s["iq_a"] - s["iq_target_a"] for s in tail]
        mean_speed = sum(s["velocity_rad_s"] for s in tail) / len(tail)
        return steps, {
            "samples": len(tail),
            "iq_target_mean_a": sum(s["iq_target_a"] for s in tail) / len(tail),
            "iq_error_rms_a": math.sqrt(sum(e * e for e in error) / len(error)),
            "speed_mean_rad_s": mean_speed,
            "speed_ripple_rms_rad_s": math.sqrt(
                sum((s["velocity_rad_s"] - mean_speed) ** 2 for s in tail) / len(tail)),
        }

    if flash_path.exists():
        flash_path.unlink()

    # 1. CAL-3 reports the dead time (ID 95) and leaves the compensation off (ID 87 = 0).
    transport, session = boot()
    checks["commissioned"] = commission(session.client) == "APPLIED"
    checks["slow_current_loop"] = session.tune("current-bandwidth", 50.0) == "APPLIED"
    _wait_still(session)
    started = session.call("calibrate", bytes([CAL_STAGES["electrical"]]))["result"]
    view = None
    for _ in range(800000):
        view = session.call("calibrate-read", quiet=True)
        if view["calibration_state"] in ("DONE", "FAILED"):
            break
    identified = session.read("dead-time-v")
    metrics["calibration"] = {"state": view["calibration_state"], "identified_v": identified,
                              "compensation_v": session.read("dead-time-comp")}
    checks["calibration_reports_only"] = (started == "ACCEPTED"
                                          and view["calibration_state"] == "DONE"
                                          and abs(identified / 0.2 - 1.0) < 0.15
                                          and metrics["calibration"]["compensation_v"] == 0.0)

    # 2. Low speed without, then with the Host-enabled compensation.
    steps_off, off = low_speed(session)
    checks["host_enables"] = session.tune("dead-time-comp", identified) == "APPLIED"
    steps_on, on = low_speed(session)
    metrics["uncompensated"], metrics["compensated"] = off, on
    ran = (steps_on == steps_off == {"arm": "APPLIED", "start": "APPLIED"}
           and on["samples"] > 0 and off["samples"] > 0)
    checks["tracking_error_reduced"] = ran and on["iq_error_rms_a"] < 0.6 * off["iq_error_rms_a"]
    checks["speed_holds"] = ran and abs(on["speed_mean_rad_s"] - speed) < 0.2
    checks["above_limit_refused"] = session.tune("dead-time-comp", 1.5) == "INVALID_ARGUMENT"

    # 3. Save, power cycle: ID 87 comes back.
    checks["saved"] = session.call("save")["result"] == "APPLIED"
    transport.__exit__()
    transport, session = boot()
    restored = metrics["restored_v"] = session.read("dead-time-comp")
    checks["compensation_restored"] = abs(restored - identified) < 1e-9
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-dead-time-comp",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def _can_heartbeat(client, node, predicate, timeout_us, keep_alive=None):
    """Wait for a heartbeat of node whose decoded content satisfies predicate."""
    identifier = falconfoc_can.arbitration_id(node, falconfoc_can.COMMANDS["heartbeat"])
    frame = client.wait(lambda f: f.id == identifier and len(f.data) == 7
                        and predicate(falconfoc_can.decode_frame(f)), timeout_us, keep_alive)
    return None if frame is None else falconfoc_can.decode_frame(frame)


def _can_estimates(frames, node):
    identifier = falconfoc_can.arbitration_id(node, falconfoc_can.COMMANDS["get-encoder-estimates"])
    return [falconfoc_can.decode_frame(frame) for frame in frames if frame.id == identifier]


def _can_run(client, node, mode, value, duration_us):
    """Stream one CANSimple setpoint for duration_us; returns the encoder estimates."""
    mark = len(client.bus.inbox)
    for _ in range(duration_us // client.step_us):
        client.set_input(node, mode, value)
        client.step()
    return _can_estimates(client.frames_since(mark), node)


def run_sil_can(simulator, base_arguments, flash_path, json_path=None):
    """CANSimple on one Board1 SIL node (parameter contract section 25): discovery and node
    assignment by serial number, cyclic heartbeat and encoder estimates, parameters over
    SDO, a motor calibration requested over CAN, velocity, position and torque control
    with the command lease as watchdog, Estop and Clear_Errors, then save and power cycle.
    Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    checks, metrics = {}, {}
    serial_number, node = 0xF0C0A1B2C3, 3
    if flash_path.exists():
        flash_path.unlink()

    def boot():
        device = falconfoc_can.MuxSimulatorNode(
            simulator, list(base_arguments) + ["--flash", str(flash_path)], serial_number)
        client = falconfoc_can.CanSimpleClient(falconfoc_can.SilCanBus([device]))
        uart = Client(device)
        uart.hello()
        return device, client, uart

    device, client, uart = boot()
    # 1. Discovery and assignment.
    checks["discovered_unaddressed"] = client.scan() == {serial_number: 0x3F}
    client.address(node, serial_number)
    reply = client.wait(lambda f: f.id == falconfoc_can.arbitration_id(node, 6), 20000)
    checks["assigned_by_serial"] = reply is not None and reply.data[0] == node
    mark = len(client.bus.inbox)
    for _ in range(int(0.35e6) // client.step_us):
        client.step()
    beats = [time for time, frame in client.bus.inbox[mark:]
             if frame.id == falconfoc_can.arbitration_id(node, 1)]
    intervals = [later - earlier for earlier, later in zip(beats, beats[1:])]
    estimates = _can_estimates(client.frames_since(mark), node)
    metrics["cyclic"] = {"heartbeat_intervals_us": intervals, "estimates": len(estimates)}
    checks["heartbeat_100ms"] = len(intervals) >= 2 and all(
        abs(interval - 100000) <= 1000 for interval in intervals)
    checks["estimates_10ms"] = abs(len(estimates) - 35) <= 1
    # 2. Parameters over SDO: the lease, a refused value, the link status.
    checks["lease_written"] = client.sdo(node, PARAMETERS["command-lease-us"], 50000)[0] == 1
    for _ in range(10):
        client.step()
    checks["lease_read_back"] = client.sdo(node, PARAMETERS["command-lease-us"]) == (0, 50000)
    checks["out_of_range_refused"] = client.sdo(node, PARAMETERS["can-node-id"], 99)[0] == 7
    link = client.sdo(node, PARAMETERS["can-status"])[1]
    checks["link_active"] = link is not None and link & 0xFF == 1
    # 3. Commutation import over UART, then the motor calibration over CAN.
    checks["commissioned"] = commission(uart) == "APPLIED"
    client.set_axis_state(node, "motor-calibration")
    running = _can_heartbeat(client, node, lambda hb: hb["state"] == "MOTOR_CALIBRATION", 150000)
    done = _can_heartbeat(client, node, lambda hb: hb["state"] == "IDLE"
                          and hb["procedure"] != "BUSY", 60000000)
    kt = client.sdo_float(node, PARAMETERS["model-kt"])
    metrics["calibration"] = {"done": done, "kt_nm_per_a": kt}
    checks["calibrated_over_can"] = (running is not None and done is not None
                                     and done["procedure"] == "SUCCESS" and kt is not None
                                     and kt > 0.0)
    # 4. Velocity: closed loop while streaming, then the lease trips when streaming stops.
    client.set_controller_mode(node, "velocity")
    for _ in range(10):
        client.step()
    keep = lambda: client.set_input(node, "velocity", 0.5)
    client.set_axis_state(node, "closed-loop")
    closed = _can_heartbeat(client, node, lambda hb: hb["state"] == "CLOSED_LOOP", 300000, keep)
    velocity = _can_run(client, node, "velocity", 0.5, 1000000)
    tail = velocity[len(velocity) // 2:]
    mean_velocity = sum(e["velocity_turns_s"] for e in tail) / max(len(tail), 1)
    metrics["velocity"] = {"target_turns_s": 0.5, "mean_turns_s": mean_velocity}
    checks["velocity_tracks"] = closed is not None and abs(mean_velocity / 0.5 - 1.0) < 0.05
    watchdog = _can_heartbeat(client, node, lambda hb: "WATCHDOG_TIMER_EXPIRED" in hb["error_names"]
                              and hb["state"] == "IDLE", 200000)
    client.send(node, "clear-errors", b"\0")
    cleared = _can_heartbeat(client, node, lambda hb: hb["errors"] == 0, 300000)
    checks["lease_is_watchdog"] = watchdog is not None and cleared is not None
    # 5. Position: once the shaft is still, zero the output, then go to a quarter turn.
    estimates_id = falconfoc_can.arbitration_id(node, falconfoc_can.COMMANDS["get-encoder-estimates"])
    client.wait(lambda f: f.id == estimates_id
                and abs(falconfoc_can.decode_frame(f)["velocity_turns_s"]) < 0.002, 3000000)
    client.send(node, "set-absolute-position", struct.pack("<f", 0.0))
    client.set_controller_mode(node, "position")
    for _ in range(10):
        client.step()
    zeroed = client.request(node, "get-encoder-estimates")
    zero_position = falconfoc_can.decode_frame(zeroed)["position_turns"] if zeroed else None
    keep = lambda: client.set_input(node, "position", 0.25)
    client.set_axis_state(node, "closed-loop")
    closed = _can_heartbeat(client, node, lambda hb: hb["state"] == "CLOSED_LOOP", 300000, keep)
    position = _can_run(client, node, "position", 0.25, 1500000)
    arrived = _can_heartbeat(client, node, lambda hb: hb["trajectory_done"], 150000, keep)
    final = position[-1]["position_turns"] if position else None
    metrics["position"] = {"zeroed_turns": zero_position, "final_turns": final}
    checks["position_zeroed"] = zero_position is not None and abs(zero_position) < 1e-3
    checks["position_reached"] = (closed is not None and arrived is not None and final is not None
                                  and abs(final - 0.25) < 0.01)
    # 6. Torque, then Estop and Clear_Errors.
    client.set_axis_state(node, "idle")
    for _ in range(40):
        client.step()
    client.set_controller_mode(node, "torque")
    for _ in range(10):
        client.step()
    keep = lambda: client.set_input(node, "torque", 0.01)
    client.set_axis_state(node, "closed-loop")
    closed = _can_heartbeat(client, node, lambda hb: hb["state"] == "CLOSED_LOOP", 300000, keep)
    torque_run = _can_run(client, node, "torque", 0.01, 300000)
    keep()
    torques = client.request(node, "get-torques")
    target, estimate = falconfoc_can.decode_frame(torques)["values"] if torques else (None, None)
    metrics["torque"] = {"target_nm": target, "estimate_nm": estimate,
                         "final_turns_s": torque_run[-1]["velocity_turns_s"] if torque_run else None}
    checks["torque_applied"] = (closed is not None and target is not None
                                and abs(target - 0.01) < 1e-6 and abs(estimate / 0.01 - 1.0) < 0.3
                                and torque_run and torque_run[-1]["velocity_turns_s"] > 0.0)
    client.send(node, "estop")
    stopped = _can_heartbeat(client, node, lambda hb: hb["state"] == "IDLE"
                             and "ESTOP_REQUESTED" in hb["error_names"], 150000)
    client.send(node, "clear-errors", b"\0")
    cleared = _can_heartbeat(client, node, lambda hb: hb["errors"] == 0, 300000)
    checks["estop_and_clear"] = stopped is not None and cleared is not None
    # 7. Save over CAN and power cycle: the node ID and the lease come back.
    client.send(node, "reboot", b"\x01")
    for _ in range(20):
        client.step()
    stored = client.sdo(node, PARAMETERS["store-stored-sequence"])[1]
    checks["saved_over_can"] = stored is not None and stored > 0
    device.__exit__()
    device, client, uart = boot()
    beat = _can_heartbeat(client, node, lambda hb: True, 300000)
    checks["node_restored"] = beat is not None and client.sdo(node, PARAMETERS["can-node-id"]) == (0, node)
    checks["lease_restored"] = client.sdo(node, PARAMETERS["command-lease-us"]) == (0, 50000)
    device.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-can", "status":
               "SIL_PASS" if passed else "SIL_FAIL", "simulated": True, "physical_applied": False,
               "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_mit(simulator, base_arguments, flash_path, json_path=None):
    """MIT impedance control on one Board1 SIL node (parameter contract section 26): calibrate
    over CANSimple, switch to MIT, zero, enter motor mode, hold a position, damp to a speed,
    apply a feedforward torque, let the lease trip and clear it, exit, switch back; then the
    same impedance law through UART SETPOINT mode 3. Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    checks, metrics = {}, {}
    serial_number, node, master = 0xF0C0A1B2C4, 3, 0x10
    if flash_path.exists():
        flash_path.unlink()
    device = falconfoc_can.MuxSimulatorNode(
        simulator, list(base_arguments) + ["--flash", str(flash_path)], serial_number)
    bus = falconfoc_can.SilCanBus([device])
    client = falconfoc_can.CanSimpleClient(bus)
    uart = Client(device)
    uart.hello()
    session = SilSession(uart, device)

    def write(name, value):
        identifier = PARAMETERS[name]
        return describe(uart.call("param-write", parameter_write(
            identifier, PARAMETER_TYPES[identifier], value)))["result"]

    def state():
        return session.status()["state"]

    def still(mit, limit=0.04, steps=4000):
        # One MIT velocity step is 2 x V_MAX / 4095 (0.032 rad/s at 65 rad/s).
        for _ in range(steps):
            reply = mit.special("clear")
            if reply is not None and abs(reply["velocity_rad_s"]) < limit:
                return reply
        return None

    # 1. Node, lease and a calibration over CANSimple (as section 25).
    checks["node_set"] = write("can-node-id", node) == "APPLIED"
    checks["lease_raised"] = session.tune("command-lease-us", 50000) == "APPLIED"
    checks["commissioned"] = commission(uart) == "APPLIED"
    client.set_axis_state(node, "motor-calibration")
    started = _can_heartbeat(client, node, lambda hb: hb["state"] == "MOTOR_CALIBRATION", 150000)
    done = _can_heartbeat(client, node, lambda hb: hb["state"] == "IDLE"
                          and hb["procedure"] != "BUSY", 60000000)
    kt = describe(uart.call("param-read", parameter_read(PARAMETERS["model-kt"])))["value"]
    metrics["kt_nm_per_a"] = kt
    checks["calibrated"] = (started is not None and done is not None
                            and done["procedure"] == "SUCCESS" and kt > 0.0)

    # 2. Switch to MIT over SDO; a reply in the original 6-byte format.
    ranges = {"p": 12.5, "v": 65.0, "t": 0.5, "kp": 500.0, "kd": 5.0}
    checks["ranges_set"] = write("mit-t-max", ranges["t"]) == "APPLIED"
    checks["master_set"] = write("can-master-id", master) == "APPLIED"
    checks["switched_over_sdo"] = client.sdo(node, PARAMETERS["can-protocol"], 1)[0] == 2
    mit = falconfoc_can.MitClient(bus, node, master, ranges)
    reply = mit.command(0.0, 0.0, 0.0, 0.0, 0.0)
    checks["original_reply"] = reply is not None and reply["format"] == "original" and reply["node"] == node
    checks["refused_outside_motor_mode"] = state() == SIL_STATE_DISABLED
    checks["damiao_selected"] = write("mit-reply-format", 1) == "APPLIED"

    # 3. Zero once the shaft is still.
    rest = still(mit)
    zeroed = mit.special("zero")
    metrics["zero"] = {"before_rad": None if rest is None else rest["position_rad"],
                       "after_rad": None if zeroed is None else zeroed["position_rad"]}
    checks["zeroed"] = rest is not None and zeroed is not None and abs(zeroed["position_rad"]) < 0.002

    # 4. Enter motor mode, streaming a zero-torque hold until RUNNING.
    entered = mit.special("enter")
    running = False
    for _ in range(400):
        reply = mit.command(0.0, 0.0, 0.0, 0.0, 0.0)
        if reply is not None and reply["error"] == "ENABLED":
            running = True
            break
    checks["entered"] = entered is not None and running and state() == SIL_STATE_RUNNING

    def stream(target, duration_us):
        samples = []
        for _ in range(duration_us // (2 * mit.step_us)):
            reply = mit.command(*target)
            if reply is not None:
                samples.append(reply)
        return samples

    # 5. Position hold: kp and kd only.
    hold = stream((0.5, 0.0, 0.5, 0.01, 0.0), 1500000)
    final = hold[-1]["position_rad"] if hold else None
    metrics["position"] = {"target_rad": 0.5, "final_rad": final,
                           "peak_rad": max((s["position_rad"] for s in hold), default=None)}
    checks["position_holds"] = final is not None and abs(final - 0.5) < 0.02
    # 6. Velocity damping: kd only, toward 5 rad/s. Against the plant's viscous damping B
    # (SIL default 0.006 N*m*s/rad, gear 1) the steady state is kd * v / (kd + B).
    damp = stream((0.0, 5.0, 0.0, 0.02, 0.0), 1000000)
    tail = damp[len(damp) // 2:]
    mean_velocity = sum(s["velocity_rad_s"] for s in tail) / max(len(tail), 1)
    expected_velocity = 0.02 * 5.0 / (0.02 + 0.006)
    metrics["velocity"] = {"target_rad_s": 5.0, "expected_rad_s": expected_velocity,
                           "mean_rad_s": mean_velocity}
    checks["velocity_damps"] = bool(tail) and abs(mean_velocity / expected_velocity - 1.0) < 0.05
    stream((0.0, 0.0, 0.0, 0.02, 0.0), 500000)
    # 7. Feedforward torque only: the reply torque follows it.
    push = stream((0.0, 0.0, 0.0, 0.0, 0.004), 40000)
    torques = [s["torque_nm"] for s in push[len(push) // 2:]]
    mean_torque = sum(torques) / max(len(torques), 1)
    metrics["torque"] = {"target_nm": 0.004, "mean_reply_nm": mean_torque,
                         "velocity_gain_rad_s": push[-1]["velocity_rad_s"] - push[0]["velocity_rad_s"]
                         if push else None}
    checks["torque_feedforward"] = bool(torques) and abs(mean_torque - 0.004) < 0.0015 and \
        metrics["torque"]["velocity_gain_rad_s"] > 0.0
    stream((0.0, 0.0, 0.0, 0.02, 0.0), 500000)
    # 8. Stop streaming: the lease trips; the reply reports it and FB clears it.
    for _ in range(200):
        mit.step()
    tripped = mit.command(0.0, 0.0, 0.0, 0.0, 0.0)
    cleared = None
    for _ in range(50):
        cleared = mit.special("clear")
        if cleared is not None and cleared["error"] == "DISABLED":
            break
    checks["lease_trip_reported"] = tripped is not None and tripped["error"] == "COMMUNICATION_LOST"
    checks["cleared"] = cleared is not None and cleared["error"] == "DISABLED"
    # 9. Exit answers; switching back restores the CANSimple heartbeat.
    checks["exit_answers"] = mit.special("exit") is not None
    checks["switched_back"] = write("can-protocol", 0) == "APPLIED"
    beat = _can_heartbeat(client, node, lambda hb: True, 150000)
    checks["cansimple_back"] = beat is not None

    # 10. UART SETPOINT mode 3: the same law over UART.
    still_rest = None
    for _ in range(4000):
        status = describe(uart.call("telemetry"))
        if abs(status.get("velocity_rad_s", 1.0)) < 0.005:
            still_rest = status
            break
    position_now = still_rest["position_rad"] if still_rest else 0.0
    hold_payload = setpoint("impedance", position_now, 0.0, 0.0, 0.0, 0.0)
    checks["uart_set_mode"] = session.lifecycle("set-mode", SIL_STATE_DISABLED, mode="impedance") == "APPLIED"
    checks["uart_arm"] = session.lifecycle("arm", SIL_STATE_ARMED, hold_payload) == "APPLIED"
    checks["uart_start"] = session.lifecycle("start", SIL_STATE_RUNNING, hold_payload) == "APPLIED"
    target = position_now + 0.3
    accepted = 0
    for _ in range(2000):
        accepted += session.call("setpoint", setpoint("impedance", target, 0.0, 0.5, 0.01, 0.0),
                                 quiet=True)["result"] == "ACCEPTED"
    final_uart = describe(uart.call("telemetry")).get("position_rad")
    metrics["uart"] = {"target_rad": target, "final_rad": final_uart, "accepted": accepted}
    checks["uart_impedance_holds"] = final_uart is not None and abs(final_uart - target) < 0.02
    session.stop()
    device.__exit__()

    summary = {"scenario": "mit", "status": "SIL_PASS" if all(checks.values()) else "SIL_FAIL",
               "checks": checks, "metrics": metrics, "physical_applied": False}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2, allow_nan=False),
                                           encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_can_multi(simulator, base_arguments, json_path=None):
    """Two Board1 SIL nodes on one virtual CAN bus: both discovered unaddressed, assigned
    nodes 1 and 2 by serial number, cyclic periods set per node, driven at different
    velocities at the same time, and stopped together by one broadcast Estop. Nothing here
    is physical evidence."""

    checks, metrics = {}, {}
    serials = {1: 0xF0C0000001, 2: 0xF0C0000002}
    devices = [falconfoc_can.MuxSimulatorNode(simulator, list(base_arguments), serials[node])
               for node in (1, 2)]
    client = falconfoc_can.CanSimpleClient(falconfoc_can.SilCanBus(devices))
    uarts = [Client(device) for device in devices]
    for uart in uarts:
        uart.hello()
    checks["both_discovered"] = client.scan() == {serials[1]: 0x3F, serials[2]: 0x3F}
    for node, serial_number in serials.items():
        client.address(node, serial_number)
    for _ in range(10):
        client.step()
    checks["both_assigned"] = client.scan() == {serials[1]: 1, serials[2]: 2}
    checks["period_written"] = client.sdo(2, PARAMETERS["can-heartbeat-ms"], 50)[0] == 2
    mark = len(client.bus.inbox)
    for _ in range(int(0.4e6) // client.step_us):
        client.step()
    frames = client.frames_since(mark)
    beats = {node: sum(1 for f in frames if f.id == falconfoc_can.arbitration_id(node, 1))
             for node in (1, 2)}
    metrics["heartbeats_in_400ms"] = beats
    checks["periods_per_node"] = abs(beats[1] - 4) <= 1 and abs(beats[2] - 8) <= 1
    for node in (1, 2):
        client.sdo(node, PARAMETERS["command-lease-us"], 50000)
    checks["commissioned"] = all(commission(uart) == "APPLIED" for uart in uarts)
    targets = {1: 0.5, 2: -0.25}
    for node in (1, 2):
        client.set_controller_mode(node, "velocity")
    for _ in range(10):
        client.step()

    def keep():
        for node, target in targets.items():
            client.set_input(node, "velocity", target)

    for node in (1, 2):
        client.set_axis_state(node, "closed-loop")
    closed = all(_can_heartbeat(client, node, lambda hb: hb["state"] == "CLOSED_LOOP", 300000, keep)
                 for node in (1, 2))
    mark = len(client.bus.inbox)
    for _ in range(1000000 // client.step_us):
        keep()
        client.step()
    frames = client.frames_since(mark)
    means = {}
    for node in (1, 2):
        estimates = _can_estimates(frames, node)
        tail = estimates[len(estimates) // 2:]
        means[node] = sum(e["velocity_turns_s"] for e in tail) / max(len(tail), 1)
    metrics["velocity"] = {"targets_turns_s": targets, "means_turns_s": means}
    checks["independent_velocities"] = closed and all(
        abs(means[node] / targets[node] - 1.0) < 0.05 for node in (1, 2))
    client.send(falconfoc_can.UNADDRESSED, "estop")
    stopped = all(_can_heartbeat(client, node, lambda hb: hb["state"] == "IDLE"
                                 and "ESTOP_REQUESTED" in hb["error_names"], 150000)
                  for node in (1, 2))
    checks["broadcast_estop"] = stopped
    for device in devices:
        device.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-can-multi", "status":
               "SIL_PASS" if passed else "SIL_FAIL", "simulated": True, "physical_applied": False,
               "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_trajectory(simulator, base_arguments, flash_path, json_path=None):
    """Trajectory shaping (parameter contract 3.4, P1 step B): acceleration (ID 106) and
    deceleration (ID 107) shape velocity references; in position mode the planner runs a
    trapezoid on its own reference and the position loop tracks it; under a constant
    load the cascade holds position without a position integrator (ID 105 dropped);
    values outside the contract are refused; the limits survive a power cycle and return
    to 0 (unlimited) on CONFIG_ERASE. Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    checks, metrics = {}, {}
    accel, decel = 20.0, 40.0
    # Velocity PI for the SIL plant (J 0.002 kg*m^2, Kt 0.105 N*m/A) at 60 rad/s, as
    # step C derives from J: Kp = J*w/Kt, Ki = Kp*w/4. The Board1 defaults (0.18, 1.4)
    # give about 9 rad/s, too slow to follow a 40 rad/s^2 plan without visible lag.
    velocity_gains = (("velocity-kp", 1.14), ("velocity-ki", 17.0))

    def boot(extra=()):
        transport = SimulatorTransport(simulator, list(base_arguments) + ["--flash", str(flash_path)]
                                       + list(extra))
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        return transport, session

    def profile(session, mode, segments):
        """ARM, START, then stream each (target, duration) segment; telemetry samples."""
        first = setpoint(mode, segments[0][0])
        session.lifecycle("set-mode", SIL_STATE_DISABLED, mode=mode)
        steps = {"arm": session.lifecycle("arm", SIL_STATE_ARMED, first)}
        steps["start"] = (session.lifecycle("start", SIL_STATE_RUNNING, first)
                          if steps["arm"] == "APPLIED" else "SKIPPED")
        samples, started = [], None
        for index, (target, duration_us) in enumerate(segments):
            payload, began = setpoint(mode, target), None
            while steps["start"] == "APPLIED":
                session.call("setpoint", payload, quiet=True)
                telemetry = session.call("telemetry", quiet=True)
                now = telemetry["uptime_us_mod32"]
                started = now if started is None else started
                began = now if began is None else began
                samples.append({"t": ((now - started) & 0xFFFFFFFF) * 1e-6, "segment": index,
                                "position": telemetry["position_rad"],
                                "velocity": telemetry["velocity_rad_s"], "iq": telemetry["iq_a"],
                                "state": telemetry["state"], "faults": telemetry["faults"]})
                if ((now - began) & 0xFFFFFFFF) >= duration_us or telemetry["state"] != SIL_STATE_RUNNING:
                    break
        status = session.status()
        session.stop()
        _wait_still(session)
        return steps, samples, status

    def first_time(samples, segment, predicate):
        rows = [s for s in samples if s["segment"] == segment]
        start = rows[0]["t"] if rows else 0.0
        return next((s["t"] - start for s in rows if predicate(s)), None)

    def ok(steps, status):
        return steps == {"arm": "APPLIED", "start": "APPLIED"} and not status["fault_names"]

    if flash_path.exists():
        flash_path.unlink()
    transport, session = boot()
    checks["commissioned"] = commission(session.client) == "APPLIED"
    checks["unlimited_by_default"] = session.read("accel") == 0.0 and session.read("decel") == 0.0
    checks["gains_written"] = all(session.tune(name, value) == "APPLIED"
                                  for name, value in velocity_gains)

    # 1. Velocity without limits, then with them: 0 -> 10 rad/s -> 0.
    segments = [(10.0, 800000), (0.0, 600000)]
    steps, direct, status = profile(session, "velocity", segments)
    metrics["direct_t50_s"] = first_time(direct, 0, lambda s: s["velocity"] >= 5.0)
    checks["direct_runs"] = ok(steps, status)
    checks["ramp_written"] = (session.tune("accel", accel) == "APPLIED"
                              and session.tune("decel", decel) == "APPLIED")
    steps, ramped, status = profile(session, "velocity", segments)
    metrics["accel_t50_s"] = first_time(ramped, 0, lambda s: s["velocity"] >= 5.0)
    metrics["decel_t50_s"] = first_time(ramped, 1, lambda s: s["velocity"] <= 5.0)
    metrics["decel_t10_s"] = first_time(ramped, 1, lambda s: s["velocity"] <= 1.0)
    # Planned: 5 rad/s at 20 rad/s² after 0.25 s; 10 -> 5 rad/s at 40 rad/s² in 0.125 s
    # and 10 -> 1 rad/s in 0.225 s. The velocity loop adds a few ms of lag.
    checks["velocity_accel_limited"] = (
        ok(steps, status) and metrics["direct_t50_s"] is not None and metrics["direct_t50_s"] < 0.1
        and metrics["accel_t50_s"] is not None and 0.2 < metrics["accel_t50_s"] < 0.32)
    checks["velocity_decel_limited"] = (
        metrics["decel_t50_s"] is not None and 0.1 < metrics["decel_t50_s"] < 0.17
        and metrics["decel_t10_s"] is not None and 0.2 < metrics["decel_t10_s"] < 0.27)

    # 2. Position: a 6 rad move is a trapezoid of 0.95 s (0.6 s up, 0.05 s at 12 rad/s,
    # 0.3 s down). Without acceleration feedforward (step C, from J) the loop lags the plan
    # and passes the target by a few hundredths of a radian before settling.
    start = session.call("telemetry", quiet=True)["position_rad"]
    target = start + 6.0
    steps, moved, status = profile(session, "position", [(target, 2000000)])
    reached = first_time(moved, 0, lambda s: abs(target - s["position"]) < 0.01)
    half_speed = first_time(moved, 0, lambda s: s["velocity"] >= 6.0)
    tail = moved[-len(moved) // 10:] if moved else []
    metrics["position_move"] = {
        "reached_s": reached, "half_speed_s": half_speed,
        "peak_velocity": max((s["velocity"] for s in moved), default=None),
        "overshoot_rad": max((s["position"] - target for s in moved), default=None),
        "final_error_rad": (sum(abs(target - s["position"]) for s in tail) / len(tail)
                            if tail else None)}
    move = metrics["position_move"]
    checks["position_trapezoid"] = (
        ok(steps, status) and half_speed is not None and 0.2 < half_speed < 0.4
        and reached is not None and 0.8 < reached < 1.2 and move["peak_velocity"] < 12.6
        and move["overshoot_rad"] < 0.05 and move["final_error_rad"] < 0.002)

    # 3. Refusals: inside (0, 0.1) and above the contract range.
    checks["below_minimum_refused"] = session.tune("accel", 0.05) == "INVALID_ARGUMENT"
    checks["above_maximum_refused"] = session.tune("decel", 200000.0) == "INVALID_ARGUMENT"
    checks["refusal_keeps_values"] = (session.read("accel") == accel
                                      and session.read("decel") == decel)
    checks["saved"] = session.call("save")["result"] == "APPLIED"
    transport.__exit__()

    # 4. Power cycle: the limits restore; CONFIG_ERASE returns them to unlimited.
    transport, session = boot()
    checks["ramp_restored"] = session.read("accel") == accel and session.read("decel") == decel
    erased = "BUSY"
    for _ in range(4000):
        erased = session.call("erase-config", quiet=True)["result"]
        if erased != "BUSY":
            break
        session.status()
    checks["erase_resets_ramp"] = (erased == "APPLIED" and session.read("accel") == 0.0
                                   and session.read("decel") == 0.0)
    transport.__exit__()
    if flash_path.exists():
        flash_path.unlink()

    # 5. A constant 0.04 N*m load from 3 s: position P + velocity PI holds without error.
    transport, session = boot(["--load-torque-nm", "0.04,3000000"])
    commission(session.client)
    hold = session.call("telemetry", quiet=True)["position_rad"]
    steps, held, status = profile(session, "position", [(hold, 5000000)])
    tail = held[-len(held) // 10:] if held else []
    metrics["load_hold"] = {
        "tail_error_rad": (sum(hold - s["position"] for s in tail) / len(tail) if tail else None),
        "tail_iq_a": (sum(s["iq"] for s in tail) / len(tail) if tail else None),
        "max_deflection_rad": max((abs(hold - s["position"]) for s in held), default=None)}
    load = metrics["load_hold"]
    checks["load_hold_without_position_integrator"] = (
        ok(steps, status) and load["tail_iq_a"] is not None and load["tail_iq_a"] > 0.2
        and abs(load["tail_error_rad"]) < 0.001)
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-trajectory",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_frame(simulator, base_arguments, flash_path, json_path=None):
    """Output frame (parameter contract 3.7, P1 step A): host positions and speeds are
    output-side through the gear ratio (ID 99) and relative to the zero offset (ID 124);
    SET_ZERO makes the present output position zero; both are refused unless DISABLED,
    survive a power cycle and return to the boot values on CONFIG_ERASE. Telemetry bytes
    75/79 carry the rotor side for the comparison. Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    checks, metrics = {}, {}
    gear = 3.0

    def boot():
        transport = SimulatorTransport(simulator, list(base_arguments) + ["--flash", str(flash_path)])
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        return transport, session

    def frame(session):
        telemetry = session.call("telemetry", quiet=True)
        return {key: telemetry[key] for key in (
            "position_rad", "velocity_rad_s", "rotor_position_rad", "rotor_velocity_rad_s")}

    def settle(session, mode, reference, duration_us=1500000):
        session.lifecycle("set-mode", SIL_STATE_DISABLED, mode=mode)
        steps, samples = session.run(mode, reference, duration_us)
        running = [s for s in samples if s["state"] == SIL_STATE_RUNNING]
        return steps, running, session.status()

    def stop(session):
        # Still at the rotor: telemetry speed is output-side, a third of it here.
        session.stop()
        for _ in range(20000):
            if abs(session.call("telemetry", quiet=True)["rotor_velocity_rad_s"]) < 0.05:
                break

    if flash_path.exists():
        flash_path.unlink()

    # 1. Boot values: gear 1 and zero 0 leave the output frame equal to the rotor frame.
    transport, session = boot()
    checks["commissioned"] = commission(session.client) == "APPLIED"
    first = metrics["boot_frame"] = frame(session)
    checks["boot_frame_is_rotor_frame"] = (
        abs(first["position_rad"] - first["rotor_position_rad"]) < 1e-6
        and session.read("output-zero") == 0.0 and session.read("gear-ratio") == 1.0)

    # 2. Gear ratio 3: an output position target moves the rotor three times as far.
    checks["gear_applied"] = session.tune("gear-ratio", gear) == "APPLIED"
    steps, running, status = settle(session, "position", 1.0)
    held = metrics["position_1_rad"] = frame(session)
    checks["output_position_tracks"] = (
        steps == {"arm": "APPLIED", "start": "APPLIED"} and not status["fault_names"]
        and abs(held["position_rad"] - 1.0) < 0.05 and abs(held["rotor_position_rad"] - 3.0) < 0.15)
    # While running, SET_ZERO and the frame parameters are refused.
    session.call("setpoint", setpoint("position", 1.0), quiet=True)
    checks["set_zero_refused_while_running"] = session.call("set-zero")["result"] == "BAD_STATE"
    checks["zero_write_refused_while_running"] = session.call("param-write", parameter_write(
        PARAMETERS["output-zero"], "f32", 0.5))["result"] == "BAD_STATE"
    checks["still_running"] = session.status()["state"] == SIL_STATE_RUNNING
    stop(session)

    # 3. Output speed: 2 rad/s output is 6 rad/s at the rotor; 5 rad/s output (15 at the
    # rotor) exceeds the 12 rad/s rotor speed limit (ID 72) and is refused.
    steps, running, status = settle(session, "velocity", 2.0)
    tail = running[len(running) * 3 // 4:]
    spun = metrics["velocity_2_rad_s"] = frame(session)
    metrics["velocity_tail_error"] = (sum(abs(2.0 - s["velocity_rad_s"]) for s in tail) / len(tail)
                                      if tail else None)
    checks["output_velocity_tracks"] = (
        steps == {"arm": "APPLIED", "start": "APPLIED"} and not status["fault_names"]
        and metrics["velocity_tail_error"] is not None and metrics["velocity_tail_error"] < 0.2
        and abs(spun["rotor_velocity_rad_s"] - gear * spun["velocity_rad_s"]) < 1e-3)
    checks["rotor_speed_limit_applies_through_gear"] = session.call(
        "setpoint", setpoint("velocity", 5.0))["result"] == "INVALID_ARGUMENT"
    stop(session)

    # 4. SET_ZERO: the present output position becomes zero.
    zeroed = session.call("set-zero")
    after = metrics["after_set_zero"] = frame(session)
    metrics["zero_rad"] = zeroed.get("zero_rad")
    checks["set_zero_applied"] = (
        zeroed["result"] == "APPLIED"
        and abs(zeroed["zero_rad"] - after["rotor_position_rad"] / gear) < 0.01
        and abs(after["position_rad"]) < 0.01
        and abs(session.read("output-zero") - zeroed["zero_rad"]) < 1e-6)
    steps, running, status = settle(session, "position", 0.5)
    moved = metrics["position_0_5_from_zero"] = frame(session)
    checks["position_relative_to_zero"] = (
        not status["fault_names"] and abs(moved["position_rad"] - 0.5) < 0.05
        and abs(moved["rotor_position_rad"] - gear * (0.5 + zeroed["zero_rad"])) < 0.15)
    stop(session)
    checks["saved"] = session.call("save")["result"] == "APPLIED"
    transport.__exit__()

    # 5. Power cycle: the zero and the gear ratio restore. The SIL plant restarts at its
    # initial angle, so the output position is that angle in the restored frame.
    transport, session = boot()
    restored = metrics["restored_frame"] = frame(session)
    checks["frame_restored"] = (
        abs(session.read("output-zero") - zeroed["zero_rad"]) < 1e-6
        and session.read("gear-ratio") == gear
        and abs(restored["position_rad"]
                - (restored["rotor_position_rad"] / gear - zeroed["zero_rad"])) < 1e-4)

    # 6. CONFIG_ERASE returns the frame to the boot values at once. It is BUSY while the
    # boot's zero-only CAL-1 owns the Runtime.
    erased_result = "BUSY"
    for _ in range(4000):
        erased_result = session.call("erase-config", quiet=True)["result"]
        if erased_result != "BUSY":
            break
        session.status()
    checks["erase_applied"] = erased_result == "APPLIED"
    erased = metrics["erased_frame"] = frame(session)
    checks["erase_resets_frame"] = (
        session.read("output-zero") == 0.0 and session.read("gear-ratio") == 1.0
        and abs(erased["position_rad"] - erased["rotor_position_rad"]) < 1e-6)
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-frame",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def run_sil_swap(simulator, base_arguments, flash_path, json_path=None):
    """Motor replacement end to end (parameter contract step 3): the placeholder is gated,
    motor-setup commissions motor A, a power cycle restores it and measures the zero, motor B
    on A's stored profile is caught by protection, motor-setup recommissions B, an exported
    group imports into an erased board, ID 17 trims the offset and CONFIG_ERASE closes the
    gate again. Nothing here is physical evidence."""

    flash_path = pathlib.Path(flash_path)
    checks, metrics = {}, {}
    motor_a = dict(SIL_CALIBRATE_PLANT)
    # A different motor inside the CAL-3 range (0.35 Ω / 0.7 mH is outside it today).
    motor_b = dict(SIL_CALIBRATE_PLANT, pole_pairs=14, offset_rad=0.4, rs_ohm=0.6,
                   ld_h=0.0012, lq_h=0.0015, flux_wb=0.01)

    def plant(values):
        return ["--motor-pole-pairs", str(values["pole_pairs"]),
                "--motor-offset-rad", str(values["offset_rad"]),
                "--adc-offset", ",".join(map(str, values["adc_offset"])),
                "--motor-rs-ohm", str(values["rs_ohm"]), "--motor-ld-h", str(values["ld_h"]),
                "--motor-lq-h", str(values["lq_h"]), "--motor-psi-f-wb", str(values["flux_wb"])]

    def boot(values):
        transport = SimulatorTransport(simulator, list(base_arguments) + plant(values)
                                       + ["--flash", str(flash_path)])
        client = Client(transport)
        client.hello()
        session = SilSession(client, transport)
        session.status()
        session.tune("command-lease-us", 50000)
        return transport, session

    def commission_state(session):
        return {name: session.read(name) for name in (
            "commutation-valid", "commutation-source", "adc-zero-state", "pole-pairs")}

    def velocity(session, duration_us=800000):
        session.lifecycle("set-mode", SIL_STATE_DISABLED, mode="velocity")
        steps, samples = session.run("velocity", 5.0, duration_us)
        running = [s for s in samples if s["state"] == SIL_STATE_RUNNING]
        tail = running[len(running) * 3 // 4:]
        status = session.status()
        session.stop()
        _wait_still(session)
        return {"steps": steps, "faults": status["fault_names"],
                "tail_error": sum(abs(5.0 - s["velocity_rad_s"]) for s in tail) / len(tail)
                if tail else None}

    def tracks(result):
        return (result["steps"] == {"arm": "APPLIED", "start": "APPLIED"} and not result["faults"]
                and result["tail_error"] is not None and result["tail_error"] < 0.3)

    if flash_path.exists():
        flash_path.unlink()

    # 1. Placeholder: the gate refuses ARM.
    transport, session = boot(motor_a)
    metrics["fresh"] = commission_state(session)
    refused = session.lifecycle("arm", SIL_STATE_ARMED, setpoint("current", 0.0, 0.2))
    checks["placeholder_gated"] = (refused == "NOT_CALIBRATED"
                                   and metrics["fresh"]["commutation-valid"] == 0)
    # 2. motor-setup commissions motor A and saves.
    setup = metrics["setup_a"] = motor_setup(session.client, rated_current_a=1.0,
                                             peak_current_a=2.0, max_speed_rad_s=60.0)
    checks["motor_a_setup"] = setup["status"] == "OK"
    transport.__exit__()

    # 3. Power cycle: restored, zero measured on the MCU, the drive tracks.
    transport, session = boot(motor_a)
    restored = velocity(session)
    metrics["restored_a"] = dict(restored, **commission_state(session))
    checks["motor_a_restored_runs"] = (tracks(restored)
                                       and metrics["restored_a"]["commutation-source"] == 2
                                       and metrics["restored_a"]["adc-zero-state"] == 1)
    group_a = {"pole_pairs": session.read("pole-pairs"),
               "offset": session.read("electrical-offset"),
               "direction": 1 if session.read("runtime-direction") == 1 else -1}
    transport.__exit__()

    # 4. Motor B on A's stored profile: the gate opens (restored group) and protection trips.
    transport, session = boot(motor_b)
    wrong = metrics["motor_b_on_profile_a"] = velocity(session, 1500000)
    checks["swap_without_setup_trips"] = bool(set(wrong["faults"]) & {
        "TRACKING_DIVERGENCE", "SUSTAINED_CURRENT", "STALL", "OVER_SPEED"})
    session.call("clear")
    for _ in range(400):
        if session.status()["state"] == SIL_STATE_DISABLED:
            break
    _wait_still(session)
    # 5. motor-setup recommissions motor B; a power cycle runs it.
    setup = metrics["setup_b"] = motor_setup(session.client, rated_current_a=0.8,
                                             peak_current_a=1.5, max_speed_rad_s=50.0)
    checks["motor_b_setup"] = (setup["status"] == "OK"
                               and setup["calibration"]["pole_pairs"] == motor_b["pole_pairs"])
    transport.__exit__()
    transport, session = boot(motor_b)
    checks["motor_b_runs_after_setup"] = tracks(velocity(session))

    # 6. CONFIG_ERASE closes the gate at once.
    checks["erase_applied"] = session.call("erase-config")["result"] == "APPLIED"
    metrics["after_erase"] = commission_state(session)
    checks["erase_closes_gate"] = (
        metrics["after_erase"]["commutation-valid"] == 0 and metrics["after_erase"]["pole-pairs"] == 7
        and session.lifecycle("arm", SIL_STATE_ARMED, setpoint("current", 0.0, 0.2))
        == "NOT_CALIBRATED")
    transport.__exit__()

    # 7. Offline import: motor A's exported group into the erased board, then an ID 17 trim.
    transport, session = boot(motor_a)
    checks["trim_refused_without_group"] = session.call("param-write", parameter_write(
        PARAMETERS["electrical-offset"], "f32", 1.0))["result"] == "BAD_STATE"
    imported = commission(session.client, group_a["pole_pairs"], group_a["direction"],
                          group_a["offset"])
    checks["import_applied"] = imported == "APPLIED" and session.read("commutation-source") == 3
    checks["imported_group_runs"] = tracks(velocity(session))
    trimmed = (group_a["offset"] + 0.01) % (2.0 * math.pi)
    checks["offset_trim_applied"] = (session.call("param-write", parameter_write(
        PARAMETERS["electrical-offset"], "f32", trimmed))["result"] == "APPLIED"
        and abs(session.read("electrical-offset") - trimmed) < 1e-5)
    transport.__exit__()

    passed = all(checks.values())
    summary = {"kind": "summary", "demo": "board1-sil-swap",
               "status": "SIL_PASS" if passed else "SIL_FAIL", "simulated": True,
               "physical_applied": False, "checks": checks,
               "failed_checks": sorted(name for name, value in checks.items() if not value),
               "metrics": metrics}
    if json_path:
        pathlib.Path(json_path).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return summary


def can_command(args, parser):
    """One CANSimple command over a USB-CAN adapter (python-can) or a fresh SIL node."""
    if args.can_command is None:
        parser.error("can requires --can-command")
    if args.can_interface:
        bus = falconfoc_can.PythonCanBus(args.can_interface, args.can_channel, args.can_bitrate)
        devices = []
    elif args.simulator:
        arguments = ["--lockstep-us", str(args.sil_lockstep_us or 500)]
        if args.sil_flash:
            arguments += ["--flash", args.sil_flash]
        devices = [falconfoc_can.MuxSimulatorNode(os.path.abspath(args.simulator), arguments,
                                                  args.can_serial)]
        bus = falconfoc_can.SilCanBus(devices)
    else:
        parser.error("can requires --can-interface or --simulator")
    client = falconfoc_can.CanSimpleClient(bus, step_us=500 if devices else 2000)
    out = {"command": args.can_command, "node": args.node}
    try:
        if args.can_command == "scan":
            out["devices"] = [{"serial": hex(serial), "node": node}
                              for serial, node in client.scan(50000).items()]
        elif args.can_command == "assign":
            client.address(args.node, args.can_serial)
            reply = client.wait(lambda f: f.command == 6 and len(f.data) == 8
                                and int.from_bytes(f.data[1:7], "little") == args.can_serial,
                                50000)
            out["assigned"] = reply is not None and reply.data[0] == args.node
        elif args.can_command == "state":
            client.set_axis_state(args.node, args.axis_state)
        elif args.can_command == "mode":
            client.set_controller_mode(args.node, args.control, args.input_mode)
        elif args.can_command == "set":
            mode = {"torque": "torque", "velocity": "velocity", "position": "position"}[args.control]
            client.set_input(args.node, mode, args.value)
        elif args.can_command == "save":
            client.send(args.node, "reboot", b"\x01")
        elif args.can_command.startswith("mit-"):
            mit = falconfoc_can.MitClient(bus, args.node, args.master_id, step_us=client.step_us)
            if args.can_command == "mit-cmd":
                out["reply"] = mit.command(args.position, args.velocity, args.kp, args.kd,
                                           args.torque, steps=10)
            else:
                out["reply"] = mit.special(args.can_command[4:], steps=10)
        elif args.can_command in ("read", "write"):
            identifier = PARAMETERS[args.param] if args.param else args.id
            kind = PARAMETER_TYPES.get(identifier, "f32") if args.param else args.type
            result, raw = client.sdo(args.node, identifier,
                                     args.value if args.can_command == "write" else None, kind)
            out.update(parameter=identifier, result=RESULTS[result] if result is not None and result < len(RESULTS) else result,
                       raw=raw)
            if raw is not None and kind == "f32":
                out["value"] = struct.unpack("<f", struct.pack("<I", raw))[0]
            elif raw is not None:
                out["value"] = raw
        if args.can_command == "watch":
            mark = len(bus.inbox)
            client.step(args.duration_us or 200000)
            out["frames"] = [falconfoc_can.decode_frame(frame)
                             for frame in client.frames_since(mark)
                             if args.node == 0x3F or frame.node == args.node]
        else:
            client.step(10000)
        print(json.dumps(out, allow_nan=False))
        return 0
    finally:
        for device in devices:
            device.__exit__()
        if not devices:
            bus.__exit__()


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
                 "calibrate", "motor-setup", "can"],
    )
    parser.add_argument("message", nargs="?", choices=sorted(MESSAGES))
    parser.add_argument("--hex", default="", help="Wire hex for decode, including delimiter")
    parser.add_argument("--can-command", choices=("scan", "assign", "state", "mode", "set", "read",
                                                  "write", "watch", "save", "mit-enter", "mit-exit", "mit-zero", "mit-clear", "mit-cmd"),
                        help="can: CANSimple command")
    parser.add_argument("--can-interface", help="can: python-can interface (slcan, gs_usb, ...)")
    parser.add_argument("--master-id", type=lambda text: int(text, 0), default=0,
                        help="can mit-*: CAN ID of the replies (ID 121)")
    parser.add_argument("--mit-position", type=float, default=0.0, dest="position",
                        help="can mit-cmd: output position in rad")
    parser.add_argument("--mit-velocity", type=float, default=0.0, dest="velocity",
                        help="can mit-cmd: output velocity in rad/s")
    parser.add_argument("--kp", type=float, default=0.0, help="can mit-cmd: stiffness N*m/rad")
    parser.add_argument("--kd", type=float, default=0.0, help="can mit-cmd: damping N*m*s/rad")
    parser.add_argument("--torque", type=float, default=0.0, help="can mit-cmd: torque N*m")
    parser.add_argument("--can-channel", help="can: python-can channel (COM5, can0, ...)")
    parser.add_argument("--can-bitrate", type=int, default=1000000)
    parser.add_argument("--can-serial", type=lambda value: int(value, 0), default=0xF0C0000001,
                        help="can: device serial number (assign; the SIL node's serial)")
    parser.add_argument("--node", type=lambda value: int(value, 0), default=0x3F,
                        help="can: CANSimple node ID (0x3F unaddressed)")
    parser.add_argument("--axis-state", choices=sorted(falconfoc_can.AXIS_STATES),
                        default="idle", help="can state: requested axis state")
    parser.add_argument("--control", choices=sorted(falconfoc_can.CONTROL_MODES),
                        default="velocity", help="can mode/set: control mode")
    parser.add_argument("--input-mode", choices=sorted(falconfoc_can.INPUT_MODES),
                        default="passthrough", help="can mode: input mode")
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
    parser.add_argument("--rated-current", type=float, default=None,
                        help="motor-setup: rated phase current in A (ID 96)")
    parser.add_argument("--peak-current", type=float, default=None,
                        help="motor-setup: peak phase current in A (ID 97)")
    parser.add_argument("--max-speed", type=float, default=None,
                        help="motor-setup: maximum mechanical speed in rad/s (ID 98)")
    parser.add_argument("--bandwidth", type=float, default=None,
                        help="motor-setup: current-loop bandwidth in Hz (ID 104)")
    parser.add_argument("--no-save", action="store_true", help="motor-setup: skip PARAM_SAVE")
    parser.add_argument("--mechanical", action="store_true",
                        help="motor-setup: also identify inertia and damping (CAL-7; the shaft "
                             "turns within ID 109 with its load)")
    parser.add_argument("--cal-stages", choices=sorted(CAL_STAGES),
                        help="request calibrate: CALIBRATE_START stage mask")
    parser.add_argument("--sil-flash", default=None,
                        help="sil-demo persist: flash image file (recreated)")
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
        if args.action == "can":
            return can_command(args, parser)
        payload = b""
        if args.message == "setpoint":
            payload = setpoint(args.mode, args.first, args.second)
        elif args.message == "set-mode":
            payload = set_mode(args.mode)
        elif args.message == "calibrate" and args.cal_stages:
            payload = bytes([CAL_STAGES[args.cal_stages]])
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
            if args.action in ("sil-demo", "motor-setup"):
                simulator_arguments = (
                    ["--lockstep-us", str(args.sil_lockstep_us)] if args.sil_lockstep_us > 0
                    else ["--speed", str(args.sil_speed)]
                )
            if args.action == "sil-demo" and args.sil_scenario == "calibrate":
                simulator_arguments += SIL_CALIBRATE_ARGUMENTS
            if args.action == "sil-demo" and args.sil_scenario in ("can", "can-multi", "mit"):
                if args.sil_lockstep_us <= 0:
                    parser.error("the CAN scenarios step a virtual bus: give --sil-lockstep-us")
                if args.sil_scenario == "mit":
                    flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                        "sil-mit-flash.bin"))
                    summary = run_sil_mit(os.path.abspath(args.simulator), simulator_arguments,
                                          flash, args.json)
                elif args.sil_scenario == "can-multi":
                    summary = run_sil_can_multi(os.path.abspath(args.simulator),
                                                simulator_arguments, args.json)
                else:
                    flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                        "sil-can-flash.bin"))
                    summary = run_sil_can(os.path.abspath(args.simulator), simulator_arguments,
                                          flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "dead-time-comp":
                flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                    "sil-dead-time-comp-flash.bin"))
                summary = run_sil_dead_time_comp(os.path.abspath(args.simulator),
                                                 simulator_arguments, flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "dead-time":
                summary = run_sil_dead_time(os.path.abspath(args.simulator),
                                            simulator_arguments, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "coast":
                summary = run_sil_coast(os.path.abspath(args.simulator), simulator_arguments,
                                        args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "driver-fault":
                summary = run_sil_driver_fault(os.path.abspath(args.simulator),
                                               simulator_arguments, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "rotor-range":
                summary = run_sil_rotor_range(os.path.abspath(args.simulator),
                                              simulator_arguments, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "inertia":
                flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                    "sil-inertia-flash.bin"))
                summary = run_sil_inertia(os.path.abspath(args.simulator), simulator_arguments,
                                          flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "trajectory":
                flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                    "sil-trajectory-flash.bin"))
                summary = run_sil_trajectory(os.path.abspath(args.simulator), simulator_arguments,
                                             flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "frame":
                flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                    "sil-frame-flash.bin"))
                summary = run_sil_frame(os.path.abspath(args.simulator), simulator_arguments,
                                        flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "swap":
                flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                    "sil-swap-flash.bin"))
                summary = run_sil_swap(os.path.abspath(args.simulator), simulator_arguments,
                                       flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "profile":
                flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                    "sil-profile-flash.bin"))
                summary = run_sil_profile(os.path.abspath(args.simulator), simulator_arguments,
                                          flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "protect":
                summary = run_sil_protect(os.path.abspath(args.simulator), simulator_arguments,
                                          args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "calibrate-faults":
                flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                    "sil-faults-flash.bin"))
                summary = run_sil_calibrate_faults(os.path.abspath(args.simulator),
                                                   simulator_arguments, flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "calibrate-persist":
                flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                    "sil-calibrate-flash.bin"))
                summary = run_sil_calibrate_persist(os.path.abspath(args.simulator),
                                                    simulator_arguments, flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            if args.action == "sil-demo" and args.sil_scenario == "persist":
                flash = args.sil_flash or str(pathlib.Path(args.simulator).resolve().with_name(
                    "sil-persist-flash.bin"))
                summary = run_sil_persist(os.path.abspath(args.simulator), simulator_arguments,
                                          flash, args.json)
                return 0 if summary["status"] == "SIL_PASS" else 3
            transport = (
                SimulatorTransport(os.path.abspath(args.simulator), simulator_arguments)
                if args.simulator
                else SerialTransport(args.port)
            )
            if args.action == "motor-setup" and args.simulator and args.sil_flash:
                transport = SimulatorTransport(os.path.abspath(args.simulator),
                                               simulator_arguments + ["--flash", args.sil_flash])
            with transport:
                client = Client(transport)
                hello = client.hello()
                if args.action == "motor-setup":
                    summary = motor_setup(client, args.rated_current, args.peak_current,
                                          args.max_speed, args.bandwidth, not args.no_save,
                                          mechanical=args.mechanical)
                    print(json.dumps(summary, allow_nan=False))
                    return 0 if summary["status"] == "OK" else 3
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
                elif args.action == "sil-demo" and args.sil_scenario == "calibrate":
                    summary = run_sil_calibrate(client, transport, args.json)
                    if summary["status"] != "SIL_PASS":
                        return 3
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
