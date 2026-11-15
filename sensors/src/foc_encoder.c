/**
 * @file    foc_encoder.c
 * @brief   Float32 magnetic encoder angle, velocity, filtering, and quality.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Implementation role
 * -------------------
 * Own the portable absolute-code interpretation and bounded feedback state.
 * Transport owners provide complete frames and source capture timestamps.
 *
 * Includes
 * --------
 * The implementation uses only the public encoder contract and the standard
 * C math/integer facilities. It does not access MCU, SPI, Runtime, or Safety.
 *
 * Private Helpers
 * ---------------
 * Helpers validate policy once, classify timestamp/code failures, and keep
 * rejected observations from changing the accepted position or filter state.
 *
 * Public Functions
 * ----------------
 * foc_encoder_init(), foc_encoder_reset(), and foc_encoder_update() are the
 * complete module boundary.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_encoder.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Macros and Constants ------------------------------------------------- */

#define FOC_ENCODER_TWO_PI     6.28318530717958647692f
#define FOC_ENCODER_PI         3.14159265358979323846f
#define FOC_ENCODER_HALF_RANGE UINT32_C(0x80000000)

/* Private Helpers -------------------------------------------------------------- */

static bool foc_encoder_float_finite(float value)
{
    return isfinite(value) != 0;
}

static void foc_encoder_invalidate_count(struct foc_encoder_context *context)
{
    if (context->invalid_count < UINT32_MAX) {
        ++context->invalid_count;
    }
}

static uint32_t
foc_encoder_directed_code(const struct foc_encoder_context *context, uint32_t raw_code)
{
    if (context->config.direction == FOC_ENCODER_DIRECTION_NORMAL) {
        return raw_code;
    }
    return raw_code == 0U ? 0U : context->config.counts_per_revolution - raw_code;
}

static float foc_encoder_code_to_rad(const struct foc_encoder_context *context, int64_t counts)
{
    const float scale = FOC_ENCODER_TWO_PI / (float)context->config.counts_per_revolution;
    return (float)counts * scale;
}

static float
foc_encoder_single_turn_rad(const struct foc_encoder_context *context, uint32_t raw_code)
{
    return foc_encoder_code_to_rad(context, (int64_t)foc_encoder_directed_code(context, raw_code));
}

static void foc_encoder_feedback_base(
    const struct foc_encoder_context *context,
    uint32_t captured_us,
    uint32_t raw_code,
    struct foc_encoder_feedback *feedback
)
{
    *feedback = (struct foc_encoder_feedback){
        .captured_us = captured_us,
        .raw_code = raw_code,
        .quality = FOC_ENCODER_QUALITY_UNINITIALIZED,
        .continuity_lost = context->continuity_lost,
        .invalid_count = context->invalid_count,
    };
    if (context->previous_seen) {
        feedback->mechanical_rad = foc_encoder_code_to_rad(context, context->position_counts);
        feedback->single_turn_rad = foc_encoder_single_turn_rad(context, context->previous_code);
        feedback->filtered_velocity_rad_s = context->lpf_velocity_rad_s;
        feedback->tracked_velocity_rad_s = context->tracker_velocity_rad_s;
    }
}

static int foc_encoder_status_for_quality(enum foc_encoder_quality quality)
{
    switch (quality) {
    case FOC_ENCODER_QUALITY_TIMEOUT:
    case FOC_ENCODER_QUALITY_TRANSPORT_ERROR:
        return FOC_ENCODER_STATUS_ERR_TRANSPORT;
    case FOC_ENCODER_QUALITY_DUPLICATE:
    case FOC_ENCODER_QUALITY_TIME_ORDER_ERROR:
    case FOC_ENCODER_QUALITY_INVALID_INTERVAL:
        return FOC_ENCODER_STATUS_ERR_ORDER;
    case FOC_ENCODER_QUALITY_DISCONTINUITY:
        return FOC_ENCODER_STATUS_ERR_DISCONTINUITY;
    case FOC_ENCODER_QUALITY_SPEED_OUT_OF_RANGE:
        return FOC_ENCODER_STATUS_ERR_SPEED;
    case FOC_ENCODER_QUALITY_INVALID_SAMPLE:
    case FOC_ENCODER_QUALITY_NUMERIC_ERROR:
        return FOC_ENCODER_STATUS_ERR_SAMPLE;
    default:
        return FOC_ENCODER_STATUS_OK;
    }
}

static int foc_encoder_reject(
    struct foc_encoder_context *context,
    uint32_t captured_us,
    uint32_t raw_code,
    enum foc_encoder_quality quality,
    struct foc_encoder_feedback *feedback
)
{
    foc_encoder_invalidate_count(context);
    foc_encoder_feedback_base(context, captured_us, raw_code, feedback);
    feedback->quality = quality;
    feedback->continuity_lost = context->continuity_lost;
    feedback->invalid_count = context->invalid_count;
    return foc_encoder_status_for_quality(quality);
}

static int foc_encoder_config_validate(const struct foc_encoder_config *config)
{
    if (config == NULL || config->counts_per_revolution < 2U ||
        config->counts_per_revolution > UINT32_C(65536) ||
        (config->direction != FOC_ENCODER_DIRECTION_NORMAL &&
         config->direction != FOC_ENCODER_DIRECTION_REVERSED) ||
        !foc_encoder_float_finite(config->max_speed_rad_s) || config->max_speed_rad_s <= 0.0f ||
        config->min_dt_us == 0U || config->max_dt_us < config->min_dt_us ||
        config->max_dt_us >= FOC_ENCODER_HALF_RANGE ||
        !foc_encoder_float_finite(config->speed_lpf_cutoff_hz) ||
        config->speed_lpf_cutoff_hz <= 0.0f || !foc_encoder_float_finite(config->tracker_alpha) ||
        config->tracker_alpha <= 0.0f || config->tracker_alpha > 1.0f ||
        !foc_encoder_float_finite(config->tracker_beta) || config->tracker_beta <= 0.0f ||
        config->tracker_beta > 2.0f) {
        return FOC_ENCODER_STATUS_ERR_CONFIG;
    }
    const float tau = 1.0f / (FOC_ENCODER_TWO_PI * config->speed_lpf_cutoff_hz);
    if (!foc_encoder_float_finite(tau) || tau <= FLT_MIN) {
        return FOC_ENCODER_STATUS_ERR_CONFIG;
    }
    return FOC_ENCODER_STATUS_OK;
}

static int foc_encoder_delta_counts(
    const struct foc_encoder_context *context,
    uint32_t raw_code,
    int64_t *delta_counts
)
{
    const int64_t counts = (int64_t)context->config.counts_per_revolution;
    int64_t delta = (int64_t)raw_code - (int64_t)context->previous_code;
    const int64_t half = counts / 2;

    if ((counts % 2) == 0 && (delta == half || delta == -half)) {
        return FOC_ENCODER_STATUS_ERR_DISCONTINUITY;
    }
    if (delta > half) {
        delta -= counts;
    } else if (delta < -half) {
        delta += counts;
    }
    *delta_counts = delta * (int64_t)context->config.direction;
    return FOC_ENCODER_STATUS_OK;
}

/* Public Functions ------------------------------------------------------------- */

int foc_encoder_init(struct foc_encoder_context *context, const struct foc_encoder_config *config)
{
    if (context == NULL || config == NULL) {
        return FOC_ENCODER_STATUS_ERR_NULL;
    }
    if (foc_encoder_config_validate(config) != FOC_ENCODER_STATUS_OK) {
        return FOC_ENCODER_STATUS_ERR_CONFIG;
    }
    *context = (struct foc_encoder_context){
        .config = *config,
        .lpf_tau_s = 1.0f / (FOC_ENCODER_TWO_PI * config->speed_lpf_cutoff_hz),
        .initialized = true,
    };
    return FOC_ENCODER_STATUS_OK;
}

int foc_encoder_reset(struct foc_encoder_context *context)
{
    if (context == NULL) {
        return FOC_ENCODER_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_ENCODER_STATUS_ERR_NOT_INITIALIZED;
    }
    const struct foc_encoder_config config = context->config;
    const float tau = context->lpf_tau_s;
    *context = (struct foc_encoder_context){
        .config = config,
        .lpf_tau_s = tau,
        .initialized = true,
    };
    return FOC_ENCODER_STATUS_OK;
}

int foc_encoder_update(
    struct foc_encoder_context *context,
    uint32_t captured_us,
    uint32_t raw_code,
    enum foc_encoder_transport transport,
    struct foc_encoder_feedback *feedback
)
{
    if (feedback == NULL) {
        return FOC_ENCODER_STATUS_ERR_NULL;
    }
    *feedback = (struct foc_encoder_feedback){0};
    if (context == NULL) {
        return FOC_ENCODER_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        feedback->quality = FOC_ENCODER_QUALITY_INVALID_CONFIG;
        return FOC_ENCODER_STATUS_ERR_NOT_INITIALIZED;
    }
    foc_encoder_feedback_base(context, captured_us, raw_code, feedback);
    if (transport != FOC_ENCODER_TRANSPORT_OK) {
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            transport == FOC_ENCODER_TRANSPORT_TIMEOUT ? FOC_ENCODER_QUALITY_TIMEOUT
                                                       : FOC_ENCODER_QUALITY_TRANSPORT_ERROR,
            feedback
        );
    }
    if (raw_code >= context->config.counts_per_revolution) {
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_INVALID_SAMPLE,
            feedback
        );
    }
    if (context->continuity_lost) {
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_DISCONTINUITY,
            feedback
        );
    }

    const uint32_t directed = foc_encoder_directed_code(context, raw_code);
    if (!context->previous_seen) {
        context->previous_seen = true;
        context->previous_us = captured_us;
        context->previous_code = raw_code;
        context->position_counts = (int64_t)directed;
        feedback->single_turn_rad = foc_encoder_code_to_rad(context, directed);
        feedback->mechanical_rad = foc_encoder_code_to_rad(context, context->position_counts);
        feedback->quality = FOC_ENCODER_QUALITY_UNINITIALIZED;
        feedback->invalid_count = context->invalid_count;
        return FOC_ENCODER_STATUS_OK;
    }

    const uint32_t elapsed_us = captured_us - context->previous_us;
    if (elapsed_us == 0U) {
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_DUPLICATE,
            feedback
        );
    }
    if (elapsed_us >= FOC_ENCODER_HALF_RANGE) {
        context->continuity_lost = true;
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_TIME_ORDER_ERROR,
            feedback
        );
    }
    if (elapsed_us < context->config.min_dt_us) {
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_INVALID_INTERVAL,
            feedback
        );
    }
    if (elapsed_us > context->config.max_dt_us) {
        context->continuity_lost = true;
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_DISCONTINUITY,
            feedback
        );
    }

    const float dt_s = (float)elapsed_us * 1.0e-6f;
    if (!foc_encoder_float_finite(dt_s) ||
        context->config.max_speed_rad_s * dt_s >= FOC_ENCODER_PI) {
        context->continuity_lost = true;
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_DISCONTINUITY,
            feedback
        );
    }

    int64_t delta_counts;
    if (foc_encoder_delta_counts(context, raw_code, &delta_counts) != FOC_ENCODER_STATUS_OK) {
        context->continuity_lost = true;
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_DISCONTINUITY,
            feedback
        );
    }
    const float delta_rad = foc_encoder_code_to_rad(context, delta_counts);
    const float raw_velocity = delta_rad / dt_s;
    if (!foc_encoder_float_finite(delta_rad) || !foc_encoder_float_finite(raw_velocity)) {
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_NUMERIC_ERROR,
            feedback
        );
    }
    if (fabsf(raw_velocity) > context->config.max_speed_rad_s) {
        context->continuity_lost = true;
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_SPEED_OUT_OF_RANGE,
            feedback
        );
    }

    const int64_t previous_position = context->position_counts;
    if ((delta_counts > 0 && previous_position > INT64_MAX - delta_counts) ||
        (delta_counts < 0 && previous_position < INT64_MIN - delta_counts)) {
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_NUMERIC_ERROR,
            feedback
        );
    }
    const int64_t next_position = previous_position + delta_counts;
    const float position_rad = foc_encoder_code_to_rad(context, next_position);
    const float alpha = dt_s / (context->lpf_tau_s + dt_s);
    const float filtered_velocity = context->velocity_seen
        ? context->lpf_velocity_rad_s + alpha * (raw_velocity - context->lpf_velocity_rad_s)
        : raw_velocity;
    float tracker_position;
    float tracker_velocity;
    if (!context->tracker_seen) {
        tracker_position = position_rad;
        tracker_velocity = raw_velocity;
    } else {
        const float predicted =
            context->tracker_position_rad + context->tracker_velocity_rad_s * dt_s;
        const float residual = position_rad - predicted;
        tracker_position = predicted + context->config.tracker_alpha * residual;
        tracker_velocity =
            context->tracker_velocity_rad_s + context->config.tracker_beta / dt_s * residual;
    }
    if (!foc_encoder_float_finite(position_rad) || !foc_encoder_float_finite(filtered_velocity) ||
        !foc_encoder_float_finite(tracker_position) ||
        !foc_encoder_float_finite(tracker_velocity)) {
        return foc_encoder_reject(
            context,
            captured_us,
            raw_code,
            FOC_ENCODER_QUALITY_NUMERIC_ERROR,
            feedback
        );
    }

    context->previous_us = captured_us;
    context->previous_code = raw_code;
    context->position_counts = next_position;
    context->lpf_velocity_rad_s = filtered_velocity;
    context->tracker_position_rad = tracker_position;
    context->tracker_velocity_rad_s = tracker_velocity;
    context->velocity_seen = true;
    context->tracker_seen = true;

    feedback->single_turn_rad = foc_encoder_code_to_rad(context, directed);
    feedback->mechanical_rad = position_rad;
    feedback->delta_rad = delta_rad;
    feedback->dt_s = dt_s;
    feedback->raw_velocity_rad_s = raw_velocity;
    feedback->filtered_velocity_rad_s = filtered_velocity;
    feedback->tracked_velocity_rad_s = tracker_velocity;
    feedback->speed_valid = true;
    feedback->valid = true;
    feedback->quality = FOC_ENCODER_QUALITY_VALID;
    feedback->invalid_count = context->invalid_count;
    return FOC_ENCODER_STATUS_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
