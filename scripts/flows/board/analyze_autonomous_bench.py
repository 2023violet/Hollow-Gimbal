"""Summarize AutonomousDiag CSV evidence without claiming waveform proof."""

import argparse
import csv
import json
from pathlib import Path


def read_rows(path: Path):
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", required=True)
    args = parser.parse_args()
    root = Path(args.input_dir)

    speed_rows = read_rows(root / "speed-step-summary.csv")
    position_rows = read_rows(root / "encoder-position.csv")
    map_rows = read_rows(root / "adc-map-trigger.csv")
    zero_rows = read_rows(root / "adc-zero-repeat.csv")

    failures = []
    if not speed_rows:
        failures.append("speed_step_missing")
    if not position_rows:
        failures.append("position_scan_missing")
    if not map_rows:
        failures.append("adc_map_trigger_missing")
    if not zero_rows:
        failures.append("adc_zero_repeat_missing")
    if any(row.get("result") != "pass" for row in speed_rows):
        failures.append("speed_step")
    if any(row.get("result") == "freeze_candidate" for row in position_rows):
        failures.append("position_freeze_candidate")
    if any(row.get("result") != "recorded" for row in map_rows):
        failures.append("adc_map_trigger")
    if any(row.get("result") != "pass" for row in zero_rows):
        failures.append("adc_zero_repeat")

    result = {
        "result": "pass" if not failures else "fail",
        "failures": failures,
        "physical_power_cycle": False,
        "psu_telemetry": "unavailable",
        "speed_step_rows": len(speed_rows),
        "position_rows": len(position_rows),
        "map_trigger_rows": len(map_rows),
        "adc_zero_rows": len(zero_rows),
        "root_cause_scope": "serial_and_bus_only",
        "waveform_or_magnetic_root_cause": "unverified",
    }
    (root / "analysis.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
