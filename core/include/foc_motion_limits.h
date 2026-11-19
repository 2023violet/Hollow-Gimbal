/**
 * @file    foc_motion_limits.h
 * @brief   Software motion and effort admission in continuous shaft coordinates.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Check absolute position targets and reject outward reference or q-current
 * requests at a boundary. The caller establishes the shared shaft frame.
 *
 * Usage example
 * -------------
 * Validate settings at initialization. Check an incoming position target,
 * then evaluate the effective reference and combined q-current request.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Stateless. The caller owns measurement quality, frame binding, and output
 * admission. Compare targets and measurements in the same continuous radians.
 *
 * Failure / recovery
 * ------------------
 * A rejection leaves inputs unchanged. A target within the interval and a
 * request directed inward can be admitted after the shaft moves outside it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Inclusive position interval and inward distance of each direction guard. */
struct foc_motion_limits_config {
    float position_min_rad;
    float position_max_rad;
    float tolerance_rad;
    /** Nonzero expected identity of the calibrated position and sign. */
    uint32_t mechanical_frame_id;
    bool enabled;
};

enum foc_motion_rejection {
    FOC_MOTION_REJECT_NONE = 0,
    FOC_MOTION_REJECT_CONFIG,
    FOC_MOTION_REJECT_UNSUPPORTED_MODE,
    FOC_MOTION_REJECT_POSITION,
    FOC_MOTION_REJECT_DIRECTION,
    FOC_MOTION_REJECT_FEEDBACK,
    FOC_MOTION_REJECT_FRAME,
    FOC_MOTION_REJECT_FOLLOWING,
};

/** Reference and combined effort in one confirmed shaft sign convention. */
struct foc_motion_limits_request {
    float position_ref_rad;
    float velocity_ref_rad_s;
    float requested_velocity_rad_s;
    float iq_ref_a;
    bool has_position;
};

/* Public Functions ------------------------------------------------------------- */

/** Validate a nonoverlapping finite interval when enabled. */
bool foc_motion_limits_validate(const struct foc_motion_limits_config *config);

/** Check an absolute position target before admission. */
enum foc_motion_rejection foc_motion_limits_reference(const struct foc_motion_limits_config *config,
                                                      float position_rad);

/**
 * Check the request against the caller's fresh, frame-matched measurement.
 * Positive and negative q current represent signed effort. Inertia, load and
 * actuator capability determine the resulting physical motion.
 */
enum foc_motion_rejection foc_motion_limits_check(const struct foc_motion_limits_config *config,
                                                  float measured_rad,
                                                  const struct foc_motion_limits_request *request);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
