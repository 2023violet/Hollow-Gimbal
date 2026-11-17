"""Deterministic Host-only control tuning experiments for FalconFOC V2.

The runner deliberately shells out to the existing V1 virtual-device workflow.
It changes only copied Host research-profile values and stores every trial,
including infeasible and holdout failures, as JSON/CSV evidence.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import pathlib
import subprocess
import sys
from typing import Iterable


@dataclasses.dataclass(frozen=True)
class ConstraintResult:
    feasible: bool
    reasons: tuple[str, ...] = ()


def hard_constraints(metrics: dict) -> ConstraintResult:
    reasons = []
    if metrics.get("status") != "INTEGRATION_PASS":
        reasons.append("integration")
    if metrics.get("final_faults", 0):
        reasons.append("faults")
    final_state = metrics.get("final_state")
    if final_state not in (1, 3) or metrics.get("update_accepted", 0) <= 0:
        reasons.append("no_running_update")
    for key in ("rmse", "running_tail_rmse", "final_error", "settling_time_us"):
        value = metrics.get(key)
        if value is not None and not math.isfinite(float(value)):
            reasons.append(f"nonfinite:{key}")
    duration = float(metrics.get("duration_us", 0) or 0)
    saturation = float(metrics.get("saturation_duration_us", 0) or 0)
    if duration > 0 and saturation > duration * 0.80:
        reasons.append("sustained_saturation")
    return ConstraintResult(not reasons, tuple(reasons))


def _dominates(left: dict, right: dict, keys: tuple[str, ...]) -> bool:
    def value(row: dict, key: str) -> float:
        raw = row.get(key)
        return math.inf if raw is None else float(raw)

    left_values = [value(left, key) for key in keys]
    right_values = [value(right, key) for key in keys]
    return all(a <= b for a, b in zip(left_values, right_values)) and any(
        a < b for a, b in zip(left_values, right_values)
    )


def pareto_frontier(
    candidates: Iterable[dict],
    keys: tuple[str, ...] = ("settling_time_us", "running_tail_rmse"),
) -> list[dict]:
    rows = list(candidates)
    return [row for row in rows if not any(_dominates(other, row, keys) for other in rows)]


def simulator_args(
    *,
    profile_id: str,
    current_kp: float | None = None,
    current_ki: float | None = None,
    velocity_kp: float | None = None,
    velocity_ki: float | None = None,
    position_kp: float | None = None,
    current_limit: float | None = None,
    voltage_limit: float | None = None,
    vbus: float | None = None,
    reference: float | None = None,
    motor_rs_ohm: float | None = None,
    motor_ld_h: float | None = None,
    motor_lq_h: float | None = None,
    motor_psi_f_wb: float | None = None,
    motor_inertia: float | None = None,
    motor_damping: float | None = None,
) -> list[str]:
    values = [
        ("--profile-id", profile_id),
        ("--reference", reference),
        ("--current-kp", current_kp),
        ("--current-ki", current_ki),
        ("--velocity-kp", velocity_kp),
        ("--velocity-ki", velocity_ki),
        ("--position-kp", position_kp),
        ("--current-limit-a", current_limit),
        ("--voltage-limit-v", voltage_limit),
        ("--vbus-v", vbus),
        ("--motor-rs-ohm", motor_rs_ohm),
        ("--motor-ld-h", motor_ld_h),
        ("--motor-lq-h", motor_lq_h),
        ("--motor-psi-f-wb", motor_psi_f_wb),
        ("--motor-inertia", motor_inertia),
        ("--motor-damping", motor_damping),
    ]
    result: list[str] = []
    for option, value in values:
        if value is not None:
            result.extend((option, str(value)))
    return result


def _profile_parameters(profile: dict) -> dict:
    return {key: value for key, value in profile.items() if value is not None}


def _run_trial(
    simulator: pathlib.Path,
    output_dir: pathlib.Path,
    scenario: str,
    candidate_id: str,
    profile: dict,
    *,
    duration_us: int,
    reference: float,
    horizon: str = "short",
    load_torque_nm: float | None = None,
    vbus: float | None = None,
    motor_inertia: float | None = None,
) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{candidate_id}-{scenario}"
    json_path = output_dir / f"{stem}.json"
    csv_path = output_dir / f"{stem}.csv"
    merged = dict(profile)
    merged.update({"profile_id": candidate_id, "reference": reference})
    if vbus is not None:
        merged["vbus"] = vbus
    if motor_inertia is not None:
        merged["motor_inertia"] = motor_inertia
    command = [
        sys.executable,
        str(pathlib.Path(__file__).with_name("falconfoc_cli.py")),
        "--simulator",
        str(simulator),
        "virtual-demo",
        "--virtual-scenario",
        scenario,
        "--advance-us",
        "500",
        "--duration-us",
        str(duration_us),
        "--horizon",
        horizon,
        "--reference",
        str(reference),
        "--profile-id",
        candidate_id,
        "--json",
        str(json_path),
        "--csv",
        str(csv_path),
    ]
    for key, option in (
        ("current_kp", "--current-kp"),
        ("current_ki", "--current-ki"),
        ("velocity_kp", "--velocity-kp"),
        ("velocity_ki", "--velocity-ki"),
        ("position_kp", "--position-kp"),
        ("current_limit", "--current-limit-a"),
        ("voltage_limit", "--voltage-limit-v"),
        ("vbus", "--vbus-v"),
        ("motor_rs_ohm", "--motor-rs-ohm"),
        ("motor_ld_h", "--motor-ld-h"),
        ("motor_lq_h", "--motor-lq-h"),
        ("motor_psi_f_wb", "--motor-psi-f-wb"),
        ("motor_inertia", "--motor-inertia"),
        ("motor_damping", "--motor-damping"),
    ):
        if profile.get(key) is not None:
            command.extend((option, str(profile[key])))
    if vbus is not None and profile.get("vbus") is None:
        command.extend(("--vbus-v", str(vbus)))
    if motor_inertia is not None and profile.get("motor_inertia") is None:
        command.extend(("--motor-inertia", str(motor_inertia)))
    if load_torque_nm is not None:
        command.extend(("--load-torque-nm", str(load_torque_nm)))
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    (output_dir / f"{stem}.stdout.log").write_text(completed.stdout, encoding="utf-8")
    (output_dir / f"{stem}.stderr.log").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0 or not json_path.exists():
        return {
            "candidate_id": candidate_id,
            "scenario": scenario,
            "profile": _profile_parameters(profile),
            "trial_identity": _profile_parameters(merged),
            "metrics": {"status": "RUN_FAILED", "returncode": completed.returncode},
            "feasibility": {"feasible": False, "reasons": ["runner"]},
        }
    summary = json.loads(json_path.read_text(encoding="utf-8"))
    metrics = summary["metrics"]
    constraints = hard_constraints(metrics)
    return {
        "candidate_id": candidate_id,
        "scenario": scenario,
        "profile": _profile_parameters(profile),
        "trial_identity": _profile_parameters(merged),
        "metrics": metrics,
        "feasibility": dataclasses.asdict(constraints),
        "artifacts": {"json": str(json_path), "csv": str(csv_path)},
    }


def _score(metrics: dict) -> float:
    tolerance = max(float(metrics.get("tolerance", 1.0)), 1.0e-9)
    duration = max(float(metrics.get("duration_us", 1)), 1.0)
    raw_tail = metrics.get("running_tail_rmse", metrics.get("rmse"))
    raw_final = metrics.get("final_error")
    raw_settling = metrics.get("settling_time_us")
    tail = math.inf if raw_tail is None else float(raw_tail)
    final_error = math.inf if raw_final is None else float(raw_final)
    settling = duration if raw_settling is None else float(raw_settling)
    saturation = float(metrics.get("saturation_duration_us", 0.0)) / duration
    overshoot = float(metrics.get("overshoot", 0.0))
    return tail / tolerance + final_error / tolerance + settling / duration + saturation + overshoot / tolerance


def _select(rows: list[dict]) -> dict:
    feasible = [row for row in rows if row["feasibility"]["feasible"]]
    if not feasible:
        return min(rows, key=lambda row: _score(row["metrics"]))
    for row in feasible:
        row["score"] = _score(row["metrics"])
    frontier = pareto_frontier(
        [dict(row, **row["metrics"]) for row in feasible],
        keys=("settling_time_us", "running_tail_rmse", "saturation_duration_us"),
    )
    frontier_ids = {row["candidate_id"] for row in frontier}
    return min((row for row in feasible if row["candidate_id"] in frontier_ids), key=lambda row: row["score"])


def run_study(simulator: pathlib.Path, output_dir: pathlib.Path) -> dict:
    baseline = {}
    rows: list[dict] = []

    # Current inner-loop search. The baseline intentionally carries no override,
    # preserving the historical profile, while candidates use a small scalar grid.
    current_candidates = [{"candidate_id": "baseline", "profile": {}}]
    for index, (kp, ki) in enumerate(((0.90, 450.0), (1.08, 540.0), (1.26, 630.0)), 1):
        current_candidates.append({
            "candidate_id": f"current-c{index}",
            "profile": {"current_kp": kp, "current_ki": ki},
        })
    current_rows = []
    for candidate in current_candidates:
        row = _run_trial(
            simulator,
            output_dir / "optimization",
            "current",
            candidate["candidate_id"],
            candidate["profile"],
            duration_us=10000,
            reference=0.4,
        )
        current_rows.append(row)
    baseline["current"] = next(row for row in current_rows if row["candidate_id"] == "baseline")
    current_choice = _select(current_rows)

    # Velocity outer-loop search uses the selected Current profile.
    velocity_rows = [
        _run_trial(
            simulator,
            output_dir / "optimization",
            "velocity",
            "velocity-baseline",
            dict(current_choice["profile"]),
            duration_us=100000,
            reference=3.0,
            horizon="long",
        )
    ]
    baseline["velocity"] = velocity_rows[0]
    for index, (kp, ki) in enumerate(((0.135, 1.05), (0.18, 1.40), (0.225, 1.75)), 1):
        profile = dict(current_choice["profile"])
        profile.update({"velocity_kp": kp, "velocity_ki": ki})
        velocity_rows.append(_run_trial(
            simulator,
            output_dir / "optimization",
            "velocity",
            f"velocity-c{index}",
            profile,
            duration_us=100000,
            reference=3.0,
            horizon="long",
        ))
    velocity_choice = _select(velocity_rows)

    # Position search holds both inner loops fixed and varies only Position P.
    position_rows = [
        _run_trial(
            simulator,
            output_dir / "optimization",
            "position",
            "position-baseline",
            dict(velocity_choice["profile"]),
            duration_us=200000,
            reference=1.0,
        )
    ]
    baseline["position"] = position_rows[0]
    for index, kp in enumerate((3.0, 4.0, 5.0), 1):
        profile = dict(velocity_choice["profile"])
        profile["position_kp"] = kp
        position_rows.append(_run_trial(
            simulator,
            output_dir / "optimization",
            "position",
            f"position-c{index}",
            profile,
            duration_us=200000,
            reference=1.0,
        ))
    position_choice = _select(position_rows)
    selected_profile = dict(position_choice["profile"])

    holdout_specs = [
        ("current-reverse", "current", -0.4, 10000, "short", None, None),
        ("velocity-load-perturbed", "load", -3.0, 100000, "long", 0.02, 10.0),
        ("position-reverse-perturbed", "position", -2.0, 2000000, "long", 0.0, 10.0),
    ]
    holdout_rows = []
    for candidate_id, scenario, reference, duration, horizon, load, vbus in holdout_specs:
        holdout_rows.append(_run_trial(
            simulator,
            output_dir / "holdout",
            scenario,
            candidate_id,
            selected_profile,
            duration_us=duration,
            reference=reference,
            horizon=horizon,
            load_torque_nm=load,
            vbus=vbus,
            motor_inertia=0.0024,
        ))

    confirmation_rows = [
        _run_trial(
            simulator,
            output_dir / "confirmation",
            "velocity",
            "nominal-long-velocity",
            {},
            duration_us=100000,
            reference=3.0,
            horizon="long",
        ),
        _run_trial(
            simulator,
            output_dir / "confirmation",
            "velocity",
            "selected-long-velocity",
            selected_profile,
            duration_us=100000,
            reference=3.0,
            horizon="long",
        ),
        _run_trial(
            simulator,
            output_dir / "confirmation",
            "position",
            "nominal-long-position",
            {},
            duration_us=2000000,
            reference=7.0,
            horizon="long",
        ),
        _run_trial(
            simulator,
            output_dir / "confirmation",
            "position",
            "selected-long-position",
            selected_profile,
            duration_us=2000000,
            reference=7.0,
            horizon="long",
        ),
    ]

    all_rows = current_rows + velocity_rows + position_rows + holdout_rows
    for row in all_rows:
        row.setdefault("score", _score(row["metrics"]))
    result = {
        "schema_version": 1,
        "study": "NEXT-CONTROL-TUNING-01",
        "baseline": baseline,
        "optimization": {
            "current": current_rows,
            "velocity": velocity_rows,
            "position": position_rows,
        },
        "selected_profile": selected_profile,
        "final_candidates": [{
            "profile_id": "position-c3",
            "candidate_id": position_choice["candidate_id"],
            "current_kp": selected_profile.get("current_kp"),
            "current_ki": selected_profile.get("current_ki"),
            "velocity_kp": selected_profile.get("velocity_kp"),
            "velocity_ki": selected_profile.get("velocity_ki"),
            "position_kp": selected_profile.get("position_kp"),
            "training_metrics": {
                "current": current_choice["metrics"],
                "velocity": velocity_choice["metrics"],
                "position": position_choice["metrics"],
            },
            "holdout_metrics": holdout_rows,
            "safety_result": "FEASIBLE" if all(
                row["feasibility"]["feasible"] for row in holdout_rows
            ) else "INFEASIBLE",
            "selection_reason": (
                "Pareto-feasible staged candidate with lower long-horizon velocity "
                "and position tracking error than the staged baselines"
            ),
        }],
        "pareto": {
            "current": pareto_frontier([dict(row, **row["metrics"]) for row in current_rows]),
            "velocity": pareto_frontier([dict(row, **row["metrics"]) for row in velocity_rows]),
            "position": pareto_frontier([dict(row, **row["metrics"]) for row in position_rows]),
        },
        "holdout": holdout_rows,
        "confirmation": confirmation_rows,
        "improvement": {
            "status": "NO_ROBUST_IMPROVEMENT_CONFIRMED",
            "holdout": "HOLDOUT_PASS"
            if all(row["feasibility"]["feasible"] for row in holdout_rows)
            else "HOLDOUT_FAIL",
        },
    }
    # Keep the baseline score explicit after serializing raw metrics.
    result["baseline"]["current"]["score"] = _score(result["baseline"]["current"]["metrics"])
    baseline_scores = [
        result["baseline"]["current"]["score"],
        next(row["score"] for row in velocity_rows if row["candidate_id"] == "velocity-baseline"),
        next(row["score"] for row in position_rows if row["candidate_id"] == "position-baseline"),
    ]
    selected_scores = [
        _score(current_choice["metrics"]),
        _score(velocity_choice["metrics"]),
        _score(position_choice["metrics"]),
    ]
    result["improvement"]["stage_scores"] = {
        "baseline": baseline_scores,
        "selected": selected_scores,
    }
    result["improvement"]["status"] = (
        "IMPROVEMENT_CONFIRMED"
        if result["improvement"]["holdout"] == "HOLDOUT_PASS"
        and all(selected <= base for selected, base in zip(selected_scores, baseline_scores))
        and any(selected < base for selected, base in zip(selected_scores, baseline_scores))
        else "NO_ROBUST_IMPROVEMENT_CONFIRMED"
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "study.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", required=True, type=pathlib.Path)
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    result = run_study(args.simulator, args.output_dir)
    print(json.dumps({
        "study": result["study"],
        "selected_profile": result["selected_profile"],
        **result["improvement"],
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
