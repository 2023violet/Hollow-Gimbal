"""Export the SOFT-05 Host example as version 1 measurement packs."""

import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile


FIELDS = (
    "stage,capture,sequence,source_us,completion_us,trigger_us,reference_us,clock_id,"
    "reference_clock_id,unit,channel,flags,evidence,p1,p2,p3,p4,p5,p6"
).split(",")
BASELINE_ROWS = [64, 321, 40, 40, 256, 2401, 1, 171]
LUT_STAGE = 4
HOLDOUT_STAGE = 7
LUT_SKEW_US = -20


def write_pack(directory, samples, stage_rows, bad_alignment=False):
    sha256 = hashlib.sha256(samples).hexdigest()
    manifest = {
        "format_version": 1,
        "source": "HOST_SYNTHETIC",
        "host_synthetic": True,
        "session": 3,
        "recorder": {
            "name": "falconfoc-calibration-workflow-demo",
            "version": "0.1.0-trace-v1",
        },
        "identity": {"device_id": 1, "motor_id": 201, "encoder_id": 101, "frame_id": 71},
        "profile": "SELECTED_STAGES" if bad_alignment else "FULL_PROFILE",
        "stage_mask": 1 << LUT_STAGE if bad_alignment else 127,
        "stage_rows": stage_rows,
        "time": {
            "unit": "us", "clock_id": 1, "max_age_us": 50, "max_skew_us": 5,
            "allow_wrap": False, "alignment": "SAME_CLOCK_PAIR",
        },
        "adc": {"max_count": 4095, "amp_per_count": 0.001, "channel_map": [1, 2, 3]},
        "encoder": {"cpr": 1024, "raw_direction": -1, "phase_order": 0, "pole_pairs": 4},
        "reference": {"id": 41, "zero_rad": 0.31, "frame_direction": 1},
        "torque_model_id": 13,
        "units": {
            "adc": "count", "current": "A", "voltage": "V", "angle": "rad",
            "encoder": "count", "speed": "rad/s", "torque": "N*m", "kt": "N*m/A",
        },
        "instruments": [
            {
                "id": "soft05-analytic-fixture",
                "model": "Deterministic C11 capture producers and shared simulated clock",
                "calibration": "Synthetic counts, terminal voltage, angle references, load and Kt",
            },
        ],
        "preconditions": {
            "adc_zero_current": "Zero current by construction, independent of ADC values",
            "output_confirmation": "Simulated held terminal voltage and observed d/q currents",
            "independent_reference": "Analytic angular reference with separate LUT holdout capture",
            "load_condition": "Signed external load observed by synthetic fixture",
            "independent_kt": "Qualified synthetic input in dedicated capture 9",
        },
        "files": {"samples.csv": {"sha256": sha256, "rows": sum(stage_rows)}},
        "generator": {
            # Reserved deterministic seed value. No pseudorandom generator is used.
            "seed": 0,
            "sample_rates_hz": {
                "adc": 100000, "electrical": 25000, "lut_train": 100000,
                "lut_holdout": 100000, "rotor": 1000, "offset": 1000, "mechanical_mean": 1000,
            },
            "parameters": {
                "rs_ohm": 0.57, "inductance_h": 0.00135, "inertia_kg_m2": 0.0024,
                "damping_nm_s_per_rad": 0.007, "kt_nm_per_a": 0.15,
                "electrical_offset_rad": 1.2, "voltage_hold_samples": 19,
                "mechanical_odd_interval_us": 940, "mechanical_even_interval_us": 1060,
                "lut_nodes": 8, "lut_training_stride_counts": 4, "lut_holdout_stride_counts": 6,
                "rotor_increment_counts": 40, "sample_availability_delay_us": 3,
            },
        },
    }
    if bad_alignment:
        manifest["generator"]["parameters"]["first_training_reference_shift_us"] = LUT_SKEW_US
    directory.mkdir()
    (directory / "samples.csv").write_bytes(samples)
    (directory / "manifest.json").write_bytes((json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
    print(f"{directory.name} rows={sum(stage_rows)} samples_sha256={sha256}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("example", type=Path, help="built falconfoc-calibration-workflow-demo")
    parser.add_argument("output", type=Path, help="directory for the two new pack directories")
    args = parser.parse_args()
    valid = args.output / "host-valid"
    misaligned = args.output / "host-misaligned"
    if valid.exists() or misaligned.exists():
        parser.error("output already contains a pack directory")

    with tempfile.TemporaryDirectory(prefix="falconfoc-fixture-") as temp:
        exported = Path(temp) / "samples.csv"
        subprocess.run([str(args.example.resolve()), "--export", str(exported)], check=True,
                       stdout=subprocess.DEVNULL)
        samples = exported.read_bytes()

    reader = csv.reader(io.StringIO(samples.decode("ascii"), newline=""), strict=True)
    if next(reader) != FIELDS:
        parser.error("the example export uses a different CSV version")
    stage_rows = [0] * len(BASELINE_ROWS)
    training = []
    holdout = []
    for row in reader:
        stage = int(row[0])
        if len(row) != len(FIELDS) or not 0 <= stage < len(stage_rows):
            parser.error("the example exported an unexpected row")
        stage_rows[stage] += 1
        if stage == LUT_STAGE and len(training) < 2:
            training.append(row)
        elif stage == HOLDOUT_STAGE and not holdout:
            holdout.append(row)
    if stage_rows != BASELINE_ROWS:
        parser.error(f"the example stage counts changed: {stage_rows}")

    reference_index = FIELDS.index("reference_us")
    training[0][reference_index] = str(int(training[0][reference_index]) + LUT_SKEW_US)
    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(FIELDS)
    writer.writerows(training + holdout)
    bad_rows = [0] * len(stage_rows)
    bad_rows[LUT_STAGE] = len(training)
    bad_rows[HOLDOUT_STAGE] = len(holdout)

    args.output.mkdir(parents=True, exist_ok=True)
    write_pack(valid, samples, stage_rows)
    write_pack(misaligned, buffer.getvalue().encode("ascii"), bad_rows, bad_alignment=True)


if __name__ == "__main__":
    main()
