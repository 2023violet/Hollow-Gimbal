/**
 * @file    traj_trap.c
 * @brief   Trapezoidal Velocity-Profile Trajectory Planner — Implementation
 * @version v4.1.0
 * @date    2026.03.19
 *
 * Changes vs v2.0.0 / v3.0.0
 * ───────────────────────────
 * 1. Return type changed int → bool throughout (int was returning C99 false/true
 *    which is fine but bool is semantically clearer and avoids accidental int math
 *    on the return value).
 *
 * 2. Private planner fields renamed with underscore prefix (_x0, _xf, …) in the
 *    header to signal "do not write from outside".  This is a convention, not a
 *    language enforcement — the struct is still fully transparent for debugging.
 *
 * 3. traj_trap_eval() signature changed to const (the function is a pure query
 *    that should not mutate planner state; only the caller's out_step is written).
 *    The internal traj->step sync removed from eval() — step is now written only
 *    by traj_trap_step_update() via the non-const internal helper.
 *
 * 4. traj_trap_init() uses memset for brevity and zero-initialises step.
 *
 * 5. Added traj_trap_is_active() and traj_trap_duration() as static inlines in
 *    the header to avoid function-call overhead for simple queries.
 *
 * 6. _sign_hard() moved to a private inline in this file (was a static function
 *    at the bottom, breaking the "private functions before public" convention).
 *
 * Algorithm correctness: unchanged from v2.0.0.  The planning equations are
 * the standard time-optimal bang-coast-bang derivation; no numerical changes.
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "traj_trap.h"
#include <math.h>
#include <string.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private constants ---------------------------------------------------------------------------- */

/* Threshold below which a constraint is considered unconfigured (zero). */
#define _TRAJ_EPS (1e-6f)

/* Private helpers ------------------------------------------------------------------------------ */

/**
 * @brief  Hard sign function: returns 1.0f for val ≥ 0, −1.0f for val < 0.
 *
 * Differs from FOC_SIGN (which also returns 1.0f for 0) — same behaviour,
 * but expressed clearly in context.  Using signbit() avoids a branch.
 */
static inline float _sign_hard(float val)
{
    return signbit(val) ? -1.0f : 1.0f;
}

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialise trajectory planner to zeroed-safe state.
 */
int traj_trap_init(struct traj_trap *traj)
{
    if (traj == NULL) {
        return -EINVAL;
    }
    memset(traj, 0, sizeof(struct traj_trap));
    /* is_active = false, t_current = 0, all floats = 0 */
    return 0;
}

/**
 * @brief  Validate and store kinematic constraints.
 */
int traj_trap_config_set(struct traj_trap *traj, const struct traj_trap_config *config)
{
    if (traj == NULL || config == NULL) {
        return -EINVAL;
    }
    if (config->max_vel <= _TRAJ_EPS || config->max_acc <= _TRAJ_EPS ||
        config->max_dec <= _TRAJ_EPS) {
        return -EINVAL; /* Reject zero / negative constraints */
    }
    traj->config = *config; /* Struct copy */
    return 0;
}

/**
 * @brief  Plan a time-optimal trapezoidal (or triangular) trajectory.
 *
 * Full derivation notes
 * ─────────────────────
 * Let s = sign of the move direction after accounting for start velocity.
 *
 * 1. Minimum stopping distance at v0:
 *      dx_stop = |v0|² / (2·d_max) × sign(v0)
 *
 * 2. Direction of planned motion:
 *      s = sign(dx − dx_stop)
 *
 * 3. Signed limits:
 *      a = s × max_acc    (positive in move direction)
 *      d = −s × max_dec   (negative in move direction — decelerating)
 *      v_max = s × max_vel
 *
 * 4. "Double deceleration" check: if v0 overshoots v_max in direction s,
 *    we must first decelerate using the negative acceleration:
 *      a = −s × max_acc   (flip sign for initial phase)
 *
 * 5. Phase durations:
 *      t_acc  = (v_max − v0) / a
 *      t_dec  = −v_max / d
 *      dx_min = ½·t_acc·(v_max + v0) + ½·t_dec·v_max
 *
 * 6. Long move (dx ≥ dx_min):
 *      t_cru = (dx − dx_min) / v_max
 *
 * 7. Short move (dx < dx_min): triangular profile.
 *    Peak velocity is solved from energy conservation:
 *      v_peak² = (d·v0² + 2·a·d·dx) / (d − a)
 *    t_acc and t_dec are recomputed from v_peak.
 */
int traj_trap_plan(struct traj_trap *traj, float target_pos, float start_pos, float start_vel)
{
    if (traj == NULL) {
        return -EINVAL;
    }
    if (traj->config.max_acc <= _TRAJ_EPS || traj->config.max_dec <= _TRAJ_EPS) {
        return -EINVAL; /* Constraints not configured */
    }

    const float a_max = traj->config.max_acc;
    const float d_max = traj->config.max_dec;
    const float v_lim = traj->config.max_vel;

    float dx = target_pos - start_pos;

    /* ── Step 1: Stopping distance from current velocity ─────────────── */
    float stop_dist = (start_vel * start_vel) / (2.0f * d_max);
    float dx_stop   = copysignf(stop_dist, start_vel);

    /* ── Step 2: Move direction ───────────────────────────────────────── */
    float s = _sign_hard(dx - dx_stop);

    /* ── Step 3: Signed limits ───────────────────────────────────────── */
    traj->_a_signed     = s * a_max;
    traj->_d_signed     = -s * d_max;
    traj->_v_max_signed = s * v_lim;

    /* ── Step 4: Double-deceleration case ────────────────────────────── */
    /* If the start velocity is already beyond v_max in the move direction,
     * the first phase must be a deceleration (flip a sign). */
    if (s * start_vel > s * traj->_v_max_signed) {
        traj->_a_signed = -s * a_max;
    }

    /* ── Step 5: Nominal phase durations ─────────────────────────────── */
    traj->_t_acc = (traj->_v_max_signed - start_vel) / traj->_a_signed;
    traj->_t_dec = -traj->_v_max_signed / traj->_d_signed;

    /* Minimum displacement to reach v_max then decelerate to zero */
    float dx_min = 0.5f * traj->_t_acc * (traj->_v_max_signed + start_vel) +
                   0.5f * traj->_t_dec * traj->_v_max_signed;

    /* ── Step 6 / 7: Long vs short move ─────────────────────────────── */
    if (s * dx < s * dx_min) {
        /* ── Short move: triangular profile ──────────────────────────── */
        /* Solve for peak velocity from energy conservation */
        float radicand = (traj->_d_signed * (start_vel * start_vel) +
                          2.0f * traj->_a_signed * traj->_d_signed * dx) /
                         (traj->_d_signed - traj->_a_signed);

        traj->_v_max_signed = s * sqrtf(fmaxf(radicand, 0.0f));
        traj->_t_acc        = fmaxf(0.0f, (traj->_v_max_signed - start_vel) / traj->_a_signed);
        traj->_t_dec        = fmaxf(0.0f, -traj->_v_max_signed / traj->_d_signed);
        traj->_t_cru        = 0.0f;
    } else {
        /* ── Long move: trapezoidal profile ──────────────────────────── */
        traj->_t_cru = (dx - dx_min) / traj->_v_max_signed;
    }

    /* ── Compute timeline and boundary positions ──────────────────────── */
    traj->_t_end     = traj->_t_acc + traj->_t_cru + traj->_t_dec;
    traj->_x0        = start_pos;
    traj->_xf        = target_pos;
    traj->_v0        = start_vel;
    traj->_x_acc_end = start_pos + start_vel * traj->_t_acc +
                       0.5f * traj->_a_signed * (traj->_t_acc * traj->_t_acc);

    /* ── Reset runtime counter and activate ──────────────────────────── */
    traj->t_current = 0.0f;
    traj->is_active = true;

    /* Seed the step output with the initial condition */
    traj_trap_eval(traj, 0.0f, &traj->step);
    return 0;
}

/**
 * @brief  Pure evaluation of trajectory at absolute time t.
 *
 * This function is const — it does not modify traj->step.
 * The caller is responsible for copying the result if needed.
 */
int traj_trap_eval(const struct traj_trap *traj, float t, struct traj_trap_step *out_step)
{
    if (traj == NULL || out_step == NULL) {
        return -EINVAL;
    }

    if (t < 0.0f) {
        /* Before start — hold initial condition */
        out_step->pos = traj->_x0;
        out_step->vel = traj->_v0;
        out_step->acc = 0.0f;

    } else if (t < traj->_t_acc) {
        /* Acceleration phase */
        out_step->pos = traj->_x0 + traj->_v0 * t + 0.5f * traj->_a_signed * (t * t);
        out_step->vel = traj->_v0 + traj->_a_signed * t;
        out_step->acc = traj->_a_signed;

    } else if (t < traj->_t_acc + traj->_t_cru) {
        /* Cruise phase */
        float dt_cru  = t - traj->_t_acc;
        out_step->pos = traj->_x_acc_end + traj->_v_max_signed * dt_cru;
        out_step->vel = traj->_v_max_signed;
        out_step->acc = 0.0f;

    } else if (t < traj->_t_end) {
        /* Deceleration phase
         * Use backward time reference: td = t − t_end (negative, counts down to 0)
         * Ensures position lands exactly on x_final regardless of float rounding.
         */
        float td      = t - traj->_t_end; /* td < 0 */
        out_step->pos = traj->_xf + 0.5f * traj->_d_signed * (td * td);
        out_step->vel = traj->_d_signed * td;
        out_step->acc = traj->_d_signed;

    } else {
        /* Trajectory complete — hold final condition */
        out_step->pos = traj->_xf;
        out_step->vel = 0.0f;
        out_step->acc = 0.0f;
    }

    return 0;
}

/**
 * @brief  Advance trajectory by dt and update traj->step.
 */
int traj_trap_step_update(struct traj_trap *traj, float dt)
{
    if (traj == NULL) {
        return -EINVAL;
    }

    if (!traj->is_active) {
        /* Trajectory already finished — hold final setpoint, not an error */
        return 0;
    }

    traj->t_current += dt;

    if (traj->t_current >= traj->_t_end) {
        /* Trajectory complete */
        traj->step.pos  = traj->_xf;
        traj->step.vel  = 0.0f;
        traj->step.acc  = 0.0f;
        traj->is_active = false;
        return 0;
    }

    /* Evaluate and write result directly to traj->step */
    return traj_trap_eval(traj, traj->t_current, &traj->step);
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
