"""CANSimple Host side of FalconFOC (parameter contract section 25).

Frame codec, a virtual CAN bus of Board1 SIL devices over multiplexed stdio, an optional
python-can adapter for a real USB-CAN interface, and a small CANSimple client. CAN IDs are
11-bit: node << 5 | command. Positions are output turns, velocities output turns/s and
torques output N*m; parameters are read and written through RxSdo/TxSdo with FalconFOC IDs.
"""

import collections
import json
import os
import queue
import struct
import subprocess
import threading
import time

COMMANDS = {
    "get-version": 0x00, "heartbeat": 0x01, "estop": 0x02, "get-error": 0x03,
    "rx-sdo": 0x04, "tx-sdo": 0x05, "address": 0x06, "set-axis-state": 0x07,
    "get-encoder-estimates": 0x09, "set-controller-mode": 0x0B, "set-input-pos": 0x0C,
    "set-input-vel": 0x0D, "set-input-torque": 0x0E, "set-limits": 0x0F,
    "set-traj-vel-limit": 0x11, "set-traj-accel-limits": 0x12, "get-iq": 0x14,
    "get-temperature": 0x15, "reboot": 0x16, "get-bus-voltage-current": 0x17,
    "clear-errors": 0x18, "set-absolute-position": 0x19, "set-pos-gain": 0x1A,
    "set-vel-gains": 0x1B, "get-torques": 0x1C,
}
AXIS_STATES = {"idle": 1, "full-calibration": 3, "motor-calibration": 4,
               "encoder-offset-calibration": 7, "closed-loop": 8}
AXIS_STATE_NAMES = {0: "UNDEFINED", 1: "IDLE", 3: "FULL_CALIBRATION", 4: "MOTOR_CALIBRATION",
                    7: "ENCODER_OFFSET_CALIBRATION", 8: "CLOSED_LOOP"}
CONTROL_MODES = {"torque": 1, "velocity": 2, "position": 3}
INPUT_MODES = {"passthrough": 1, "vel-ramp": 2, "trap-traj": 5}
PROCEDURES = {0: "SUCCESS", 1: "BUSY", 2: "CANCELLED", 3: "DISARMED", 4: "NO_RESPONSE",
              5: "POLE_PAIR_CPR_MISMATCH", 6: "PHASE_RESISTANCE_OUT_OF_RANGE", 11: "TIMEOUT",
              13: "INVALID_STATE", 14: "NOT_CALIBRATED", 15: "NOT_CONVERGING"}
ERRORS = {0x2: "SYSTEM_LEVEL", 0x4: "TIMING_ERROR", 0x8: "MISSING_ESTIMATE", 0x20: "DRV_FAULT",
          0x100: "DC_BUS_OVER_VOLTAGE", 0x200: "DC_BUS_UNDER_VOLTAGE",
          0x1000: "CURRENT_LIMIT_VIOLATION", 0x8000: "VELOCITY_LIMIT_VIOLATION",
          0x1000000: "WATCHDOG_TIMER_EXPIRED", 0x2000000: "ESTOP_REQUESTED",
          0x40000000: "CALIBRATION_ERROR"}
LINK_STATES = {0: "OFF", 1: "ACTIVE", 2: "PASSIVE", 3: "BUS_OFF", 4: "NO_CRYSTAL"}
UNADDRESSED = 0x3F
CHANNEL_UART, CHANNEL_CAN, CHANNEL_TICK = 0, 1, 2


def arbitration_id(node, command):
    return (node & 0x3F) << 5 | (command & 0x1F)


class CanFrame(collections.namedtuple("CanFrame", "id data rtr")):
    """One classic CAN frame with an 11-bit identifier."""

    def __new__(cls, id, data=b"", rtr=False):
        return super().__new__(cls, id, bytes(data), bool(rtr))

    @property
    def node(self):
        return self.id >> 5

    @property
    def command(self):
        return self.id & 0x1F


def error_names(mask):
    return [name for bit, name in ERRORS.items() if mask & bit]


def decode_frame(frame):
    """Human-readable CANSimple content of a device frame."""
    data = frame.data
    out = {"node": frame.node, "command": frame.command}
    if frame.command == COMMANDS["heartbeat"] and len(data) >= 7:
        errors, state, procedure, done = struct.unpack_from("<IBBB", data)
        out.update(kind="heartbeat", errors=errors, error_names=error_names(errors),
                   state=AXIS_STATE_NAMES.get(state, state),
                   procedure=PROCEDURES.get(procedure, procedure), trajectory_done=bool(done))
    elif frame.command == COMMANDS["get-encoder-estimates"] and len(data) == 8:
        position, velocity = struct.unpack("<ff", data)
        out.update(kind="estimates", position_turns=position, velocity_turns_s=velocity)
    elif frame.command == COMMANDS["address"] and len(data) == 8:
        out.update(kind="address", node_id=data[0],
                   serial=int.from_bytes(data[1:7], "little"))
    elif frame.command == COMMANDS["tx-sdo"] and len(data) == 8:
        result, endpoint, value = struct.unpack("<BHxI", data)
        out.update(kind="tx-sdo", result=result, endpoint=endpoint, value=value)
    elif frame.command == COMMANDS["get-error"] and len(data) == 8:
        active, disarm = struct.unpack("<II", data)
        out.update(kind="error", active=active, active_names=error_names(active),
                   disarm_reason=disarm)
    elif len(data) == 8 and frame.command in (COMMANDS["get-iq"], COMMANDS["get-torques"],
                                              COMMANDS["get-bus-voltage-current"],
                                              COMMANDS["get-temperature"]):
        out.update(kind="pair", values=list(struct.unpack("<ff", data)))
    else:
        out.update(kind="raw", data=data.hex())
    return out


class MuxSimulatorNode:
    """One Board1 SIL device started with --can: UART bytes and CAN frames share stdio.
    It also works as a UART transport for the existing Client (read/write)."""

    def __init__(self, executable, arguments, serial_number):
        command = [executable] + list(arguments) + ["--can", hex(serial_number)]
        self.serial_number = serial_number
        self.process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        self.received = queue.Queue()
        self.frames = queue.Queue()
        self.acks = queue.Queue()
        self.metadata = queue.Queue(maxsize=512)
        self.reader = threading.Thread(target=self._receive, daemon=True)
        self.metadata_reader = threading.Thread(target=self._receive_metadata, daemon=True)
        self.reader.start()
        self.metadata_reader.start()

    def _read_exact(self, size):
        data = b""
        while len(data) < size:
            chunk = self.process.stdout.read(size - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def _receive(self):
        while True:
            header = self._read_exact(2)
            payload = self._read_exact(header[1]) if header is not None else None
            if payload is None:
                self.received.put(b"")
                self.acks.put(None)
                return
            if header[0] == CHANNEL_UART:
                for byte in payload:
                    self.received.put(bytes([byte]))
            elif header[0] == CHANNEL_CAN and len(payload) == 12:
                identifier, flags, length = struct.unpack_from("<HBB", payload)
                self.frames.put(CanFrame(identifier & 0x7FF, payload[4:4 + min(length, 8)],
                                         flags & 1))
            elif header[0] == CHANNEL_TICK and len(payload) == 4:
                self.acks.put(struct.unpack("<I", payload)[0])

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

    def _record(self, channel, payload):
        self.process.stdin.write(bytes([channel, len(payload)]) + payload)
        self.process.stdin.flush()

    def write(self, data):
        for start in range(0, len(data), 255):
            self._record(CHANNEL_UART, data[start:start + 255])

    def read(self):
        try:
            return self.received.get(timeout=2.0)
        except queue.Empty as error:
            raise TimeoutError("Device response timed out") from error

    def drain_metadata(self):
        items = []
        while True:
            try:
                items.append(self.metadata.get_nowait())
            except queue.Empty:
                return items

    def send_frame(self, frame):
        data = frame.data.ljust(8, b"\0")[:8]
        self._record(CHANNEL_CAN, struct.pack("<HBB", frame.id, 1 if frame.rtr else 0,
                                              len(frame.data)) + data)

    def tick(self, us):
        self._record(CHANNEL_TICK, struct.pack("<I", us))

    def wait_ack(self, timeout=10.0):
        try:
            now = self.acks.get(timeout=timeout)
        except queue.Empty as error:
            raise TimeoutError("SIL time advance not acknowledged") from error
        if now is None:
            raise TimeoutError("SIL device closed")
        return now

    def drain_frames(self):
        frames = []
        while True:
            try:
                frames.append(self.frames.get_nowait())
            except queue.Empty:
                return frames

    def __enter__(self):
        return self

    def __exit__(self, *_):
        try:
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=2)
        self.reader.join(timeout=1)
        self.metadata_reader.join(timeout=1)
        self.process.stdout.close()
        self.process.stderr.close()


class SilCanBus:
    """Virtual bus of SIL nodes in lockstep: a step advances every node by the same time,
    then each frame a node sent reaches every other node (next step) and the Host."""

    def __init__(self, nodes):
        self.nodes = list(nodes)
        self.inbox = []
        self.now_us = 0

    def send(self, frame):
        for node in self.nodes:
            node.send_frame(frame)

    def step(self, us):
        for node in self.nodes:
            node.tick(us)
        for node in self.nodes:
            node.wait_ack()
        self.now_us += us
        for index, node in enumerate(self.nodes):
            for frame in node.drain_frames():
                for other in self.nodes:
                    if other is not node:
                        other.send_frame(frame)
                self.inbox.append((self.now_us, frame))


class PythonCanBus:
    """Optional real adapter through python-can (slcan, gs_usb/candleLight, pcan ...).
    Import is deferred until selected. Not exercised without hardware."""

    def __init__(self, interface, channel, bitrate=1000000):
        import can
        self.can = can
        self.bus = can.interface.Bus(interface=interface, channel=channel, bitrate=bitrate)
        self.inbox = []
        self.started = time.monotonic()
        self.now_us = 0

    def send(self, frame):
        self.bus.send(self.can.Message(arbitration_id=frame.id, data=frame.data,
                                       is_extended_id=False, is_remote_frame=frame.rtr,
                                       dlc=len(frame.data)))

    def step(self, us):
        deadline = time.monotonic() + us * 1e-6
        while True:
            remaining = deadline - time.monotonic()
            message = self.bus.recv(timeout=max(remaining, 0.0))
            if message is not None and not message.is_extended_id:
                self.inbox.append((int((time.monotonic() - self.started) * 1e6),
                                   CanFrame(message.arbitration_id, bytes(message.data),
                                            message.is_remote_frame)))
            if remaining <= 0:
                break
        self.now_us = int((time.monotonic() - self.started) * 1e6)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.bus.shutdown()


class CanSimpleClient:
    """CANSimple requests over a bus (SilCanBus or PythonCanBus)."""

    def __init__(self, bus, step_us=500):
        self.bus, self.step_us = bus, step_us
        self.mark = 0

    def send(self, node, command, data=b"", rtr=False):
        if isinstance(command, str):
            command = COMMANDS[command]
        self.bus.send(CanFrame(arbitration_id(node, command), data, rtr))

    def step(self, us=None):
        self.bus.step(self.step_us if us is None else us)

    def frames_since(self, mark):
        return [frame for _, frame in self.bus.inbox[mark:]]

    def wait(self, predicate, timeout_us, keep_alive=None):
        """Step until a received frame satisfies predicate; keep_alive() runs every step."""
        mark = len(self.bus.inbox)
        waited = 0
        while waited <= timeout_us:
            for _, frame in self.bus.inbox[mark:]:
                if predicate(frame):
                    return frame
            mark = len(self.bus.inbox)
            if keep_alive is not None:
                keep_alive()
            self.step()
            waited += self.step_us
        return None

    def request(self, node, command, timeout_us=20000):
        if isinstance(command, str):
            command = COMMANDS[command]
        self.send(node, command, rtr=True)
        identifier = arbitration_id(node, command)
        return self.wait(lambda frame: frame.id == identifier and not frame.rtr, timeout_us)

    def sdo(self, node, parameter, value=None, kind="u32", timeout_us=20000):
        """Read (value None) or write a FalconFOC parameter. Returns (result, raw u32)."""
        if value is None:
            payload = struct.pack("<BHBI", 0, parameter, 0, 0)
        else:
            raw = struct.unpack("<I", struct.pack("<f", value))[0] if kind == "f32" else int(value)
            payload = struct.pack("<BHBI", 1, parameter, 0, raw)
        self.send(node, "rx-sdo", payload)
        identifier = arbitration_id(node, COMMANDS["tx-sdo"])
        reply = self.wait(lambda frame: frame.id == identifier and len(frame.data) == 8
                          and struct.unpack_from("<H", frame.data, 1)[0] == parameter, timeout_us)
        if reply is None:
            return None, None
        result, _, raw = struct.unpack("<BHxI", reply.data)
        return result, raw

    def sdo_float(self, node, parameter):
        result, raw = self.sdo(node, parameter)
        return None if raw is None else struct.unpack("<f", struct.pack("<I", raw))[0]

    def set_axis_state(self, node, state):
        self.send(node, "set-axis-state", struct.pack("<I", AXIS_STATES.get(state, state)))

    def set_controller_mode(self, node, control, input_mode="passthrough"):
        self.send(node, "set-controller-mode",
                  struct.pack("<II", CONTROL_MODES.get(control, control),
                              INPUT_MODES.get(input_mode, input_mode)))

    def set_input(self, node, mode, value):
        if mode == "position":
            self.send(node, "set-input-pos", struct.pack("<fhh", value, 0, 0))
        elif mode == "velocity":
            self.send(node, "set-input-vel", struct.pack("<ff", value, 0.0))
        else:
            self.send(node, "set-input-torque", struct.pack("<f", value))

    def address(self, node, serial_number):
        self.send(UNADDRESSED, "address",
                  bytes([node]) + serial_number.to_bytes(6, "little") + b"\0")

    def scan(self, timeout_us=20000):
        """Discovery: every device answers an RTR Address to the broadcast node."""
        mark = len(self.bus.inbox)
        self.send(UNADDRESSED, "address", rtr=True)
        waited = 0
        while waited < timeout_us:
            self.step()
            waited += self.step_us
        found = {}
        for frame in self.frames_since(mark):
            if frame.command == COMMANDS["address"] and len(frame.data) == 8:
                found[int.from_bytes(frame.data[1:7], "little")] = frame.data[0]
        return found
