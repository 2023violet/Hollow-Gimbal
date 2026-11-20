"""Archive the locked Board1 SRAM image and optionally prepare an ADC_ZERO Trace V1."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

from replay import (
    CSV_HEADER, IDENTITY_FIELDS, JSON_LIMIT, UINT32_MAX, UNITS, PackError, fields,
    integer, json_object, label, number, read_bounded, require, sha256_string,
    validate_csv, validate_identity, validate_manifest,
)


IMAGE_BYTES = 5480
WINDOW_US = 8000
ADC_CAPACITY = 42
ENCODER_CAPACITY = 18
ADC_ROWS = 40
ENCODER_ROWS = 16
ADC_PERIOD_US = 200
ENCODER_PERIOD_US = 500
ADC_FENCE_US = 80
HALF_RANGE = 1 << 31
PRODUCERS = {1: "BOARD_RECORDED", 2: "HOST_VIRTUAL"}
META = ("format_version", "source", "capture_id", "raw_sha256", "build_tag",
        "firmware_sha256", "identity", "origin_note")
WITNESS = ("format_version", "stage", "capture_id", "raw_sha256", "firmware_sha256",
           "covers_start_us", "covers_end_us", "witness_source", "independent_zero_current",
           "sampling_window_verified", "amp_per_count", "instrument", "evidence_reference")
HEADER = """magic version header_bytes total_bytes disposition id producer build_tag clock
start_us end_us adc_count encoder_count adc_overflow encoder_overflow adc_period_us encoder_period_us
ranks logical_channels encoder_cpr encoder_direction pole_pairs phase_order adc_record_bytes
encoder_record_bytes adc_capacity encoder_capacity board_profile encoder_profile clock_hz
freeze_us crc32""".split()
ADC = """id clock sequence completion_us slot generation requested_us ranks12 ranks34 flags
rejection missing_slots late_count sensor_sequence sensor_us h0_us session command_sequence
faults safety h0_result h0_cycles slot_start_us token_slot h0_return_us""".split()
ENCODER = """id clock sequence encoder_sequence cs_us rx_us close_us h1_return_us raw_word
flags transport result losses h1_status spi_status h1_cycles""".split()

ADC_SLOT, ADC_TOKEN, ADC_VALID, ADC_H0, ADC_SENSOR = 1, 2, 4, 8, 16
ADC_CLEAR, ADC_CONSUMED = 128, 256
ENCODER_CS, ENCODER_RAW = 1, 2
OUTPUT_LOCKED, OUTPUT_CONFIRMED = 256, 512


def sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def signed(word):
    return word if word < HALF_RANGE else word - (1 << 32)


def elapsed(later, earlier):
    return (later - earlier) & UINT32_MAX


def decode(raw, keys, offset):
    return dict(zip(keys, struct.unpack_from(f"<{len(keys)}I", raw, offset), strict=True))


def read_lane(raw, header, keys, start, count, capacity, completion):
    rows = []
    prev_time = None
    size = 4 * len(keys)
    for i in range(count):
        row = decode(raw, keys, start + i * size)
        require(row["id"] == header["id"], "CAPTURE_ID_MISMATCH", f"{completion} row {i} has a foreign capture.")
        require(row["clock"] == header["clock"], "CLOCK_MISMATCH", f"{completion} row {i} has a foreign clock.")
        require(row["sequence"] == i, "ROW_SEQUENCE", f"{completion} row {i} has a discontinuous stored index.")
        time = elapsed(row[completion], header["start_us"])
        require(time < WINDOW_US and (prev_time is None or time >= prev_time), "TIME_ORDER",
                f"{completion} row {i} is reversed or outside the acquisition window.")
        prev_time = time
        rows.append(row)
    require(not any(raw[start + count * size:start + capacity * size]), "NONZERO_TAIL",
            f"Undeclared bytes at the end of the {completion} lane.")
    return rows


def read_image(raw):
    require(len(raw) == IMAGE_BYTES, "RAW_SIZE", f"Recorder V1 requires precisely {IMAGE_BYTES} bytes.")
    header = decode(raw, HEADER, 0)
    require((header["magic"], header["version"]) == (0x31434246, 1), "RAW_VERSION",
            "Unknown Board1 SRAM image format.")
    require(header["crc32"] == zlib.crc32(raw[:124] + bytes(4) + raw[128:]), "CRC_MISMATCH",
            "Recorder checksum disagrees with the frozen image.")
    layout = ("header_bytes", "total_bytes", "adc_record_bytes", "encoder_record_bytes",
              "adc_capacity", "encoder_capacity")
    require(tuple(header[key] for key in layout) == (128, IMAGE_BYTES, 100, 64, ADC_CAPACITY, ENCODER_CAPACITY)
            and header["adc_count"] <= ADC_CAPACITY and header["encoder_count"] <= ENCODER_CAPACITY,
            "RAW_LAYOUT", "Capture counts or recorder layout are out of bounds.")
    require(header["id"] != 0 and header["producer"] in PRODUCERS and header["disposition"] in (1, 2),
            "RAW_IDENTITY", "Missing capture ID, invalid producer, or unsealed recorder.")
    require((header["clock"], header["clock_hz"]) == (1, 1_000_000), "CLOCK_MISMATCH",
            "Expected the common TMR2 microsecond clock.")
    require((header["adc_period_us"], header["encoder_period_us"]) == (ADC_PERIOD_US, ENCODER_PERIOD_US)
            and elapsed(header["end_us"], header["start_us"]) == WINDOW_US, "TIME_POLICY", "Unexpected window or cadence.")
    require(header["disposition"] == 2 or WINDOW_US <= elapsed(header["freeze_us"], header["start_us"]) < HALF_RANGE,
            "TIME_ORDER", "Normal seal must follow the planned window in the source clock.")
    require((header["ranks"], header["logical_channels"]) == (0x06050401, 0x00060504),
            "RANK_MAP", "Expected VBUS/IC/IB/IA and Board1 A/B/C channel order 4/5/6.")
    header["encoder_direction"] = signed(header["encoder_direction"])
    require((header["board_profile"], header["encoder_profile"], header["encoder_cpr"],
             header["encoder_direction"], header["pole_pairs"], header["phase_order"]) == (1, 1, 65536, -1, 7, 0),
            "RAW_IDENTITY", "Unexpected immutable locked Board1 profile.")
    adc = read_lane(raw, header, ADC, 128, header["adc_count"], ADC_CAPACITY, "completion_us")
    encoder = read_lane(raw, header, ENCODER, 128 + ADC_CAPACITY * 100,
                        header["encoder_count"], ENCODER_CAPACITY, "close_us")
    for row in adc:
        row["ranks"] = [row["ranks12"] & 65535, row["ranks12"] >> 16,
                        row["ranks34"] & 65535, row["ranks34"] >> 16]
        row["h0_result"] = signed(row["h0_result"])
        flags, safety = row["flags"], row["safety"]
        require(flags < 512 and all(rank <= 4095 for rank in row["ranks"]), "ADC_RECORD", "Bad ADC flags or ADC12 rank.")
        require(safety & OUTPUT_LOCKED and (not safety & OUTPUT_CONFIRMED or safety & 15 == 0),
                "OUTPUT_LOCK_VIOLATION", "A row lacks the Board lock or confirms an action other than INHIBIT.")
        require(not flags & ADC_VALID or flags & (ADC_SLOT | ADC_TOKEN | ADC_CLEAR) == ADC_SLOT | ADC_TOKEN | ADC_CLEAR,
                "ADC_RECORD", "A valid ADC row must own a coherent cleared slot and token.")
        require(not flags & ADC_CONSUMED or flags & ADC_H0, "ADC_RECORD", "Consumption requires an H0 call.")
        require(not flags & ADC_H0 or (row["h0_us"] == row["completion_us"] and
                elapsed(row["h0_return_us"], row["h0_us"]) < HALF_RANGE), "TIME_ORDER",
                "H0 must use the completion time argument and return after it.")
        require(not flags & ADC_SENSOR or elapsed(row["h0_us"], row["sensor_us"]) < HALF_RANGE,
                "TIME_ORDER", "The observed sensor timestamp is later than its H0 consumer.")
        if flags & ADC_VALID:
            require(row["slot"] == row["token_slot"] and
                    elapsed(row["completion_us"], row["slot_start_us"]) < ADC_PERIOD_US,
                    "SLOT_TOKEN_MISMATCH", "A valid completion must belong to its originating carrier slot.")
        if flags & ADC_TOKEN:
            age = elapsed(row["completion_us"], row["requested_us"])
            require(age < HALF_RANGE, "TIME_ORDER", "An ADC completion predates its trigger request.")
            require(not flags & ADC_VALID or age <= ADC_FENCE_US, "ADC_DEADLINE", "Valid ADC exceeds the request fence.")
    for row in encoder:
        row["h1_status"] = signed(row["h1_status"])
        flags = row["flags"]
        require(flags < 128 and row["raw_word"] <= 65535 and row["transport"] <= 4 and row["result"] <= 4,
                "ENCODER_RECORD", "Bad SPI record flags, status, or word width.")
        require((flags & ENCODER_RAW and flags & ENCODER_CS) or
                (not flags & ENCODER_RAW and row["raw_word"] == 0 and row["rx_us"] == 0),
                "ENCODER_RECORD", "SPI bytes are usable only when the current transaction read RX.")
        require(elapsed(row["h1_return_us"], row["close_us"]) < HALF_RANGE, "TIME_ORDER", "H1 return predates close.")
        if flags & ENCODER_CS:
            require(elapsed(row["close_us"], row["cs_us"]) < HALF_RANGE, "TIME_ORDER", "Close predates CS.")
        if flags & ENCODER_RAW:
            require(elapsed(row["rx_us"], row["cs_us"]) <= elapsed(row["close_us"], row["cs_us"]),
                    "TIME_ORDER", "SPI RX lies outside the transaction.")
    return header, adc, encoder


def inspect_metadata(raw, metadata, header):
    fields(metadata, META, "metadata", "METADATA_INVALID")
    require(integer(metadata["format_version"], 1, 1) and label(metadata["origin_note"])
            and integer(metadata["build_tag"]) and integer(metadata["capture_id"], 1)
            and (metadata["firmware_sha256"] is None or sha256_string(metadata["firmware_sha256"])),
            "METADATA_INVALID", "Invalid version, origin note, capture ID, build tag or declared firmware digest.")
    identity = fields(metadata["identity"], IDENTITY_FIELDS, "identity", "IDENTITY_INVALID")
    require(all(x is None or integer(x, 1) for x in identity.values()), "IDENTITY_INVALID",
            "Inventory IDs must be nonzero uint32 or null (unknown).")
    require(sha256_string(metadata["raw_sha256"]) and metadata["raw_sha256"] == sha256(raw), "HASH_MISMATCH",
            "The metadata does not bind these raw bytes.")
    require(metadata["capture_id"] == header["id"] and metadata["build_tag"] == header["build_tag"],
            "CAPTURE_ID_MISMATCH", "The raw capture ID or build tag disagrees with the declaration.")
    require(metadata["source"] == PRODUCERS[header["producer"]], "SOURCE_MISMATCH",
            "Declared source disagrees with the producer field.")
    return "PROVENANCE_UNVERIFIED" if metadata["source"] == "BOARD_RECORDED" else "PROVENANCE_DECLARED"


def sequence_health(rows, key):
    gaps = duplicates = 0
    for left, right in zip(rows, rows[1:]):
        step = elapsed(right[key], left[key])
        require(step < HALF_RANGE, "SEQUENCE_ORDER", f"{key} reversed or jumped beyond the half-range.")
        gaps += max(0, step - 1)
        duplicates += step == 0
    return {"missing": gaps, "repeated": duplicates}


def observations(header, adc, encoder):
    slots = sequence_health([row for row in adc if row["flags"] & ADC_SLOT], "slot")
    tokens = sequence_health([row for row in adc if row["flags"] & ADC_VALID], "generation")
    frames = sequence_health(encoder, "encoder_sequence")
    for key in ("missing_slots", "late_count"):
        require(all(left[key] <= right[key] for left, right in zip(adc, adc[1:])),
                "COUNTER_ORDER", f"{key} decreased within a capture.")
    delta = {key: adc[-1][key] - adc[0][key] if len(adc) > 1 else 0
             for key in ("missing_slots", "late_count")}
    adc_invalid = sum(not row["flags"] & ADC_VALID for row in adc)
    encoder_errors = sum(row["result"] != 0 or not row["flags"] & ENCODER_RAW for row in encoder)
    short = {"adc": max(0, ADC_ROWS - len(adc)), "encoder": max(0, ENCODER_ROWS - len(encoder))}
    complete = (header["disposition"] == 1 and len(adc) == ADC_ROWS and len(encoder) == ENCODER_ROWS
                and not any((header["adc_overflow"], header["encoder_overflow"], adc_invalid,
                             encoder_errors, *slots.values(), *tokens.values(), *frames.values(), *delta.values())))
    return {"complete": complete, "nominal_shortfall": short, "slot_sequence": slots,
            "valid_token_sequence": tokens, "encoder_sequence": frames, "adc_counter_delta": delta,
            "adc_invalid": adc_invalid, "encoder_errors": encoder_errors,
            "max_adc_service_to_h0_return_us": max((elapsed(row["h0_return_us"], row["completion_us"])
                                                   for row in adc if row["flags"] & ADC_H0), default=None),
            "max_encoder_close_to_h1_return_us": max((elapsed(row["h1_return_us"], row["close_us"])
                                                      for row in encoder), default=None),
            "adc_range": [{"min": min(row["ranks"][i] for row in adc), "max": max(row["ranks"][i] for row in adc)}
                          for i in range(4)] if adc else [],
            "output_locked_rows": len(adc),
            "software_inhibit_confirmed_rows": sum(bool(row["safety"] & OUTPUT_CONFIRMED) for row in adc)}


def prepare_trace(raw_witness, header, adc, metadata, health):
    reason = "PRECONDITION_REQUIRED"
    require(health["complete"], "ACQUISITION_INCOMPLETE", "The frozen acquisition has gaps, errors, or overflow.")
    witness = fields(json_object(raw_witness, reason), WITNESS, "qualification", reason)
    require(integer(witness["format_version"], 1, 1) and witness["stage"] == "ADC_ZERO"
            and integer(witness["capture_id"], 1) and witness["capture_id"] == header["id"]
            and witness["raw_sha256"] == metadata["raw_sha256"] and
            witness["firmware_sha256"] == metadata["firmware_sha256"]
            and integer(witness["covers_start_us"]) and witness["covers_start_us"] == header["start_us"]
            and integer(witness["covers_end_us"]) and witness["covers_end_us"] == header["end_us"],
            reason, "Witness must bind this exact acquisition and its full planned source window.")
    host = metadata["source"] == "HOST_VIRTUAL"
    require(witness["witness_source"] == ("HOST_VIRTUAL" if host else "EXTERNAL_INSTRUMENT")
            and (host or metadata["firmware_sha256"] is not None)
            and witness["independent_zero_current"] is True and witness["sampling_window_verified"] is True
            and number(witness["amp_per_count"]) and witness["amp_per_count"] > 0
            and label(witness["evidence_reference"]), reason,
            "Supply the independent zero-current and timing witness, instrument gain, and evidence reference.")
    identity = validate_identity(metadata["identity"], "IDENTITY_INVALID")
    require(all(elapsed(row["requested_us"], header["start_us"]) < WINDOW_US and
                all(0 < rank < 4095 for rank in row["ranks"][1:]) for row in adc),
            reason, "The source proxy must be inside the witnessed window and current ranks must be unsaturated.")

    # V1 flags: valid + same-frame + qualified-window + source-is-proxy.
    rows = [(0, header["id"], row["sequence"], row["requested_us"], row["completion_us"],
             row["requested_us"], "", 1, "", 1, 0x654, 153, 1, *row["ranks"][1:], "", "", "")
            for row in adc]
    csv = CSV_HEADER + b"\n" + "".join(",".join(map(str, row)) + "\n" for row in rows).encode("ascii")
    descriptions = dict.fromkeys(("output_confirmation", "independent_reference", "load_condition",
                                  "independent_kt"), "Not observed for this selected ADC-only stage.")
    descriptions["adc_zero_current"] = witness["evidence_reference"]
    manifest = {
        "format_version": 1, "source": metadata["source"], "host_synthetic": False,
        "session": header["id"], "recorder": {"name": "Board1 SRAM capture", "version": "1"},
        "identity": identity, "profile": "SELECTED_STAGES", "stage_mask": 1,
        "stage_rows": [len(adc), 0, 0, 0, 0, 0, 0, 0],
        "time": {"unit": "us", "clock_id": 1, "max_age_us": ADC_FENCE_US, "max_skew_us": 0,
                 "allow_wrap": header["end_us"] < header["start_us"], "alignment": "SAME_CLOCK_PAIR"},
        "adc": {"max_count": 4095, "amp_per_count": witness["amp_per_count"], "channel_map": [4, 5, 6]},
        "encoder": {"cpr": header["encoder_cpr"], "raw_direction": header["encoder_direction"],
                    "phase_order": header["phase_order"], "pole_pairs": header["pole_pairs"]},
        "reference": {"id": 0, "zero_rad": 0, "frame_direction": 1}, "torque_model_id": 0,
        "units": UNITS, "instruments": [witness["instrument"]], "preconditions": descriptions,
        "files": {"samples.csv": {"sha256": sha256(csv), "rows": len(adc)}},
    }
    if host:
        manifest["generator"] = {"seed": 0, "sample_rates_hz": {"adc": 5000, "encoder": 2000},
                                 "parameters": {"fixture": "foc_board_capture production SRAM writer"}}
    validate_manifest(manifest)
    validate_csv(csv, len(adc))
    return manifest, csv


def json_bytes(data):
    return (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def import_image(args):
    raw = read_bounded(args.image, 8192)
    args.out.mkdir()
    files = {"capture.bin": sha256(raw)}
    (args.out / "capture.bin").write_bytes(raw)
    report = {"archive_version": 1, "status": "REJECTED", "files": files}
    error = None
    try:
        meta = read_bounded(args.metadata, JSON_LIMIT)
        (args.out / "source.json").write_bytes(meta)
        files["source.json"] = sha256(meta)
        metadata = json_object(meta, "METADATA_INVALID")
        header, adc, encoder = read_image(raw)
        provenance = inspect_metadata(raw, metadata, header)
        health = observations(header, adc, encoder)
        report.update(status="DIAGNOSTIC_OBSERVATION", source=metadata["source"], provenance=provenance,
                      header=header, health=health, adc=adc, encoder=encoder,
                      stage_status={"ADC_ZERO": ("INDEPENDENT_ZERO_CURRENT_AND_TIMING_WITNESS_REQUIRED"
                                                 if health["complete"] else "ACQUISITION_INCOMPLETE"),
                                    "ENCODER_LUT_ROTOR_OFFSET": "INDEPENDENT_ANGLE_REFERENCE_REQUIRED",
                                    "R_L": "OBSERVED_TERMINAL_VOLTAGE_CURRENT_AND_EXCITATION_REQUIRED",
                                    "J_B": "QUALIFIED_TORQUE_AND_LOAD_REQUIRED",
                                    "K_D": "QUALIFIED_J_B_AND_INDEPENDENT_KT_REQUIRED"})
        if args.qualification is not None:
            witness = read_bounded(args.qualification, JSON_LIMIT)
            (args.out / "qualification.json").write_bytes(witness)
            files["qualification.json"] = sha256(witness)
            manifest, csv = prepare_trace(witness, header, adc, metadata, health)
            (args.out / "trace").mkdir()
            for filename, payload in (("manifest.json", json_bytes(manifest)), ("samples.csv", csv)):
                (args.out / "trace" / filename).write_bytes(payload)
                files[f"trace/{filename}"] = sha256(payload)
            report["stage_status"]["ADC_ZERO"] = "READY_FOR_SOFT06_VALIDATION"
            report["status"] = ("TRACE_PREPARED_PROVENANCE_UNVERIFIED" if provenance == "PROVENANCE_UNVERIFIED"
                                else "QUALIFIED_MEASUREMENT_TRACE")
    except PackError as caught:
        report["rejection"] = {"reason": caught.reason, "detail": str(caught)}
        error = caught
    (args.out / "diagnostic.json").write_bytes(json_bytes(report))
    if error is not None:
        raise error
    return {"status": report["status"], "provenance": report["provenance"], "health": report["health"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    importer = commands.add_parser("import", help="Archive the SRAM dump and its measurement declarations.")
    importer.add_argument("image", type=Path)
    importer.add_argument("--metadata", type=Path, required=True)
    importer.add_argument("--out", type=Path, required=True, help="New archive directory.")
    importer.add_argument("--qualification", type=Path)
    inspect = commands.add_parser("describe", help="Decode a frozen raw image without declaring its provenance.")
    inspect.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "import":
            result = import_image(args)
        else:
            raw = read_bounded(args.image, 8192)
            header, adc, encoder = read_image(raw)
            result = {"raw_sha256": sha256(raw), "header": header, "health": observations(header, adc, encoder)}
        print(json.dumps(result, ensure_ascii=True))
        return 0
    except PackError as error:
        print(json.dumps({"status": "REJECTED", "reason": error.reason, "detail": str(error)}))
    except OSError as error:
        print(json.dumps({"status": "REJECTED", "reason": "ARCHIVE_IO_ERROR", "detail": str(error)}))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())