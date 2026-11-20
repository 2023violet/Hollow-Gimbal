"""Read a stopped locked-Board1 observation log without connecting to a target."""

import argparse
import json
from pathlib import Path

from board_capture import IMAGE_BYTES, observations, read_image, sha256
from replay import HALF_RANGE_MAX, JSON_LIMIT, UINT32_MAX, PackError, read_bounded, require


SPAN_NAMES = {
    "trigger_irq", "adc_irq", "spi_cs_irq", "spi_send_irq", "spi_close_irq", "spi_rx_irq",
    "uart_irq", "supervision", "serial", "adc_trigger", "adc_request_to_pcce_proxy",
    "pcce_to_ranks", "h0", "h1", "encoder_decode", "adapter",
}
LANE_NAMES = {"h0", "h1", "supervision"}


def words(tokens, size):
    require(len(tokens) == size and all(s.isascii() and s.isdecimal() and len(s) <= 10 for s in tokens),
            "LOG_FORMAT", "Expected bounded decimal uint32 fields.")
    values = [int(s) for s in tokens]
    require(all(v <= UINT32_MAX for v in values), "LOG_FORMAT", "Expected bounded uint32 fields.")
    return values


def parse_snapshot(text, clock_hz):
    relevant = [line.split() for line in text.splitlines() if line.startswith("FCT ")]
    require(relevant[:2] == [["FCT", "1"], ["FCT", "ENABLED", "1"]],
            "DWT_NOT_AVAILABLE", "A versioned snapshot from a CYCCNT-enabled locked image is required.")
    require(relevant[-1:] == [["FCT", "END"]], "LOG_FORMAT", "Snapshot lacks its end marker.")
    require(type(clock_hz) is int and 1 <= clock_hz <= UINT32_MAX, "CLOCK_INVALID",
            "Explicit clock rate must be a positive uint32.")
    spans, lanes, lock = {}, {}, None
    for line in relevant[2:-1]:
        kind, *data = line[1:]
        if kind == "SPAN" and data:
            name, *numeric = data
            require(name in SPAN_NAMES and name not in spans, "LOG_FORMAT", "Unknown or duplicate span.")
            start, end, identity, detail = words(numeric, 4)
            cycles = (end - start) & UINT32_MAX
            require(0 < cycles <= HALF_RANGE_MAX, "SPAN_ORDER", "Span duration is zero, reversed, or half-range ambiguous.")
            spans[name] = {"start_cycles": start, "end_cycles": end, "cycles": cycles,
                           "elapsed_ns": cycles * 1_000_000_000 // clock_hz, "id": identity, "detail": detail}
        elif kind == "LANE" and data:
            name, *numeric = data
            require(name in LANE_NAMES and name not in lanes, "LOG_FORMAT", "Unknown or duplicate Runtime lane.")
            lanes[name] = dict(zip(("due", "missed", "late", "budget", "latest_delay_us"), words(numeric, 5),
                                   strict=True))
        elif kind == "LOCK" and lock is None:
            action, confirmed, faults, session, result = words(data, 5)
            require(confirmed in (0, 1) and not (confirmed and action != 0),
                    "OUTPUT_LOCK_VIOLATION", "Locked snapshot confirms a non-INHIBIT action.")
            lock = {"action": action, "software_inhibit_confirmed": bool(confirmed),
                    "latched_faults": faults, "session": session, "adapter_result": result}
        else:
            raise PackError("LOG_FORMAT", "Unexpected line or multiple concatenated snapshots.")
    require(lanes.keys() == LANE_NAMES and lock is not None, "LOG_FORMAT", "Incomplete Runtime observations.")
    return {"clock_hz_for_conversion": clock_hz, "spans": spans, "lanes": lanes, "lock": lock,
            "unobserved_spans": sorted(SPAN_NAMES - spans.keys()), "target_wcet_verified": False,
            "physical_inhibit_verified": False, "evidence_grade": "OBSERVATIONS_ONLY"}


def stack_summary(data, low, painted_end, top):
    require(0x20000000 <= low < painted_end <= top <= 0x20008000
            and not (low % 4 or painted_end % 4 or top % 4) and len(data) == painted_end - low,
            "STACK_LAYOUT", "Expected the exact declared aligned painted SRAM interval.")
    change = next((offset for offset, value in enumerate(data) if value != 0xA5), None)
    depth = None if change is None else top - (low + change)
    return {"low": low, "end": painted_end, "top": top, "sha256": sha256(data),
            "minimum_observed_depth_bytes": depth, "painted_low_boundary_changed": change == 0,
            "scope": "PAINTED_REGION_ONLY"}


def capture_summary(path, source, clock_hz):
    raw = read_bounded(path, IMAGE_BYTES)
    header, adc, encoder = read_image(raw)
    require(header["producer"] == (1 if source == "BOARD_DECLARED" else 2),
            "SOURCE_MISMATCH", "SRAM producer and observation source disagree.")
    result = {"capture_id": header["id"], "sha256": sha256(raw), "health": observations(header, adc, encoder)}
    for lane, rows, flag, key in (("h0", adc, 64, "h0_cycles"), ("h1", encoder, 64, "h1_cycles")):
        cycles = [row[key] for row in rows if row["flags"] & flag]
        require(all(0 < value <= HALF_RANGE_MAX for value in cycles), "SPAN_ORDER", "Invalid captured DWT duration.")
        result[lane] = {"observations": len(cycles), "max_observed_cycles": max(cycles, default=None),
                        "max_observed_ns": max(cycles) * 1_000_000_000 // clock_hz if cycles else None}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--clock-hz", type=int, required=True, help="Clock established by the supplying measurement record.")
    parser.add_argument("--source", choices=("HOST_FIXTURE", "BOARD_DECLARED"), required=True)
    parser.add_argument("--variant", choices=("CAPTURE_OFF", "CAPTURE_ON"), required=True)
    parser.add_argument("--capture", type=Path, help="Optional contemporaneous frozen V1 image, Capture ON only.")
    parser.add_argument("--stack-dump", type=Path)
    parser.add_argument("--stack-range", nargs=3, metavar=("LOW", "PAINTED_END", "TOP"),
                        type=lambda s: int(s, 0))
    args = parser.parse_args()
    try:
        require((args.stack_dump is None) == (args.stack_range is None), "STACK_LAYOUT",
                "Supply both the dump and its painted range.")
        require(args.capture is None or args.variant == "CAPTURE_ON", "VARIANT_MISMATCH",
                "Capture OFF cannot supply a frozen SRAM image.")
        log = read_bounded(args.log, JSON_LIMIT)
        report = parse_snapshot(log.decode("utf-8-sig"), args.clock_hz)
        report.update(source=args.source, variant=args.variant, snapshot_sha256=sha256(log),
                      provenance="PROVENANCE_UNVERIFIED" if args.source == "BOARD_DECLARED" else "PROVENANCE_DECLARED")
        if args.capture:
            report["capture"] = capture_summary(args.capture, args.source, args.clock_hz)
        if args.stack_dump:
            report["stack"] = stack_summary(read_bounded(args.stack_dump, 32768), *args.stack_range)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (PackError, UnicodeError) as error:
        print(json.dumps({"status": "REJECTED", "reason": getattr(error, "reason", "LOG_FORMAT"),
                          "detail": str(error)}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())