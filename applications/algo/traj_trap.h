/**
 * @file    traj_trap.h
 * @brief   Velocity-Profile Trajectory Planner (Trapezoidal, Triangular, and S-Curve)
 * @version v4.1.0
 * @date    2026.03.19
 *
 * Algorithm
 * ─────────
 * Given a start state (x0, v0) and a target position (x_f), plans the
 * time-optimal trajectory subject to velocity (v_max), acceleration (a_max),
 * deceleration (d_max), and jerk (j_max) constraints. Three profile types are
 * supported:
 *
 *   Trapezoidal (long move)      Triangular (short move)      S-Curve (smooth move)
 *   ──────────────────────       ────────────────────────     ────────────────────────
 *   vel ▲                        vel ▲                        vel ▲
 *       │  ╭──────╮                  │  ╭╮                        │  ╭╮    ╭╮
 *       │ ╱        ╲                 │ ╱  ╲                       │ ╱  ╲  ╱  ╲
 *       └──────────────► t           └──────► t                   └──────────────► t
 *       accel  cruise  decel         accel decel                 jerk accel jerk decel
 *
 * The planner handles:
 *   · Non-zero start velocity (mid-move re-plan)
 *   · Signed motion (forward and reverse)
 *   · Asymmetric accel/decel (max_acc ≠ max_dec)
 *   · "Double-deceleration" case (v0 > v_max in the move direction)
 *   · S-curve jerk-limited motion for smoother transitions
 *
 * Units:  rad, rad/s, rad/s², rad/s³  (consistent with the Themis internal rad domain)
 *
 * Thread safety:
 *   traj_trap_plan() is called from the RTOS task (1 kHz).
 *   traj_trap_step_update() is also called from the RTOS task.
 *   No ISR access — no locking required.
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* include -------------------------------------------------------------------------------------- */
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief  Kinematic constraints for the trajectory planner.
 *
 * All three values must be strictly positive.  They can be changed between
 * moves by calling traj_trap_config_set(); a running trajectory is not
 * immediately affected — changes take effect on the next traj_trap_plan().
 */
struct traj_trap_config {
    float max_vel; /* Maximum cruise velocity (rad/s).       Must be > 0. */
    float max_acc; /* Maximum acceleration rate (rad/s²).    Must be > 0. */
    float max_dec; /* Maximum deceleration rate (rad/s²).    Must be > 0. */
};

/**
 * @brief  One trajectory sample: position, velocity, and acceleration commands.
 *
 * Consumed by foc_process_input_task() at 1 kHz to update the FOC setpoints.
 * The velocity field is used as a feed-forward term in the velocity PID.
 * The acceleration field is available for inertia-based current feed-forward
 * (divide by Kt and multiply by Js to get Iq_ff).
 */
struct traj_trap_step {
    float pos; /* Position command (rad)                           */
    float vel; /* Velocity command / feed-forward (rad/s)         */
    float acc; /* Acceleration command / feed-forward (rad/s²)    */
};

/**
 * @brief  Trapezoidal trajectory planner state.
 *
 * Members beginning with an underscore are computed by traj_trap_plan() and
 * read by traj_trap_eval().  Do not modify them directly.
 */
struct traj_trap {
    /* Configuration — written by traj_trap_config_set() */
    struct traj_trap_config config;

    /* Planned profile parameters — written by traj_trap_plan() */
    float _x0;           /* Start position (rad)                          */
    float _xf;           /* End position (rad)                            */
    float _v0;           /* Start velocity (rad/s)                        */
    float _a_signed;     /* Signed acceleration (may be negative in DD case) */
    float _v_max_signed; /* Signed cruise velocity                        */
    float _d_signed;     /* Signed deceleration (negative of move direction) */
    float _t_acc;        /* Duration of acceleration phase (s)            */
    float _t_cru;        /* Duration of cruise phase (s)                  */
    float _t_dec;        /* Duration of deceleration phase (s)            */
    float _t_end;        /* Total trajectory duration (s)                 */
    float _x_acc_end;    /* Position at the end of acceleration phase     */

    /* Runtime state — written by traj_trap_step_update() */
    float t_current; /* Elapsed time since plan() was called (s)      */
    bool  is_active; /* true while trajectory is in progress           */

    /* Current output sample — written by traj_trap_eval() */
    struct traj_trap_step step;
};

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialise the trajectory planner to a safe default state.
 *
 * Must be called before any other traj_trap function.
 * Config values are set to 0 — call traj_trap_config_set() before planning.
 *
 * @param  traj  Planner instance.
 * @return true on success, false if traj is NULL.
 */
int traj_trap_init(struct traj_trap *traj);

/**
 * @brief  Set or update kinematic constraints.
 *
 * All three values must be strictly positive (> 1e-6).
 * If a trajectory is active, the new constraints take effect on the next
 * traj_trap_plan() call only.
 *
 * @param  traj    Planner instance.
 * @param  config  New constraints.
 * @return true on success, false if any constraint is non-positive or NULL.
 */
int traj_trap_config_set(struct traj_trap *traj, const struct traj_trap_config *config);

/**
 * @brief  Plan a new trajectory from (start_pos, start_vel) to target_pos.
 *
 * Re-entrant: safe to call while a previous trajectory is still active.
 * The old trajectory is discarded and the new one starts immediately from
 * (start_pos, start_vel).  For a smooth in-motion replanning, pass the
 * current traj->step.pos and traj->step.vel as start arguments.
 *
 * @param  traj        Planner instance.
 * @param  target_pos  Destination position (rad).
 * @param  start_pos   Current position (rad).  May be mid-move.
 * @param  start_vel   Current velocity (rad/s).  May be non-zero.
 * @return true on success, false if constraints are not configured.
 */
int traj_trap_plan(struct traj_trap *traj, float target_pos, float start_pos, float start_vel);

/**
 * @brief  Evaluate the trajectory at a given absolute time t.
 *
 * Does not advance the internal timer.  Useful for look-ahead or
 * offline analysis.
 *
 * @param  traj      Planner instance (must have been planned).
 * @param  t         Absolute time relative to plan start (s).
 * @param  out_step  Output — filled with pos/vel/acc at time t.
 * @return true on success, false if traj or out_step is NULL.
 */
int traj_trap_eval(const struct traj_trap *traj, float t, struct traj_trap_step *out_step);

/**
 * @brief  Advance the trajectory by dt seconds and update traj->step.
 *
 * Called from the RTOS motor task every 1 ms.  Returns true while the
 * trajectory is still running.  Once the trajectory completes, is_active
 * is set to false and the function returns true (not an error — it simply
 * holds the final setpoint).
 *
 * @param  traj  Planner instance.
 * @param  dt    Time step in seconds (typically TRAJ_TASK_TS = 1e-3).
 * @return true on success, false if traj is NULL.
 */
int traj_trap_step_update(struct traj_trap *traj, float dt);

/**
 * @brief  Query whether the trajectory is still running.
 *
 * @return true if in progress, false if completed or not yet planned.
 */
static inline bool traj_trap_is_active(const struct traj_trap *traj)
{
    return (traj != NULL) && traj->is_active;
}

/**
 * @brief  Query total trajectory duration (s).  Valid after traj_trap_plan().
 */
static inline float traj_trap_duration(const struct traj_trap *traj)
{
    return (traj != NULL) ? traj->_t_end : 0.0f;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
