/**
 * @file    traj_scurve.c
 * @brief   7-Segment Jerk-Limited S-Curve Trajectory Planner
 * @version v1.1.0
 * @date    2026.03.21
 *
 * Design notes
 * ------------
 * 1) Full 7-phase structure with optional degenerate phases:
 *      [1] +J  [2] +A  [3] -J  [4] cruise  [5] -J  [6] -D  [7] +J
 *    where +A is acceleration toward motion direction, -D is deceleration.
 *
 * 2) All planning is done in a positive scalar domain (distance >= 0), then
 *    mapped back to world sign at the final boundary-table write.
 *
 * 3) Short move handling uses a binary-search peak velocity solve, avoiding
 *    heuristic sqrt(distance * acc) estimates that are inaccurate with jerk
 *    limits and non-zero start/end velocities.
 *
 * 4) Runtime eval uses start-of-phase snapshots and local-time integration,
 *    guaranteeing C1/C2 continuity (pos/vel/acc) between phases.
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "traj_scurve.h"
#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private constants ---------------------------------------------------------------------------- */

#define _SC_EPS          (1e-6f)
#define _SC_SOLVE_ITERS  (40U)

/* Private helpers ------------------------------------------------------------------------------ */

static inline float _f_abs(float x)
{
    return fabsf(x);
}

static inline float _f_max(float a, float b)
{
    return (a > b) ? a : b;
}

/**
 * @brief  Integrate one constant-jerk phase.
 */
static void _integrate_const_jerk(float  p0,
                                  float  v0,
                                  float  a0,
                                  float  jerk,
                                  float  dt,
                                  float *p1,
                                  float *v1,
                                  float *a1)
{
    float dt2 = dt * dt;
    float dt3 = dt2 * dt;

    *a1 = a0 + jerk * dt;
    *v1 = v0 + a0 * dt + 0.5f * jerk * dt2;
    *p1 = p0 + v0 * dt + 0.5f * a0 * dt2 + (1.0f / 6.0f) * jerk * dt3;
}

/**
 * @brief  Build a 3-phase jerk-limited speed transition (no cruise).
 *
 * Transition is from v_start to v_end with bounded |acc| and |jerk|.
 *
 * @param  v_start     Start velocity (>= 0 in planning domain)
 * @param  v_end       End velocity   (>= 0 in planning domain)
 * @param  acc_limit   Positive acceleration magnitude limit
 * @param  jerk_limit  Positive jerk magnitude limit
 * @param  t1          [out] jerk-up/down duration (phase 1)
 * @param  t2          [out] constant-acc duration (phase 2)
 * @param  t3          [out] jerk-down/up duration (phase 3)
 * @param  dist        [out] transition distance (>= 0)
 */
static void _build_speed_transition(float  v_start,
                                    float  v_end,
                                    float  acc_limit,
                                    float  jerk_limit,
                                    float *t1,
                                    float *t2,
                                    float *t3,
                                    float *dist)
{
    float dv      = _f_abs(v_end - v_start);
    float j       = jerk_limit;
    float a_lim   = acc_limit;
    float t_jerk  = a_lim / j;
    float dv_tri  = a_lim * a_lim / j;
    float t_const = 0.0f;

    if (dv <= _SC_EPS) {
        *t1   = 0.0f;
        *t2   = 0.0f;
        *t3   = 0.0f;
        *dist = 0.0f;
        return;
    }

    if (dv < dv_tri) {
        t_jerk = sqrtf(dv / j);
        t_const = 0.0f;
    } else {
        t_const = (dv - dv_tri) / a_lim;
    }

    /* Integrate in local positive direction. */
    {
        float sign = (v_end >= v_start) ? 1.0f : -1.0f;

        float p = 0.0f;
        float v = v_start;
        float a = 0.0f;

        _integrate_const_jerk(p, v, a, sign * j, t_jerk, &p, &v, &a);
        _integrate_const_jerk(p, v, a, 0.0f, t_const, &p, &v, &a);
        _integrate_const_jerk(p, v, a, -sign * j, t_jerk, &p, &v, &a);

        *dist = p;
    }

    *t1 = t_jerk;
    *t2 = t_const;
    *t3 = t_jerk;
}

/**
 * @brief  Total distance for accelerate(v0->vp) + decelerate(vp->v1).
 */
static float _distance_for_peak_velocity(float v0,
                                         float v1,
                                         float vp,
                                         float a_max,
                                         float d_max,
                                         float j_max)
{
    float t1a, t2a, t3a, d_acc;
    float t1d, t2d, t3d, d_dec;

    _build_speed_transition(v0, vp, a_max, j_max, &t1a, &t2a, &t3a, &d_acc);
    _build_speed_transition(vp, v1, d_max, j_max, &t1d, &t2d, &t3d, &d_dec);

    return d_acc + d_dec;
}

/**
 * @brief  Build planner boundary table from phase durations/jerks.
 */
static void _build_boundaries(struct traj_scurve *traj,
                              const float        phase_dur[7],
                              const float        phase_jerk[7],
                              float              start_pos,
                              float              start_vel,
                              float              dir)
{
    float p = start_pos;
    float v = start_vel;
    float a = 0.0f;

    float t_sum = 0.0f;

    for (uint32_t i = 0U; i < 7U; i++) {
        traj->boundaries[i].pos  = p;
        traj->boundaries[i].vel  = v;
        traj->boundaries[i].acc  = a;
        traj->boundaries[i].jerk = phase_jerk[i] * dir;
        traj->boundaries[i].dur  = phase_dur[i];

        t_sum += phase_dur[i];
        traj->boundaries[i].t_end = t_sum;

        _integrate_const_jerk(p,
                              v,
                              a,
                              traj->boundaries[i].jerk,
                              phase_dur[i],
                              &p,
                              &v,
                              &a);
    }

    traj->boundaries[7].pos  = p;
    traj->boundaries[7].vel  = v;
    traj->boundaries[7].acc  = 0.0f;
    traj->boundaries[7].jerk = 0.0f;
    traj->boundaries[7].dur  = 0.0f;
    traj->boundaries[7].t_end = t_sum;

    traj->t_total   = t_sum;
    traj->t_current = 0.0f;
    traj->is_active = (t_sum > _SC_EPS);

    traj->step.pos  = start_pos;
    traj->step.vel  = start_vel;
    traj->step.acc  = 0.0f;
    traj->step.jerk = (traj->is_active) ? traj->boundaries[0].jerk : 0.0f;
}

/* Public functions ----------------------------------------------------------------------------- */

int traj_scurve_init(struct traj_scurve *traj)
{
    if (traj == NULL) {
        return -EINVAL;
    }

    memset(traj, 0, sizeof(struct traj_scurve));
    traj->is_active = false;
    return 0;
}

int traj_scurve_config_set(struct traj_scurve *traj, const struct traj_scurve_config *config)
{
    if (traj == NULL || config == NULL) {
        return -EINVAL;
    }

    if (config->max_vel <= _SC_EPS || config->max_acc <= _SC_EPS || config->max_dec <= _SC_EPS ||
        config->max_jerk <= _SC_EPS) {
        return -EINVAL;
    }

    traj->config = *config;
    return 0;
}

int traj_scurve_plan(struct traj_scurve *traj,
                     float               target_pos,
                     float               start_pos,
                     float               start_vel,
                     float               end_vel)
{
    if (traj == NULL) {
        return -EINVAL;
    }

    if (traj->config.max_vel <= _SC_EPS || traj->config.max_acc <= _SC_EPS ||
        traj->config.max_dec <= _SC_EPS || traj->config.max_jerk <= _SC_EPS) {
        return -EINVAL;
    }

    {
        float dx = target_pos - start_pos;

        if (_f_abs(dx) <= _SC_EPS && _f_abs(start_vel - end_vel) <= _SC_EPS) {
            memset(traj->boundaries, 0, sizeof(traj->boundaries));
            traj->boundaries[0].pos = start_pos;
            traj->boundaries[0].vel = start_vel;
            traj->boundaries[7].pos = target_pos;
            traj->boundaries[7].vel = end_vel;
            traj->t_total   = 0.0f;
            traj->t_current = 0.0f;
            traj->is_active = false;
            traj->step.pos  = target_pos;
            traj->step.vel  = end_vel;
            traj->step.acc  = 0.0f;
            traj->step.jerk = 0.0f;
            return 0;
        }

        /* Planning sign domain (positive motion direction). */
        {
            float dir = (dx >= 0.0f) ? 1.0f : -1.0f;

            float x0 = start_pos * dir;
            float xf = target_pos * dir;
            float v0 = start_vel * dir;
            float v1 = end_vel * dir;
            float ds = xf - x0;

            float v_max = traj->config.max_vel;
            float a_max = traj->config.max_acc;
            float d_max = traj->config.max_dec;
            float j_max = traj->config.max_jerk;

            float v_low;
            float v_high;
            float v_peak;

            float d_lim;
            float t1a, t2a, t3a, d_acc;
            float t1d, t2d, t3d, d_dec;

            float T[7] = {0};
            float J[7] = {0};

            v_low  = _f_max(_f_max(v0, v1), 0.0f);
            v_high = _f_max(v_low, v_max);

            d_lim = _distance_for_peak_velocity(v0, v1, v_high, a_max, d_max, j_max);

            if (d_lim <= ds + _SC_EPS) {
                /* Long move: reaches v_max and includes cruise. */
                v_peak = v_high;
            } else {
                /* Short move: solve v_peak by bisection, no cruise. */
                for (uint32_t k = 0U; k < _SC_SOLVE_ITERS; k++) {
                    float vm   = 0.5f * (v_low + v_high);
                    float d_vm = _distance_for_peak_velocity(v0, v1, vm, a_max, d_max, j_max);

                    if (d_vm > ds) {
                        v_high = vm;
                    } else {
                        v_low = vm;
                    }
                }
                v_peak = 0.5f * (v_low + v_high);
            }

            _build_speed_transition(v0, v_peak, a_max, j_max, &t1a, &t2a, &t3a, &d_acc);
            _build_speed_transition(v_peak, v1, d_max, j_max, &t1d, &t2d, &t3d, &d_dec);

            T[0] = t1a;
            T[1] = t2a;
            T[2] = t3a;
            T[3] = _f_max(0.0f, (ds - d_acc - d_dec) / _f_max(v_peak, _SC_EPS));
            T[4] = t1d;
            T[5] = t2d;
            T[6] = t3d;

            /* Jerk signs in +dir planning domain. */
            J[0] = +j_max;
            J[1] = 0.0f;
            J[2] = -j_max;
            J[3] = 0.0f;
            J[4] = -j_max;
            J[5] = 0.0f;
            J[6] = +j_max;

            _build_boundaries(traj, T, J, start_pos, start_vel, dir);

            /* Force exact final target to avoid tiny floating residuals. */
            traj->boundaries[7].pos = target_pos;
            traj->boundaries[7].vel = end_vel;
            traj->boundaries[7].acc = 0.0f;
            traj->boundaries[7].jerk = 0.0f;

            return 0;
        }
    }

    return 0;
}

int traj_scurve_eval(const struct traj_scurve *traj, float t, struct traj_scurve_step *out_step)
{
    if (traj == NULL || out_step == NULL) {
        return -EINVAL;
    }

    if (!traj->is_active || t <= 0.0f) {
        out_step->pos  = traj->boundaries[0].pos;
        out_step->vel  = traj->boundaries[0].vel;
        out_step->acc  = traj->boundaries[0].acc;
        out_step->jerk = traj->boundaries[0].jerk;
        return 0;
    }

    if (t >= traj->t_total) {
        out_step->pos  = traj->boundaries[7].pos;
        out_step->vel  = traj->boundaries[7].vel;
        out_step->acc  = 0.0f;
        out_step->jerk = 0.0f;
        return 0;
    }

    for (uint32_t i = 0U; i < 7U; i++) {
        if (t <= traj->boundaries[i].t_end) {
            float phase_start_t = traj->boundaries[i].t_end - traj->boundaries[i].dur;
            float t_local       = t - phase_start_t;

            _integrate_const_jerk(traj->boundaries[i].pos,
                                  traj->boundaries[i].vel,
                                  traj->boundaries[i].acc,
                                  traj->boundaries[i].jerk,
                                  t_local,
                                  &out_step->pos,
                                  &out_step->vel,
                                  &out_step->acc);
            out_step->jerk = traj->boundaries[i].jerk;
            return 0;
        }
    }

    /* Numerical fallback. */
    out_step->pos  = traj->boundaries[7].pos;
    out_step->vel  = traj->boundaries[7].vel;
    out_step->acc  = 0.0f;
    out_step->jerk = 0.0f;
    return 0;
}

int traj_scurve_step_update(struct traj_scurve *traj, float dt)
{
    if (traj == NULL) {
        return -EINVAL;
    }

    if (!traj->is_active) {
        return 0;
    }

    if (dt <= 0.0f) {
        return -EINVAL;
    }

    traj->t_current += dt;

    if (traj->t_current >= traj->t_total) {
        traj->step.pos  = traj->boundaries[7].pos;
        traj->step.vel  = traj->boundaries[7].vel;
        traj->step.acc  = 0.0f;
        traj->step.jerk = 0.0f;
        traj->is_active = false;
        return 0;
    }

    return traj_scurve_eval(traj, traj->t_current, &traj->step);
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
