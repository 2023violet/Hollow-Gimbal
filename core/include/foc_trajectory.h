/**
 * @file    foc_trajectory.h
 * @brief   Small Float32 velocity and point-to-point trajectory planner.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Design overview
 * ---------------
 * This module generates bounded mechanical velocity references for the existing
 * velocity PI loop. It owns no position sensor, clock, scheduler, Safety state,
 * or output path. A caller may copy a context for a trial and commit it only
 * after the downstream control update is accepted.
 *
 * Usage example
 * -------------
 * Initialize a context, copy it for an H1 trial, step the copy with the actual
 * source interval, and commit the copy only after H0 accepts the derived target.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns one context per motion session. Configuration is copied at
 * initialization and is read-only afterwards. The context contains only the
 * planned reference and lifecycle state; sensor and output ownership remain
 * with Runtime and Pipeline.
 *
 * Failure / recovery
 * ------------------
 * Invalid inputs clear the output and leave the context unchanged. Reset starts
 * a new reference from a caller-confirmed velocity. The position planner uses
 * continuous mechanical coordinates and never wraps a target to one turn.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Status returned by a trajectory lifecycle or planning operation. */
enum foc_trajectory_status {
    /** Operation completed successfully. */
    FOC_TRAJECTORY_STATUS_OK = 0,
    /** A required pointer was NULL. */
    FOC_TRAJECTORY_STATUS_ERR_NULL = -1,
    /** Settings or an initial velocity violated the configured bounds. */
    FOC_TRAJECTORY_STATUS_ERR_CONFIG = -2,
    /** Context was not initialized or was invalidated. */
    FOC_TRAJECTORY_STATUS_ERR_STATE = -3,
    /** dt was zero, negative, non-finite, or over the configured bound. */
    FOC_TRAJECTORY_STATUS_ERR_INVALID_DT = -4,
    /** A non-finite target or computed reference was rejected. */
    FOC_TRAJECTORY_STATUS_ERR_NONFINITE = -5,
};

/** Lifecycle state of one caller-owned planner instance. */
enum foc_trajectory_state {
    /** Storage has not been initialized. */
    FOC_TRAJECTORY_STATE_UNINITIALIZED = 0,
    /** Initialized and waiting for its first valid reference. */
    FOC_TRAJECTORY_STATE_READY,
    /** A bounded reference is being generated. */
    FOC_TRAJECTORY_STATE_ACTIVE,
    /** A caller-visible invalidation requires an explicit reset. */
    FOC_TRAJECTORY_STATE_INVALID,
};

/** Bounded mechanical reference policy. */
struct foc_trajectory_config {
    /** False leaves the caller's legacy reference path untouched. */
    bool enabled;
    /** Symmetric maximum planned mechanical speed in rad/s. */
    float max_velocity_rad_s;
    /** Symmetric maximum planned acceleration in rad/s². */
    float max_accel_rad_s2;
    /** Maximum accepted actual step interval in seconds. */
    float max_dt_s;
    /** Position band in which the point-to-point target requests zero velocity. */
    float position_tolerance_rad;
};

/** Instance-owned planned reference state. */
struct foc_trajectory_context {
    /** Validated immutable settings copied at initialization. */
    struct foc_trajectory_config config;
    /** Current planned mechanical velocity in rad/s. */
    float planned_velocity_rad_s;
    /** Lifecycle state. */
    enum foc_trajectory_state state;
    /** Set after successful initialization. */
    bool initialized;
};

/** One planned velocity output for the existing velocity PI loop. */
struct foc_trajectory_output {
    /** Bounded mechanical velocity reference in rad/s. */
    float velocity_ref_rad_s;
    /** True when the planner generated a bounded reference. */
    bool active;
};

/* Public Functions ------------------------------------------------------------- */

/** Validate settings and initialize a planner with zero planned velocity. */
int foc_trajectory_init(struct foc_trajectory_context *context,
                        const struct foc_trajectory_config *config);

/** Start a new session from a caller-confirmed finite planned velocity. */
int foc_trajectory_reset(struct foc_trajectory_context *context, float initial_velocity_rad_s);

/** Mark an initialized planner invalid without changing its stored velocity. */
int foc_trajectory_invalidate(struct foc_trajectory_context *context);

/** Apply speed and acceleration limits to a mechanical velocity target. */
int foc_trajectory_velocity_step(struct foc_trajectory_context *context,
                                 float target_velocity_rad_s,
                                 float dt_s,
                                 struct foc_trajectory_output *output);

/** Generate a bounded point-to-point velocity reference from continuous position. */
int foc_trajectory_position_step(struct foc_trajectory_context *context,
                                 float target_position_rad,
                                 float measured_position_rad,
                                 float dt_s,
                                 struct foc_trajectory_output *output);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
