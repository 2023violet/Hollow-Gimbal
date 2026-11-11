"""FalconFOC V1 wire codec, JSON CLI and optional serial connection."""

import argparse
import binascii
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

    def __init__(self, executable):
        self.process = subprocess.Popen(
            [executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        self.received = queue.Queue(maxsize=512)
        self.reader = threading.Thread(target=self._receive, daemon=True)
        self.reader.start()

    def _receive(self):
        while True:
            byte = self.process.stdout.read(1)
            self.received.put(byte)
            if not byte:
                return

    def write(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def read(self):
        try:
            return self.received.get(timeout=1.0)
        except queue.Empty as error:
            raise TimeoutError("Device response timed out") from error

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
        self.process.stdout.close()


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    connection = parser.add_mutually_exclusive_group()
    connection.add_argument("--port", help="Optional pyserial port. Connects when selected.")
    connection.add_argument("--simulator", help="Host foc-serial-sim executable")
    parser.add_argument("action", choices=["encode", "decode", "vectors", "request", "demo"])
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
    args = parser.parse_args()
    try:
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
                parser.error("request/demo requires --simulator or --port")
            if args.action == "request" and args.message is None:
                parser.error("request requires a message")
            transport = SimulatorTransport(args.simulator) if args.simulator else SerialTransport(args.port)
            with transport:
                client = Client(transport)
                hello = client.hello()
                if args.action == "demo":
                    print(json.dumps(describe(hello), allow_nan=False))
                    for name in ("capability", "status", "fault", "telemetry", "arm", "save", "stop"):
                        print(json.dumps(describe(client.call(name)), allow_nan=False))
                else:
                    response = hello if args.message == "hello" else client.call(args.message, payload)
                    print(json.dumps(describe(response), allow_nan=False))
    except (ValueError, OSError, TimeoutError, OverflowError, struct.error, ImportError) as error:
        print(json.dumps({"error": str(error)}), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
