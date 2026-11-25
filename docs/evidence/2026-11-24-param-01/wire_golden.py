"""Record every PARAM_READ / PARAM_WRITE response of the Board1 SIL, byte for byte.

Usage: python -B golden.py <falconfoc-board1-sil.exe> <out.json>
Lockstep makes the run deterministic; two runs of one binary must match exactly.
"""
import contextlib
import io
import json
import math
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools/falconfoc-cli"))
import falconfoc_cli as cli  # noqa: E402

SIL = sys.argv[1]
records = []


def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def raw(client, name, payload, tag):
    frame = client.call(name, payload)
    records.append([tag, name, payload.hex(), frame.payload.hex()])
    return frame


def read_all(client, tag):
    for identifier in range(201):
        raw(client, "param-read", struct.pack("<H", identifier), f"{tag}:read")


def candidates(info, reply):
    kind = info["type"]
    lo, hi = reply["minimum"], reply["maximum"]
    if kind == "u32":
        values = [lo, hi, (lo + hi) // 2, lo - 1 if lo > 0 else None, hi + 1 if hi < 0xFFFFFFFF else None,
                  300000]
        return [(1, v) for v in values if v is not None] + [(2, bits(float(lo)))]
    span = hi - lo
    values = [lo, hi, lo + span / 3.0, lo - max(abs(lo) * 0.01, 1e-3), hi + max(abs(hi) * 0.01, 1e-3)]
    out = [(2, bits(v)) for v in values]
    out += [(2, bits(math.nan)), (2, bits(math.inf)), (1, int(abs(lo)))]
    return out


def write_sweep(client, tag):
    for name, info in cli.PARAMETER_INFO.items():
        if info["access"] == "ro":
            continue
        reply = cli.describe(client.call("param-read", cli.parameter_read(info["id"])))
        if reply["result"] != "OK":
            continue
        for type_code, value in candidates(info, reply):
            payload = struct.pack("<HBI", info["id"], type_code, value & 0xFFFFFFFF)
            raw(client, "param-write", payload, f"{tag}:write")
            for _ in range(3):  # let H0 apply or refuse a tuning candidate
                raw(client, "status", b"", f"{tag}:status")
            raw(client, "param-read", struct.pack("<H", info["id"]), f"{tag}:readback")


def run(arguments, tag, calibrate):
    flash = Path(tempfile.mkdtemp()) / "flash.bin"
    with cli.SimulatorTransport(SIL, ["--lockstep-us", "500"] + arguments + ["--flash", str(flash)]) as transport, \
            contextlib.redirect_stdout(io.StringIO()):
        client = cli.Client(transport, sequence=1)
        client.hello()
        read_all(client, f"{tag}:fresh")
        write_sweep(client, f"{tag}:sweep")
        read_all(client, f"{tag}:after-sweep")
        if calibrate:
            session = cli.SilSession(client, transport)
            session.tune("command-lease-us", 50000)
            session.call("calibrate")
            cli._calibrate_wait(session)
            read_all(client, f"{tag}:calibrated")
            session.lifecycle("set-mode", cli.SIL_STATE_DISABLED, mode="velocity")
            session.lifecycle("arm", cli.SIL_STATE_ARMED, cli.setpoint("velocity", 1.0))
            session.lifecycle("start", cli.SIL_STATE_RUNNING, cli.setpoint("velocity", 1.0))
            for identifier in (1, 2, 64, 74, 120, 124):
                info = next(i for i in cli.PARAMETER_INFO.values() if i["id"] == identifier)
                value = 9 if info["type"] == "u32" and identifier == 1 else (10 if info["type"] == "u32" else bits(0.5))
                raw(client, "param-write", struct.pack("<HBI", identifier, 1 if info["type"] == "u32" else 2, value),
                    f"{tag}:running-write")
            read_all(client, f"{tag}:running")
            session.stop()
            for _ in range(400):
                status = session.status()
                if status["state"] == cli.SIL_STATE_DISABLED and status["software_output_acknowledged"]:
                    break
            cli._wait_still(session)
            raw(client, "param-write", struct.pack("<HBI", 1, 1, 12), f"{tag}:save-dirty")
            raw(client, "save", b"", f"{tag}:save")
            read_all(client, f"{tag}:saved")


run(cli.SIL_CALIBRATE_ARGUMENTS, "calibrate-plant", True)
run([], "default-plant", False)
Path(sys.argv[2]).write_text(json.dumps(records, indent=0), encoding="utf-8")
print(len(records), "records")
