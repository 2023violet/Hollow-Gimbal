"""Validate and replay a read-only FalconFOC measurement pack."""

import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import re
import subprocess


FORMAT_VERSION = 1
JSON_LIMIT = 64 * 1024
SAMPLE_LIMIT = 16 * 1024 * 1024
ROW_LIMIT = 100000
LINE_LIMIT = 512
UINT32_MAX = (1 << 32) - 1
HALF_RANGE_MAX = (1 << 31) - 1
HOST_SKEW_LIMIT_US = 50
FLOAT32_MAX = 3.4028234663852886e38
SOURCE_IDS = {"HOST_SYNTHETIC": 1, "HOST_VIRTUAL": 2, "EXTERNAL_RECORDED": 3, "BOARD_RECORDED": 4}
HOST_SOURCES = {"HOST_SYNTHETIC", "HOST_VIRTUAL"}
UNITS = {"adc": "count", "current": "A", "voltage": "V", "angle": "rad",
         "encoder": "count", "speed": "rad/s", "torque": "N*m", "kt": "N*m/A"}
IDENTITY_FIELDS = ("device_id", "motor_id", "encoder_id", "frame_id")
PRECONDITION_FIELDS = ("adc_zero_current", "output_confirmation", "independent_reference",
                       "load_condition", "independent_kt")
CSV_NAME = "samples.csv"
CSV_HEADER = (
    b"stage,capture,sequence,source_us,completion_us,trigger_us,reference_us,"
    b"clock_id,reference_clock_id,unit,channel,flags,evidence,p1,p2,p3,p4,p5,p6"
)
UINT = rb"[0-9]{1,10}"
OPTIONAL_UINT = rb"(?:" + UINT + rb")?"
OPTIONAL_DECIMAL = rb"(?:[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)?"
CSV_ROW = re.compile(b",".join(
    [UINT] * 4 + [OPTIONAL_UINT] * 3 + [UINT, OPTIONAL_UINT] + [UINT] * 4 + [OPTIONAL_DECIMAL] * 6
))
REASON_DETAILS = {
    "AGE": "The recorded completion exceeds the permitted source-to-completion age.",
    "ALIGNMENT": "The measurement and its reference fail the required same-clock pairing or skew bound.",
    "CLOCK": "The sample or reference clock does not match the declared source clock.",
    "DATA_REQUIRED": "The stage lacks a required measurement or accepted dependency.",
    "GAP": "The sample sequence or capture interval has a gap.",
    "HOLDOUT_NOT_INDEPENDENT": "The LUT holdout needs a separately identified capture with its own observations.",
    "HOLDOUT_REQUIRED": "The selected LUT needs a completed independent holdout before rotor or offset replay.",
    "HOLDOUT_REUSED_PAYLOAD": "The LUT holdout reuses a training observation after the capture identity or time changed.",
    "IDENTITY": "The recorded measurement does not match the capture's configuration or channel identity.",
    "INCOMPLETE": "The supplied capture ends before its declared number of records.",
    "KT_REQUIRED_FOR_DQ": "Observed d/q current requires an independently qualified Kt before torque can be estimated.",
    "LOAD_REQUIRED": "The load was neither measured nor independently established as negligible.",
    "PRECONDITION_REQUIRED": "The sample lacks the required observed acquisition precondition.",
    "REFERENCE_REQUIRED": "The stage needs an independent measured reference with its associated capture time.",
    "SATURATION": "The sample flags report saturation or limiting during acquisition.",
    "SEQUENCE": "The sample sequence fails the capture's ordering contract.",
    "TIME_ORDER": "The recorded times violate the source clock's ordering or wrap policy.",
    "TORQUE_SOURCE_REQUIRED": "The stage lacks observed torque or a qualified source for the torque model.",
    "UNITS": "The row's unit code does not describe the selected stage's measurements.",
    "WORKFLOW_QUALITY_OR_DEPENDENCY": "The native workflow found a missing dependency or rejected the stage quality.",
    "CANDIDATE_ADMISSION_REJECTED": "The selected stages completed and the existing calibration admission rejected the combined result.",
}


class PackError(ValueError):
    def __init__(self, reason, detail):
        super().__init__(detail)
        self.reason = reason


def require(test, reason, detail):
    if not test:
        raise PackError(reason, detail)


def read_bounded(path, limit):
    try:
        require(path.is_file(), "INPUT_IO_ERROR", f"Expected a regular file at {path.name}.")
        with path.open("rb") as stream:
            raw = stream.read(limit + 1)
    except OSError as error:
        raise PackError("INPUT_IO_ERROR", f"Cannot read {path.name}. {error.strerror}.") from error
    require(len(raw) <= limit, "FILE_TOO_LARGE", f"{path.name} exceeds {limit} bytes.")
    return raw


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate key {key!r}")
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError(f"Invalid JSON number {value}")


def finite_float(value):
    parsed = float(value)
    if not math.isfinite(parsed):
        reject_constant(value)
    return parsed


def json_object(raw, reason):
    try:
        result = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object,
                            parse_float=finite_float, parse_constant=reject_constant)
    except (ValueError, RecursionError) as error:
        raise PackError(reason, "Expected UTF-8 JSON with unique keys and finite numbers.") from error
    require(type(result) is dict, reason, "Expected a JSON object.")
    return result


def fields(value, names, field, reason="MANIFEST_INVALID"):
    require(type(value) is dict and value.keys() == set(names), reason,
            f"{field} must contain exactly {', '.join(names)}.")
    return value


def integer(value, low=0, high=UINT32_MAX):
    return type(value) is int and low <= value <= high


def number(value):
    return type(value) in (int, float) and -FLOAT32_MAX <= value <= FLOAT32_MAX


def label(value):
    return (type(value) is str and 0 < len(value.strip()) and len(value) <= 256
            and all(character.isprintable() for character in value))


def direction(value):
    return type(value) is int and value in (-1, 1)


def sha256_string(value):
    return type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def validate_identity(value, reason):
    identity = fields(value, IDENTITY_FIELDS, "identity", reason)
    require(all(integer(item, 1) for item in identity.values()), reason, "Identity IDs must be nonzero uint32.")
    return identity


def validate_manifest(manifest):
    require(integer(manifest.get("format_version"), FORMAT_VERSION, FORMAT_VERSION), "UNSUPPORTED_VERSION",
            f"The only supported measurement format version is {FORMAT_VERSION}.")
    source = manifest.get("source")
    require(type(source) is str and source in SOURCE_IDS, "SOURCE_INVALID", "Select a supported source class.")
    host = source in HOST_SOURCES
    keys = ("format_version", "source", "host_synthetic", "session", "recorder", "identity", "profile",
            "stage_mask", "stage_rows", "time", "adc", "encoder", "reference", "torque_model_id",
            "units", "instruments", "preconditions", "files")
    fields(manifest, keys + (("generator",) if host else ()), "manifest")
    require(manifest["host_synthetic"] is (source == "HOST_SYNTHETIC"), "SOURCE_INVALID",
            "host_synthetic must agree with source.")
    require(integer(manifest["session"], 1), "MANIFEST_INVALID", "Session must be a nonzero uint32.")
    recorder = fields(manifest["recorder"], ("name", "version"), "recorder")
    require(all(label(item) for item in recorder.values()), "MANIFEST_INVALID", "Recorder name and version are required.")
    identity = validate_identity(manifest["identity"], "IDENTITY_INVALID")
    profile = manifest["profile"]
    stage_mask = manifest["stage_mask"]
    require(profile in ("FULL_PROFILE", "SELECTED_STAGES") and integer(stage_mask, 1, 127)
            and (profile != "FULL_PROFILE" or stage_mask == 127), "MANIFEST_INVALID", "Profile and stage mask disagree.")
    require(manifest["units"] == UNITS, "UNIT_MISMATCH", "Measurement units must use the V1 SI/count vocabulary.")

    time = fields(manifest["time"], ("unit", "clock_id", "max_age_us", "max_skew_us", "allow_wrap", "alignment"), "time")
    require(time["unit"] == "us", "UNIT_MISMATCH", "Time must use integer microseconds.")
    require(integer(time["clock_id"], 1) and type(time["allow_wrap"]) is bool
            and integer(time["max_age_us"], 0, HALF_RANGE_MAX) and integer(time["max_skew_us"], 0, HOST_SKEW_LIMIT_US)
            and time["alignment"] == "SAME_CLOCK_PAIR", "CLOCK_POLICY_INVALID",
            "V1 requires same-clock pairs, a nonzero clock ID, an explicit wrap policy, sub-half-range age "
            "and pairing skew within the 50 us Host policy ceiling.")

    adc = fields(manifest["adc"], ("max_count", "amp_per_count", "channel_map"), "adc")
    channel_map = adc["channel_map"]
    require(integer(adc["max_count"], 1, 65535) and number(adc["amp_per_count"]) and adc["amp_per_count"] > 0
            and type(channel_map) is list and len(channel_map) == 3
            and all(integer(channel, 0, 15) for channel in channel_map) and len(set(channel_map)) == 3,
            "IDENTITY_INVALID", "ADC requires a valid range, positive gain and three distinct channel IDs in A/B/C order.")
    map_bits = sum(channel << (4 * phase) for phase, channel in enumerate(channel_map))
    encoder = fields(manifest["encoder"], ("cpr", "raw_direction", "phase_order", "pole_pairs"), "encoder")
    require(integer(encoder["cpr"], 2, 65536) and direction(encoder["raw_direction"])
            and integer(encoder["phase_order"], 0, 1) and integer(encoder["pole_pairs"], 1, 255),
            "IDENTITY_INVALID", "Encoder CPR, direction, phase order and expected pole pairs must be valid.")
    reference = fields(manifest["reference"], ("id", "zero_rad", "frame_direction"), "reference")
    require(integer(reference["id"]) and number(reference["zero_rad"]) and direction(reference["frame_direction"])
            and integer(manifest["torque_model_id"]), "IDENTITY_INVALID", "Invalid angle reference or torque-model metadata.")

    instruments = manifest["instruments"]
    require(type(instruments) is list and bool(instruments), "MANIFEST_INVALID", "At least one instrument is required.")
    for instrument in instruments:
        fields(instrument, ("id", "model", "calibration"), "instrument")
        require(all(label(item) for item in instrument.values()), "MANIFEST_INVALID", "Instrument metadata must be text.")
    descriptions = fields(manifest["preconditions"], PRECONDITION_FIELDS, "preconditions")
    require(all(label(item) for item in descriptions.values()), "MANIFEST_INVALID", "Provide each precondition declaration.")
    if host:
        generator = fields(manifest["generator"], ("seed", "sample_rates_hz", "parameters"), "generator")
        rates, parameters = generator["sample_rates_hz"], generator["parameters"]
        require(integer(generator["seed"]) and type(rates) is dict and bool(rates)
                and all(label(key) and number(value) and value > 0 for key, value in rates.items())
                and type(parameters) is dict and bool(parameters)
                and all(label(key) and (number(value) or label(value) or type(value) is bool)
                        for key, value in parameters.items()), "MANIFEST_INVALID",
                "Host generator metadata requires a seed, sample rates and scalar model parameters.")

    files = fields(manifest["files"], (CSV_NAME,), "files")
    samples = fields(files[CSV_NAME], ("sha256", "rows"), CSV_NAME)
    stage_rows = manifest["stage_rows"]
    require(sha256_string(samples["sha256"]) and integer(samples["rows"], 0, ROW_LIMIT), "MANIFEST_INVALID",
            "Provide a lowercase SHA-256 and a bounded row count for samples.csv.")
    require(type(stage_rows) is list and len(stage_rows) == 8
            and all(integer(count, 0, ROW_LIMIT) for count in stage_rows) and sum(stage_rows) == samples["rows"],
            "ROW_COUNT_MISMATCH", "The eight stream counts must sum to the declared CSV row count.")
    require(stage_rows[6] <= 1, "ROW_COUNT_MISMATCH", "The Kt stream can contain at most one record.")

    return [FORMAT_VERSION, SOURCE_IDS[source], manifest["session"], stage_mask,
            *[identity[key] for key in IDENTITY_FIELDS], adc["max_count"], adc["amp_per_count"], map_bits,
            encoder["cpr"], encoder["raw_direction"], encoder["phase_order"], encoder["pole_pairs"],
            time["clock_id"], time["max_age_us"], time["max_skew_us"], int(time["allow_wrap"]),
            reference["id"], reference["zero_rad"], reference["frame_direction"], manifest["torque_model_id"], *stage_rows]


def strip_line_ending(line):
    return line.removesuffix(b"\n").removesuffix(b"\r") if line.endswith(b"\n") else line


def validate_csv(raw, expected_rows):
    stream = io.BytesIO(raw)
    require(strip_line_ending(stream.readline()) == CSV_HEADER, "CSV_INVALID", "Missing or incorrect V1 CSV header.")
    count = 0
    for count, line in enumerate(stream, 1):
        require(count <= expected_rows, "ROW_COUNT_MISMATCH", "CSV contains extra rows.")
        row = strip_line_ending(line)
        require(len(row) <= LINE_LIMIT and CSV_ROW.fullmatch(row) is not None, "CSV_INVALID",
                f"Sample row {count} must have 19 bounded numeric fields with only permitted empty fields.")
    require(count == expected_rows, "ROW_COUNT_MISMATCH", f"CSV has {count} rows, expected {expected_rows}.")


def attestation_status(manifest, digest, pack, path):
    source = manifest["source"]
    if source in HOST_SOURCES:
        require(path is None, "ATTESTATION_INVALID", "An external-source attestation applies only to an external source.")
        return "PROVENANCE_DECLARED", "The manifest identifies this capture as Host data."
    if path is None:
        return "PROVENANCE_UNVERIFIED", "An independent acquisition attestation is required to establish external origin."

    try:
        attestation_path = path.resolve()
        separate = pack.resolve() not in attestation_path.parents
    except (OSError, RuntimeError) as error:
        raise PackError("ATTESTATION_INVALID", "Cannot resolve the supplied attestation path.") from error
    require(separate, "ATTESTATION_INVALID", "The caller's trusted attestation must be outside the pack directory.")
    attestation = json_object(read_bounded(attestation_path, JSON_LIMIT), "ATTESTATION_INVALID")
    fields(attestation, ("manifest_sha256", "source", "identity", "attested_by"), "attestation", "ATTESTATION_INVALID")
    validate_identity(attestation["identity"], "ATTESTATION_INVALID")
    require(sha256_string(attestation["manifest_sha256"]) and label(attestation["attested_by"]), "ATTESTATION_INVALID",
            "Attestation must identify the reviewer and provide a lowercase manifest SHA-256.")
    require(attestation["manifest_sha256"] == digest and attestation["source"] == source
            and attestation["identity"] == manifest["identity"], "ATTESTATION_MISMATCH",
            "Attestation does not bind this exact manifest, source class and configuration identity.")
    return "PROVENANCE_VERIFIED", f"Origin is bound by the caller-trusted attestation from {attestation['attested_by']}."


def run_native(path, validate_only, metadata, raw):
    args = [str(path.absolute()), *(["--validate"] if validate_only else []), *map(str, metadata)]
    try:
        result = subprocess.run(args, input=raw, capture_output=True, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise PackError("RUNNER_ERROR", f"Cannot complete execution of {path.name}.") from error
    require(result.returncode == 0, "RUNNER_ERROR", f"The native runner exited with status {result.returncode}.")
    require(len(result.stdout) <= JSON_LIMIT, "RUNNER_ERROR", "The runner report exceeds the supported size.")
    report = json_object(result.stdout, "RUNNER_ERROR")
    require(type(report.get("trace_valid")) is bool and type(report.get("candidate_available")) is bool
            and type(report.get("review_required")) is bool and label(report.get("calibration_state"))
            and label(report.get("reason")) and type(report.get("stages")) is dict
            and all(label(name) and type(stage) is dict for name, stage in report["stages"].items())
            and integer(report.get("rejected_samples"), 0, ROW_LIMIT), "RUNNER_ERROR",
            "The native runner report is missing required V1 evidence.")
    return report


def explain(report):
    evidence = report["workflow"]
    notes = [report["provenance_detail"]] if report["provenance_detail"] else []
    if evidence is None:
        notes.append(report["detail"])
        return notes
    stages = evidence["stages"]
    for name, stage in stages.items():
        reason = stage.get("reason")
        quality = stage.get("quality")
        if label(reason) and reason != "NONE":
            detail = REASON_DETAILS.get(reason, "See the stage's sample counts and workflow result.")
            notes.append(f"{name} reported {reason}. {detail}")
        elif label(quality) and quality not in ("PASS", "NOT_SELECTED", "NOT_RUN"):
            detail = REASON_DETAILS.get(quality, "See the stage's native algorithm and workflow statuses.")
            notes.append(f"{name} reported {quality}. {detail}")
    if report["reason"] in REASON_DETAILS and all(stage.get("reason") != report["reason"] for stage in stages.values()):
        notes.append(REASON_DETAILS[report["reason"]])
    if report["candidate_available_before_provenance"] and not report["can_generate_candidate"]:
        notes.append("The offline candidate requires verified external provenance.")
    elif report["can_generate_candidate"]:
        notes.append("The offline candidate requires explicit review and initialization through the application workflow.")
    else:
        notes.append("Candidate generation requires a complete request and calibration admission.")
    return notes


def print_result(report, as_json):
    if as_json:
        print(json.dumps(report, ensure_ascii=True, allow_nan=False, sort_keys=True))
        return
    print(f"{report['command']} trace_valid={report['trace_valid']} state={report['calibration_state']} "
          f"reason={report['reason']} rejected_samples={report['rejected_samples']}")
    print(f"source={report['source']} provenance={report['provenance']} manifest_sha256={report['manifest_sha256']}")
    print(f"candidate={report['can_generate_candidate']} application_eligible={report['application_eligible']} "
          f"applied={report['applied']} persisted={report['persisted']}")
    if report["detail"] and report["command"] != "explain":
        print(report["detail"])
    if report["workflow"] is not None:
        workflow = report["workflow"]
        for name, stage in workflow["stages"].items():
            print(f"{name} {json.dumps(stage, sort_keys=True)}")
        if report["command"] != "validate" and "metrics" in workflow:
            print(f"metrics {json.dumps(workflow['metrics'], sort_keys=True)}")
    for note in report.get("explanation", ()):
        print(note)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("validate", "replay", "explain"))
    parser.add_argument("pack", type=Path, metavar="pack-directory")
    parser.add_argument("--runner", type=Path, required=True, help="path to the native calibration replay runner")
    parser.add_argument("--attestation", type=Path, help="caller-trusted external origin attestation outside the pack")
    parser.add_argument("--json", action="store_true", help="emit stable JSON")
    args = parser.parse_args(argv)
    validate_only = args.command == "validate"
    report = dict(format_version=FORMAT_VERSION, command=args.command, manifest_sha256=None, samples_sha256=None,
                  source=None, provenance=None, provenance_detail=None, trace_valid=False, calibration_state="NOT_RUN",
                  rejected_samples=0, reason=None, detail=None, workflow=None, capture_identity=None, result_identity=None,
                  candidate_available_before_provenance=False, can_generate_candidate=False, application_eligible=False,
                  applied=False, persisted=False)
    ok = False
    try:
        manifest_raw = read_bounded(args.pack / "manifest.json", JSON_LIMIT)
        digest = hashlib.sha256(manifest_raw).hexdigest()
        report["manifest_sha256"] = digest
        manifest = json_object(manifest_raw, "MANIFEST_INVALID")
        metadata = validate_manifest(manifest)
        report["source"] = manifest["source"]
        report["capture_identity"] = {key: manifest[key] for key in (
            "identity", "session", "profile", "stage_mask", "adc", "encoder", "reference", "torque_model_id"
        )}
        samples = manifest["files"][CSV_NAME]
        raw = read_bounded(args.pack / CSV_NAME, SAMPLE_LIMIT)
        report["samples_sha256"] = hashlib.sha256(raw).hexdigest()
        require(report["samples_sha256"] == samples["sha256"], "HASH_MISMATCH", "samples.csv does not match its manifest hash.")
        report["provenance"], report["provenance_detail"] = attestation_status(manifest, digest, args.pack, args.attestation)
        validate_csv(raw, samples["rows"])
        native = run_native(args.runner, validate_only, metadata, raw)
        report.update({key: native[key] for key in ("trace_valid", "calibration_state", "rejected_samples", "reason")})
        report["workflow"] = native
        report["result_identity"] = native.get("result_identity")
        ok = native["trace_valid"] and (validate_only or (native["calibration_state"] == "COMPLETED"
                                                          and native["reason"] == "NONE"))
        report["candidate_available_before_provenance"] = ok and not validate_only and native["candidate_available"]
        report["can_generate_candidate"] = (report["candidate_available_before_provenance"]
                                             and report["provenance"] != "PROVENANCE_UNVERIFIED")
    except PackError as error:
        report["reason"], report["detail"] = error.reason, str(error)
    if args.command == "explain":
        report["explanation"] = explain(report)
    print_result(report, args.json)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
