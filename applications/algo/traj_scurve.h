/**
 * @file traj_scurve.h
 * @brief  7-Segment Jerk-Limited S-Curve Trajectory Planner
 * @version v1.1.0
 * @date 2026.03.21
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief  Planner limits (all positive magnitudes).
 */
struct traj_scurve_config {
    float max_vel;  /* rad/s   > 0 */
    float max_acc;  /* rad/s^2 > 0 */
    float max_dec;  /* rad/s^2 > 0 */
    float max_jerk; /* rad/s^3 > 0 */
};

/**
 * @brief  Trajectory state at a given time.
 */
struct traj_scurve_step {
    float pos;  /* rad     */
    float vel;  /* rad/s   */
    float acc;  /* rad/s^2 */
    float jerk; /* rad/s^3 */
};

/**
 * @brief  Start-of-phase boundary snapshot.
 *
 * Layout convention:
 *   boundaries[0..6] -> phase 1..7 starts
 *   boundaries[7]    -> final state at t_total
 */
struct traj_scurve_boundary {
    float t_end; /* Absolute end time of this phase */
    float pos;   /* Position at phase start         */
    float vel;   /* Velocity at phase start         */
    float acc;   /* Acceleration at phase start     */
    float jerk;  /* Constant jerk in this phase     */
    float dur;   /* Phase duration                  */
};

/**
 * @brief  7-segment S-curve planner instance.
 */
struct traj_scurve {
    struct traj_scurve_config config;

    struct traj_scurve_boundary boundaries[8];
    struct traj_scurve_step     step;

    float t_total;   /* total trajectory time */
    float t_current; /* current runtime time  */
    bool  is_active; /* trajectory running    */
};

/* Public functions ----------------------------------------------------------------------------- */

int traj_scurve_init(struct traj_scurve *traj);
int traj_scurve_config_set(struct traj_scurve *traj, const struct traj_scurve_config *config);

int traj_scurve_plan(struct traj_scurve *traj,
                     float               target_pos,
                     float               start_pos,
                     float               start_vel,
                     float               end_vel);

int traj_scurve_eval(const struct traj_scurve *traj, float t, struct traj_scurve_step *out_step);
int traj_scurve_step_update(struct traj_scurve *traj, float dt);

/* Public inline helpers ------------------------------------------------------------------------ */

static inline bool traj_scurve_is_active(const struct traj_scurve *traj)
{
    return (traj != NULL) && traj->is_active;
}

static inline float traj_scurve_duration(const struct traj_scurve *traj)
{
    return (traj != NULL) ? traj->t_total : 0.0f;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
