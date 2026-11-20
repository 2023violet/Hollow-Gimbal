/**
 * @file    foc_virtual_cogging.c
 * @brief   Acquire Virtual Motor shaft reaction and validate periodic compensation.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * Equal and opposite fixture speeds reverse viscous/odd drag. Their mean
 * preserves load and cogging. Same-phase samples a turn apart identify the
 * affine load slope without absorbing the periodic component into that slope.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_cogging.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Constants ------------------------------------------------------------ */

enum {
    FOC_COGGING_CAPTURE_POINTS = 2 * FOC_RUNTIME_COGGING_POINTS
};

static const double foc_cogging_two_pi = 6.28318530717958647692;
static const double foc_cogging_pitch = 6.28318530717958647692 / FOC_RUNTIME_COGGING_POINTS;
static const double foc_cogging_holdout_offset = 0.38196601125;
static const double foc_cogging_step_s = 0.00005;
static const double foc_cogging_min_signal_nm = 1.0e-7;

/* Private Helpers -------------------------------------------------------------- */

static int foc_cogging_move(
    struct foc_virtual_motor *plant,
    double destination,
    const struct foc_virtual_cogging_scan_config *config,
    double *elapsed_s
)
{
    struct foc_virtual_motor_observation state;
    if (foc_virtual_motor_observe(plant, &state) != 0) {
        return -1;
    }
    const double speed = config->speed_rad_s;
    const double duration = fabs(destination - state.mechanical_position_rad) / speed;
    if (duration == 0.0) {
        return 0;
    }
    const double velocity = copysign(speed, destination - state.mechanical_position_rad);
    if (duration > 30.0 || *elapsed_s + duration > 120.0 ||
        foc_virtual_motor_set_fixture_speed(plant, velocity) != 0) {
        return -1;
    }
    const unsigned int steps = (unsigned int)ceil(duration / foc_cogging_step_s);
    const double dt = duration / (double)steps;
    for (unsigned int step = 0U; step < steps; ++step) {
        if ((config->permit != NULL && !config->permit(config->permit_argument)) ||
            foc_virtual_motor_step(plant, 0.0, 0.0, dt) != 0) {
            return -1;
        }
    }
    *elapsed_s += duration;
    return 0;
}

static int foc_cogging_capture(
    const struct foc_virtual_motor *motor,
    double first_position,
    const struct foc_virtual_cogging_scan_config *config,
    double *elapsed_s,
    bool reverse,
    double samples[FOC_COGGING_CAPTURE_POINTS]
)
{
    struct foc_virtual_motor plant = *motor;
    const double run_in = first_position +
        (reverse ? ((double)FOC_COGGING_CAPTURE_POINTS - 0.5) : -0.5) * foc_cogging_pitch;
    if (foc_cogging_move(&plant, run_in, config, elapsed_s) != 0) {
        return -1;
    }
    for (unsigned int sample = 0U; sample < FOC_COGGING_CAPTURE_POINTS; ++sample) {
        const unsigned int i = reverse ? FOC_COGGING_CAPTURE_POINTS - 1U - sample : sample;
        const double target = first_position + (double)i * foc_cogging_pitch;
        struct foc_virtual_motor_observation observed;
        if (foc_cogging_move(&plant, target, config, elapsed_s) != 0 ||
            foc_virtual_motor_observe(&plant, &observed) != 0 || !observed.fixture_active ||
            observed.mechanical_velocity_rad_s !=
                (reverse ? -config->speed_rad_s : config->speed_rad_s) ||
            fabs(observed.mechanical_position_rad - target) > 1.0e-7) {
            return -1;
        }
        /* Both sources act in the same measured shaft frame. Their sum is the
         * net torque needed to maintain test speed, even with electrical drag. */
        samples[i] += 0.5 * (observed.fixture_torque_nm + observed.electromagnetic_torque_nm);
    }
    return 0;
}

static int foc_cogging_measure(
    const struct foc_virtual_motor *motor,
    double first_position,
    const struct foc_virtual_cogging_scan_config *config,
    double *elapsed_s,
    double *load_bias,
    double *load_slope,
    double periodic[FOC_RUNTIME_COGGING_POINTS]
)
{
    double midpoint[FOC_COGGING_CAPTURE_POINTS] = {0};
    if (foc_cogging_capture(motor, first_position, config, elapsed_s, false, midpoint) != 0 ||
        foc_cogging_capture(motor, first_position, config, elapsed_s, true, midpoint) != 0) {
        return -1;
    }
    double delta = 0.0;
    for (unsigned int i = 0U; i < FOC_RUNTIME_COGGING_POINTS; ++i) {
        delta += midpoint[i + FOC_RUNTIME_COGGING_POINTS] - midpoint[i];
    }
    *load_slope = delta / (foc_cogging_two_pi * FOC_RUNTIME_COGGING_POINTS);
    double intercept = 0.0;
    for (unsigned int i = 0U; i < FOC_RUNTIME_COGGING_POINTS; ++i) {
        const double position = first_position + (double)i * foc_cogging_pitch;
        periodic[i] = 0.5 * (midpoint[i] + midpoint[i + FOC_RUNTIME_COGGING_POINTS]) -
            *load_slope * (position + 0.5 * foc_cogging_two_pi);
        intercept += periodic[i];
    }
    *load_bias = intercept / FOC_RUNTIME_COGGING_POINTS;
    for (unsigned int i = 0U; i < FOC_RUNTIME_COGGING_POINTS; ++i) {
        periodic[i] -= *load_bias;
    }
    return isfinite(*load_slope) && isfinite(*load_bias) ? 0 : -1;
}

/* Public Functions ------------------------------------------------------------- */

int foc_virtual_cogging_calibrate(
    const struct foc_virtual_motor *motor,
    const struct foc_virtual_cogging_scan_config *config,
    struct foc_virtual_cogging_result *result
)
{
    if (result == NULL) {
        return FOC_VIRTUAL_COGGING_ERR_INPUT;
    }
    *result = (struct foc_virtual_cogging_result){0};
    struct foc_virtual_motor_observation initial;
    if (config == NULL || config->motor_id == 0U || config->mechanical_frame_id == 0U ||
        config->training_capture_id == 0U || config->holdout_capture_id == 0U ||
        config->training_capture_id == config->holdout_capture_id ||
        !isfinite(config->speed_rad_s) || config->speed_rad_s < 0.5 || config->speed_rad_s > 20.0 ||
        !isfinite(config->max_holdout_rms_nm) || config->max_holdout_rms_nm <= 0.0 ||
        foc_virtual_motor_observe(motor, &initial) != 0 || initial.fixture_active ||
        fabs(initial.mechanical_position_rad) > 4096.0) {
        return FOC_VIRTUAL_COGGING_ERR_INPUT;
    }
    const double origin =
        floor(initial.mechanical_position_rad / foc_cogging_two_pi) * foc_cogging_two_pi;
    double training[FOC_RUNTIME_COGGING_POINTS];
    double holdout[FOC_RUNTIME_COGGING_POINTS];
    double holdout_bias;
    double holdout_slope;
    double elapsed_s = 0.0;
    if (foc_cogging_measure(
            motor,
            origin,
            config,
            &elapsed_s,
            &result->training_load_bias_nm,
            &result->training_load_slope_nm_per_rad,
            training
        ) != 0 ||
        foc_cogging_measure(
            motor,
            origin + foc_cogging_holdout_offset * foc_cogging_pitch,
            config,
            &elapsed_s,
            &holdout_bias,
            &holdout_slope,
            holdout
        ) != 0) {
        return FOC_VIRTUAL_COGGING_ERR_CAPTURE;
    }
    result->lut.motor_id = config->motor_id;
    result->lut.mechanical_frame_id = config->mechanical_frame_id;
    result->lut.training_capture_id = config->training_capture_id;
    result->lut.holdout_capture_id = config->holdout_capture_id;
    for (unsigned int i = 0U; i < FOC_RUNTIME_COGGING_POINTS; ++i) {
        if (!isfinite(training[i]) || fabs(training[i]) > FLT_MAX || !isfinite(holdout[i])) {
            return FOC_VIRTUAL_COGGING_ERR_CAPTURE;
        }
        result->lut.torque_nm[i] = (float)training[i];
    }
    double baseline_sq = 0.0;
    double compensated_sq = 0.0;
    for (unsigned int i = 0U; i < FOC_RUNTIME_COGGING_POINTS; ++i) {
        const unsigned int next = (i + 1U) % FOC_RUNTIME_COGGING_POINTS;
        const double estimate = (1.0 - foc_cogging_holdout_offset) * result->lut.torque_nm[i] +
            foc_cogging_holdout_offset * result->lut.torque_nm[next];
        const double error = holdout[i] - estimate;
        compensated_sq += error * error;
        baseline_sq += holdout[i] * holdout[i];
    }
    result->holdout_baseline_rms_nm = sqrt(baseline_sq / FOC_RUNTIME_COGGING_POINTS);
    result->holdout_compensated_rms_nm = sqrt(compensated_sq / FOC_RUNTIME_COGGING_POINTS);
    result->lut.qualified = isfinite(result->holdout_baseline_rms_nm) &&
        result->holdout_baseline_rms_nm > foc_cogging_min_signal_nm &&
        result->holdout_compensated_rms_nm < 0.5 * result->holdout_baseline_rms_nm &&
        result->holdout_compensated_rms_nm <= config->max_holdout_rms_nm;
    return result->lut.qualified ? FOC_VIRTUAL_COGGING_OK : FOC_VIRTUAL_COGGING_ERR_HOLDOUT;
}

/* ---------------------------------------- end of file ----------------------------------------- */
