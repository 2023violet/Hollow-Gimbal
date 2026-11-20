"""Score the production Host Current Startup V1 CSV."""

import argparse
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import sys

H0_US = 200
BAND_HOLD_US = 2000
RS_OHM = {"GENERIC_RESEARCH_MOTOR": 0.60, "CALIBRATION_SALIENT_MOTOR": 0.78}
STIMULI = {
    "cold_small": (0.15,),
    "cold_nominal": (0.45,),
    "cold_large": (2.20,),
    "running_reverse": (0.45, -0.45),
    "limited_reverse": (2.20, -0.35),
    "mismatch_fast_electrical": (0.45,),
    "mismatch_slow_electrical": (0.45,),
}
FLOAT_FIELDS = {
    "iq_ref_a", "iq_adc_a", "iq_plant_a", "voltage_v", "voltage_limit_v",
    "id_integral_v", "iq_integral_v", "shaft_speed_rad_s", "vbus_v", "current_limit_a",
}


class QualityFailure(ValueError):
    """A complete observation violated a fixed control or transaction gate."""


def require(condition, context):
    if not condition:
        raise ValueError(context)


def phase_metrics(rows, start_us):
    target = rows[0]["iq_ref_a"]
    band = max(0.04, 0.10 * abs(target))
    errors = [row["iq_plant_a"] - target for row in rows]
    times = [row["time_us"] for row in rows]
    in_band = [abs(error) <= band for error in errors]
    first = next((time for time, inside in zip(times, in_band) if inside), None)
    last_out = max((i for i, inside in enumerate(in_band) if not inside), default=-1)
    settled = times[last_out + 1] if last_out + 1 < len(times) else None
    hold = times[-1] - settled if settled is not None else 0
    if hold < BAND_HOLD_US:
        settled = None
    tail = errors[len(errors) * 3 // 4:]
    return {
        "target_a": target,
        "band_a": band,
        "published_us": rows[0]["published_us"],
        "sample_us": rows[0]["sample_us"],
        "h0_us": rows[0]["h0_us"],
        "update_us": start_us,
        "model_apply_us": rows[0]["model_apply_us"],
        "publication_to_model_us": start_us - rows[0]["published_us"],
        "first_in_band_us": first,
        "first_in_band_after_apply_us": first - start_us if first is not None else None,
        "settled_us": settled,
        "settle_after_apply_us": settled - start_us if settled is not None else None,
        "observed_hold_us": hold,
        "rms_a": math.sqrt(sum(error * error for error in errors) / len(errors)),
        "peak_error_a": max(map(abs, errors)),
        "overshoot_a": max(0.0, *(math.copysign(1.0, target) * error for error in errors)),
        "tail_bias_a": sum(tail) / len(tail),
        "voltage_first_v": rows[0]["voltage_v"],
        "peak_voltage_v": max(row["voltage_v"] for row in rows),
        "near_cap_first": bool(rows[0]["near_voltage_cap"]),
        "near_cap_total_us": H0_US * sum(row["near_voltage_cap"] for row in rows),
        "near_cap_in_tail": any(row["near_voltage_cap"] for row in rows[len(rows) * 3 // 4:]),
        "integral_after_first_update_v": [rows[0]["id_integral_v"], rows[0]["iq_integral_v"]],
        "integral_final_v": [rows[-1]["id_integral_v"], rows[-1]["iq_integral_v"]],
    }


def evaluate_case(profile, name, direction, rows):
    context = f"{name}/{direction}"
    require([row["time_us"] for row in rows] == list(range(0, 62801, H0_US)), context)
    controlled = []
    for row in rows:
        require(row["time_us"] == row["h0_us"], context)
        if row["faults"]:
            raise QualityFailure(f"{context}: unexpected fault")
        if row["applied"]:
            controlled.append(row)
            if (row["state"] != 3 or row["action"] != 1 or not row["output_confirmed"]
                    or not row["adc_seen"] or row["sample_us"] != row["h0_us"]
                    or row["model_apply_us"] != row["h0_us"]
                    or row["consumed_us"] != row["published_us"]
                    or abs(row["iq_ref_a"]) > row["current_limit_a"]
                    or row["voltage_v"] > row["voltage_limit_v"] * (1 + 16 * 2**-23) + 1e-7):
                raise QualityFailure(f"{context}: invalid active output at {row['h0_us']} us")
        elif row["action"] != 0 or row["voltage_v"] != 0:
            raise QualityFailure(f"{context}: output without a confirmed active update")
    if ([row["time_us"] for row in controlled] != list(range(2000, 61801, H0_US))
            or [row["updates"] for row in controlled] != list(range(1, 301))
            or len({row["session"] for row in controlled}) != 1):
        raise QualityFailure(f"{context}: missing or duplicated active output")
    after_stop = rows[-5:]
    if not all(row["updates"] == 300 and row["output_confirmed"] and row["state"] == 1
               for row in after_stop):
        raise QualityFailure(f"{context}: output remained active after stop")
    first_command = next(row for row in rows if row["command_seen"])
    first_adc = next(row for row in rows if row["adc_seen"])
    start = next(row for row in rows if row["state"] == 3)
    if start["time_us"] != 1800 or start["action"] != 0:
        raise QualityFailure(f"{context}: START and first UPDATE must be distinct")
    require(first_command["published_us"] == 700 and first_command["h0_us"] == 800, context)
    has_switch = len(STIMULI[name]) == 2
    parts = [controlled[:150], controlled[150:]] if has_switch else [controlled]
    expected = [target if direction == "positive" else -target for target in STIMULI[name]]
    result = {
        "first_command_us": first_command["published_us"],
        "first_command_observed_h0_us": first_command["h0_us"],
        "first_valid_adc_us": first_adc["sample_us"],
        "start_applied_us": start["time_us"],
        "first_update_us": controlled[0]["time_us"],
        "first_command_to_model_us": controlled[0]["model_apply_us"] - first_command["published_us"],
        "stop_inhibit_us": after_stop[0]["time_us"],
        "updates_after_stop": after_stop[-1]["updates"] - after_stop[0]["updates"],
        "phases": [],
    }
    for index, (part, target) in enumerate(zip(parts, expected)):
        require(all(abs(row["iq_ref_a"] - target) < 1e-6 for row in part), context)
        start = part[0]["model_apply_us"]
        require(part[0]["published_us"] == (31800 if index else 1800), context)
        metrics = phase_metrics(part, start)
        settle = metrics["settle_after_apply_us"]
        if name == "limited_reverse" and index == 0:
            resistive_v = RS_OHM[profile] * abs(target)
            require(all(abs(row["voltage_limit_v"] - 0.72) < 1e-6 for row in part), context)
            feasible_limit_witness = (resistive_v > part[0]["voltage_limit_v"]
                                      and metrics["near_cap_in_tail"] and settle is None)
            metrics["static_voltage_needed_v"] = resistive_v
            metrics["verdict"] = ("LIMITED_BY_FEASIBILITY" if feasible_limit_witness
                                  else "CONTROL_QUALITY_FAIL")
        else:
            max_settle = (15000 if name == "limited_reverse"
                          else 8000 if name.startswith("mismatch_") else 4000)
            max_overshoot = 0.18 if abs(target) > 2.0 else 0.08
            passed = (settle is not None and settle <= max_settle
                      and metrics["overshoot_a"] <= max_overshoot
                      and not metrics["near_cap_in_tail"])
            metrics["verdict"] = "CONTROL_QUALITY_PASS" if passed else "CONTROL_QUALITY_FAIL"
        result["phases"].append(metrics)
    return result


def score(path, profile):
    cases = defaultdict(list)
    with Path(path).open(newline="", encoding="ascii") as stream:
        for row in csv.DictReader(stream):
            name, direction = row.pop("scenario"), row.pop("direction")
            values = {key: (float(value) if key in FLOAT_FIELDS else int(value))
                      for key, value in row.items()}
            require(all(math.isfinite(value) for value in values.values()), (name, direction))
            cases[name, direction].append(values)
    require(cases.keys() == {(name, sign) for name in STIMULI for sign in ("negative", "positive")},
            "startup matrix is incomplete")
    return {
        "quality_version": 1,
        "profile": profile,
        "cases": {f"{name}/{sign}": evaluate_case(profile, name, sign, rows)
                  for (name, sign), rows in cases.items()},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=RS_OHM)
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()
    try:
        result = score(args.csv, args.profile)
    except QualityFailure as error:
        print(json.dumps({"verdict": "CONTROL_QUALITY_FAIL", "error": str(error)}))
        return 1
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        print(json.dumps({"verdict": "INSUFFICIENT_EVIDENCE", "error": str(error)}))
        return 2
    print(json.dumps(result, indent=2))
    return int(any(phase["verdict"] == "CONTROL_QUALITY_FAIL"
                   for case in result["cases"].values() for phase in case["phases"]))


if __name__ == "__main__":
    sys.exit(main())
