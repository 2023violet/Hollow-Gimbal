"""Unit tests for the deterministic Host control-tuning research selector."""

import unittest

import control_tuning as tuning


class ControlTuningTests(unittest.TestCase):
    def test_hard_constraints_reject_unsafe_candidate(self):
        metrics = {
            "status": "INTEGRATION_PASS",
            "final_state": 3,
            "update_accepted": 1,
            "saturation_duration_us": 0,
            "final_error": 0.01,
            "settling_time_us": 1000,
        }
        unsafe = dict(metrics, final_state=5)
        self.assertTrue(tuning.hard_constraints(metrics).feasible)
        self.assertFalse(tuning.hard_constraints(unsafe).feasible)

    def test_pareto_frontier_keeps_tradeoff_candidates(self):
        candidates = [
            {"candidate_id": "fast", "settling_time_us": 100, "running_tail_rmse": 0.20},
            {"candidate_id": "quiet", "settling_time_us": 200, "running_tail_rmse": 0.10},
            {"candidate_id": "dominated", "settling_time_us": 300, "running_tail_rmse": 0.30},
        ]
        frontier = tuning.pareto_frontier(candidates)
        self.assertEqual({row["candidate_id"] for row in frontier}, {"fast", "quiet"})

    def test_simulator_args_carry_research_profile(self):
        args = tuning.simulator_args(
            profile_id="CQ-TUNE-C01",
            current_kp=1.2,
            current_ki=600.0,
            velocity_kp=0.2,
            velocity_ki=1.5,
            position_kp=4.5,
            current_limit=2.2,
            voltage_limit=2.0,
            vbus=10.0,
            reference=2.0,
        )
        self.assertIn("--profile-id", args)
        self.assertIn("CQ-TUNE-C01", args)
        self.assertIn("--current-kp", args)
        self.assertIn("1.2", args)
        self.assertIn("--reference", args)
        self.assertIn("2.0", args)


if __name__ == "__main__":
    unittest.main()
