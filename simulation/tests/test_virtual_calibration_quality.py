"""Evaluate reproducible Host calibration trials against independent model witnesses."""

import argparse
import csv
import hashlib
import json
import math
import pathlib
import statistics
import subprocess
import tempfile
from collections import defaultdict


PROFILES = ("GENERIC_RESEARCH_MOTOR", "CALIBRATION_SALIENT_MOTOR")
TRIALS = (
    (137, 2000, -0.9, 0), (509, 7200, 0.31, 1), (2017, 11400, 1.3, 0),
    (9661, 23800, 3.14, 1), (41719, 46000, 5.8, 0), (65353, 79300, 6.25, 1),
)
CONTROL = {"0": ("iq_a", 0.045), "1": ("speed_rad_s", 0.25),
           "2": ("position_rad", 0.045), "3": ("position_rad", 0.035)}
DISCRETE = {"lut_relative_direction", "impedance_filter_hz"}
ABSOLUTE = {"coarse_offset_rad": 0.10, "offset_rad": 0.002,
            "adc_a_counts": 1.0, "adc_b_counts": 1.0, "adc_c_counts": 1.0}
REQUIRED = {
    "adc_a_counts", "adc_b_counts", "adc_c_counts", "offset_rad", "rs_ohm",
    "ld_h", "lq_h", "flux_wb", "ke", "kt", "j_kg_m2", "b_nm_s_rad",
    "current_kp_d", "current_kp_q", "current_ki", "velocity_kp", "velocity_ki",
    "position_kp", "impedance_k", "impedance_d", *DISCRETE,
}


def read_rows(path):
    with path.open(newline="", encoding="ascii") as stream:
        return list(csv.DictReader(stream))


def unique(events, name):
    rows = [row for row in events if row.get("event") == name]
    assert len(rows) == 1, (name, rows)
    return rows[0]


def measure_check(estimate, oracle, name):
    error = estimate - oracle
    if name.endswith("offset_rad"):
        error = math.remainder(error, math.tau)
    relative = error / abs(oracle) if oracle else None
    if name in ABSOLUTE:
        passed = abs(error) <= ABSOLUTE[name]
    elif name in DISCRETE:
        passed = error == 0.0
    else:
        passed = relative is not None and abs(relative) <= 0.03
    return {"estimate": estimate, "oracle": oracle, "absolute_error": abs(error),
            "signed_error": error, "relative_error": relative, "passed": passed}


def lut_check(holdout):
    rows = read_rows(holdout)
    assert len(rows) == 1200
    directions = {}
    for direction in ("-1", "1"):
        run = [row for row in rows if row["direction"] == direction]
        assert len(run) == 600
        truth = [float(row["shaft_rad"]) for row in run]
        assert abs(truth[-1] - truth[0]) > 6.28
        errors = {}
        for label, column in (("raw", "raw_rad"), ("corrected", "corrected_rad")):
            residuals = [math.remainder(float(row[column]) - angle, math.tau)
                         for row, angle in zip(run, truth)]
            errors[f"{label}_rms_rad"] = math.sqrt(statistics.mean(e * e for e in residuals))
            errors[f"{label}_peak_rad"] = max(map(abs, residuals))
        errors["max_electrical_error_rad"] = max(abs(float(row["electrical_error_rad"])) for row in run)
        errors["passed"] = (errors["corrected_rms_rad"] < 0.2 * errors["raw_rms_rad"]
                            and errors["max_electrical_error_rad"] < 0.005)
        directions[direction] = errors
    return directions


def control_check(path):
    grouped = defaultdict(list)
    for row in read_rows(path):
        grouped[(row["mode"], row["direction"])].append(row)
    assert set(grouped) == {(mode, sign) for mode in CONTROL for sign in ("-1", "1")}
    measurements = {}
    for (mode, sign), rows in grouped.items():
        feedback, limit = CONTROL[mode]
        tail = rows[-len(rows) // 4:]
        rms = math.sqrt(statistics.mean((float(row[feedback]) - float(row["target"])) ** 2 for row in tail))
        max_voltage = max(float(row["voltage_v"]) for row in rows)
        max_iq = max(abs(float(row["iq_ref_a"])) for row in rows)
        assert all(int(b["updates"]) > int(a["updates"]) for a, b in zip(rows, rows[1:]))
        measurements[f"{mode}/{sign}"] = {
            "tail_rms": rms, "tail_rms_limit": limit, "peak_confirmed_voltage_v": max_voltage,
            "peak_command_iq_a": max_iq, "passed": rms <= limit and max_voltage <= 2.401 and max_iq <= 2.5,
        }
    return measurements


def independent_prediction(candidate, rows):
    """Compare a frozen fit with different training apertures as held-out input."""
    values = {name: value["estimate"] for name, value in candidate.items()}
    data = defaultdict(list)
    for row in rows:
        data[row["stage"]].append(row)
    # R/L uses the existing 25 mV capture budget. Keep the 15 mV exploratory
    # diagnostic alongside each applied-model and exact-oracle prediction.
    limits = {"ADC_ZERO": 1.0, "ELECTRICAL_D": 0.025, "ELECTRICAL_Q": 0.025,
              "FLUX": 0.008, "MECHANICAL": 0.001}
    result = {}
    adc = data["ADC_ZERO"]
    offsets = [statistics.mean(float(row[key]) for row in adc) for key in "abc"]
    samples = {"ADC_ZERO": [offsets[i] - values[f"adc_{key}_counts"] for i, key in enumerate("abc")]}
    for stage, inductance in (("ELECTRICAL_D", "ld_h"), ("ELECTRICAL_Q", "lq_h")):
        wave = data[stage]
        residuals = []
        for start in range(0, len(wave) - 8, 8):
            first, last = wave[start], wave[start + 8]
            elapsed = (int(last["end_us"]) - int(first["end_us"])) * 1e-6
            window = wave[start:start + 9]
            current = [float(row["a"]) for row in window]
            mean_current = (sum(current[1:-1]) + (current[0] + current[-1]) / 2) / 8
            mean_voltage = statistics.mean(float(row["b"]) for row in window[1:])
            residuals.append(mean_voltage - values["rs_ohm"] * mean_current -
                             values[inductance] * (current[-1] - current[0]) / elapsed)
        samples[stage] = residuals
    intervals = data["FLUX"]
    residuals = []
    for start in range(0, len(intervals), 32):
        window = intervals[start:start + 32]
        first, last = window[0], window[-1]
        elapsed = (int(last["end_us"]) - int(first["begin_us"])) * 1e-6
        mean_iq = statistics.mean((float(row["b"]) + float(row["d"])) / 2 for row in window)
        average_cross = statistics.mean(float(row["e"]) *
                                         (float(row["a"]) + float(row["c"])) / 2 for row in window)
        omega = statistics.mean(float(row["e"]) for row in window)
        voltage = statistics.mean(float(row["f"]) for row in window)
        residuals.append(voltage - values["rs_ohm"] * mean_iq - values["ld_h"] * average_cross -
                         values["lq_h"] * (float(last["d"]) - float(first["b"])) / elapsed -
                         values["flux_wb"] * omega)
    samples["FLUX"] = residuals
    wave = data["MECHANICAL"]
    residuals = []
    for start in range(0, len(wave) - 50, 50):
        window = wave[start:start + 51]
        elapsed = (int(window[-1]["end_us"]) - int(window[0]["end_us"])) * 1e-6
        speed = [float(row["a"]) for row in window]
        net = [float(row["b"]) - float(row["c"]) for row in window]
        mean_speed = (sum(speed[1:-1]) + (speed[0] + speed[-1]) / 2) / 50
        mean_net = (sum(net[1:-1]) + (net[0] + net[-1]) / 2) / 50
        residuals.append(mean_net - values["b_nm_s_rad"] * mean_speed -
                         values["j_kg_m2"] * (speed[-1] - speed[0]) / elapsed)
    samples["MECHANICAL"] = residuals
    for stage, residuals in samples.items():
        assert residuals
        rms = math.sqrt(statistics.mean(value * value for value in residuals))
        result[stage] = {"rms": rms, "limit": limits[stage], "windows": len(residuals),
                         "passed": rms <= limits[stage]}
    return result


def value_hash(rows, columns):
    return hashlib.sha256(json.dumps([[row[column] for column in columns] for row in rows])
                          .encode("ascii")).hexdigest()


def execute(simulator, output):
    runs = {}
    failures = []
    for profile, direction in zip(PROFILES, (1, -1)):
        captures = []
        runs[profile] = captures
        for session, trial in enumerate(TRIALS, 1):
            seed, start, angle, reverse_first = trial
            folder = output / profile / str(seed)
            folder.mkdir(parents=True)
            paths = [folder / name for name in ("bootstrap.csv", "training.csv", "lut-holdout.csv", "control.csv")]
            command = [str(simulator), profile, str(session), *map(str, trial), *map(str, paths)]
            result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=35)
            (folder / "events.jsonl").write_text(result.stdout, encoding="utf-8")
            if result.stderr:
                (folder / "stderr.log").write_text(result.stderr, encoding="utf-8")
            events = [json.loads(line) for line in result.stdout.splitlines()]
            run = {"command": command, "session": session, "seed": seed, "start_us": start,
                   "initial_position_rad": angle, "reverse_first": bool(reverse_first),
                   "exit_status": result.returncode,
                   "failed_reason": next((row.get("error") for row in events if row.get("error")), None)}
            captures.append(run)
            if result.returncode != 0:
                failures.append((profile, seed, run["failed_reason"]))
                continue
            final = unique(events, "TRIAL_COMPLETE")
            coarse = unique(events, "BOOTSTRAP_COARSE_REFERENCE")
            capture = unique(events, "BOOTSTRAP_CAPTURE")
            assert final["session"] == session and final["original_updates"] == capture["accepted_updates"] == 0
            assert capture["output_inhibited"] and capture["samples"] == 256
            assert coarse["pole_pairs"] == 4 and coarse["encoder_direction"] == direction
            expected_metrics = {row["name"]: (row["estimate"], row["oracle"])
                                for row in events if row.get("event") == "MEASUREMENT"}
            assert REQUIRED <= expected_metrics.keys()
            expected_metrics["coarse_offset_rad"] = (coarse["offset_rad"], expected_metrics["offset_rad"][1])
            per_metric = {name: measure_check(*values, name) for name, values in expected_metrics.items()}
            boot = read_rows(paths[0])
            assert len(boot) == 256
            assert (float(boot[0]["shaft_rad_s"]) < 0) == bool(reverse_first)
            assert int(boot[0]["aperture_us"]) > start + 9600
            witnesses = lut_check(paths[2])
            controls = control_check(paths[3])
            training = read_rows(paths[1])
            run.update({"metrics": per_metric, "lut_holdout": witnesses, "control_holdout": controls,
                        "boot_value_hash": value_hash(boot, ("raw_encoder", "shaft_rad_s", "emf_alpha_v", "emf_beta_v")),
                        "training_value_hash": value_hash(training, ("stage", "encoder", *tuple("abcdef"))),
                        "pole_pairs": coarse["pole_pairs"], "direction": coarse["encoder_direction"],
                        "flux_direction_delta_wb": abs(expected_metrics["flux_positive_wb"][0] -
                                                       expected_metrics["flux_negative_wb"][0])})
            for key, values in (("oracle", per_metric), ("LUT", witnesses), ("control", controls)):
                failures += [(profile, seed, key, group, value) for group, value in values.items()
                             if not value["passed"]]
    summary = {}
    for profile, records in runs.items():
        good = [row for row in records if row["exit_status"] == 0]
        assert len(set(row["boot_value_hash"] for row in good)) == len(good)
        assert len(set(row["training_value_hash"] for row in good)) == len(good)
        if len(good) > 1:
            for fit, holdout in zip(good, good[1:] + good[:1]):
                rows = read_rows(output / profile / str(holdout["seed"]) / "training.csv")
                metrics = independent_prediction(fit["metrics"], rows)
                oracle = {name: {"estimate": value["oracle"]} for name, value in fit["metrics"].items()}
                floor = independent_prediction(oracle, rows)
                for stage, result in metrics.items():
                    result["oracle_rms"] = floor[stage]["rms"]
                    if stage.startswith("ELECTRICAL_"):
                        result["above_exploratory_15mv"] = result["rms"] > 0.015
                fit["independent_prediction"] = {
                    "holdout_session": holdout["session"], "holdout_seed": holdout["seed"], "metrics": metrics
                }
                failures += [(profile, fit["seed"], "prediction", group, value)
                             for group, value in metrics.items() if not value["passed"]]
        stats = {}
        for name in good[0]["metrics"] if good else []:
            readings = [row["metrics"][name] for row in good]
            estimated = [v["estimate"] for v in readings]
            errors = [v["signed_error"] for v in readings]
            relative = [abs(v["relative_error"]) for v in readings if v["relative_error"] is not None]
            stats[name] = {"oracle": readings[0]["oracle"],
                           "estimate_mean": statistics.mean(estimated),
                           "sample_error_stdev": statistics.stdev(errors) if len(errors) > 1 else None,
                           "max_abs_error": max(map(abs, errors)),
                           "max_abs_deviation_from_mean_error": max(abs(e - statistics.mean(errors)) for e in errors),
                           "max_relative_error": max(relative) if relative else None}
        summary[profile] = {"independent_trials": len(records), "estimated_trials": len(good),
                            "failed_rate": 1.0 - len(good) / len(records),
                            "unidentifiable_rate": sum(row["failed_reason"] == "REFERENCE_UNIDENTIFIABLE"
                                                       for row in records) / len(records),
                            "parameter_statistics": stats,
                            "maximum_flux_direction_difference_wb": max(
                                (row["flux_direction_delta_wb"] for row in good), default=None)}
    report = {"profiles": summary, "runs": runs, "failures": failures}
    (output / "repeatability-summary.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n",
                                                      encoding="utf-8")
    assert not failures, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulator", type=pathlib.Path)
    parser.add_argument("--output-dir", type=pathlib.Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir()
        execute(args.simulator.resolve(), args.output_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="falconfoc-cal-repeat-") as folder:
            execute(args.simulator.resolve(), pathlib.Path(folder))


if __name__ == "__main__":
    main()
