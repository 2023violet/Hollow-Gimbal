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
 * Admit the optional correction at initialization and apply it before direction.
 * Use the same corrected observation for phase, velocity, and continuous position.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_encoder.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Macros and Constants ------------------------------------------------- */

#define FOC_ENCODER_TWO_PI         6.28318530717958647692f
#define FOC_ENCODER_PI             3.14159265358979323846f
#define FOC_ENCODER_HALF_RANGE     UINT32_C(0x80000000)
#define FOC_ENCODER_MAX_LUT_LENGTH 1024U

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

static uint32_t foc_encoder_apply_lut(const struct foc_encoder_context *context, uint32_t raw_code)
{
    if (context->lut_offsets == NULL) {
        return raw_code;
    }
    const int32_t cpr = (int32_t)context->config.counts_per_revolution;
    const uint32_t length = context->lut_length;
    const uint32_t scaled = raw_code * length;
    const uint32_t index = scaled / (uint32_t)cpr;
    const uint32_t next = index + 1U == length ? 0U : index + 1U;
    const int32_t fraction = (int32_t)(scaled - index * (uint32_t)cpr);
    const int32_t left = context->lut_offsets[index];
    const int32_t diff = (int32_t)context->lut_offsets[next] - left;
    /* The admitted slope bounds diff * fraction. Round ties towards +infinity. */
    int32_t numerator = diff * fraction + cpr / 2;
    if (numerator < 0) {
        numerator -= cpr - 1;
    }
    const int32_t corrected = (int32_t)raw_code + left + numerator / cpr;
    if (corrected < 0) {
        return (uint32_t)(corrected + cpr);
    }
    return (uint32_t)(corrected >= cpr ? corrected - cpr : corrected);
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
        feedback->single_turn_rad =
            foc_encoder_single_turn_rad(context, context->previous_corrected_code);
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

static bool foc_encoder_short_arc(int32_t counts, uint32_t previous, uint32_t current, int32_t *arc)
{
    int32_t delta = (int32_t)current - (int32_t)previous;
    const int32_t half = counts / 2;
    if ((counts % 2) == 0 && (delta == half || delta == -half)) {
        return false;
    }
    if (delta > half) {
        delta -= counts;
    } else if (delta < -half) {
        delta += counts;
    }
    *arc = delta;
    return true;
}

static bool foc_encoder_delta_counts(
    const struct foc_encoder_context *context,
    uint32_t raw_code,
    uint32_t corrected_code,
    int64_t *delta_counts
)
{
    const int32_t counts = (int32_t)context->config.counts_per_revolution;
    int32_t corrected;
    if (!foc_encoder_short_arc(
            counts,
            context->previous_corrected_code,
            corrected_code,
            &corrected
        )) {
        return false;
    }
    if (context->lut_varies) {
        /* Each endpoint can have 0.75 count of raw quantization error
         * (slope <= 1.5), then 0.5 count of output rounding. Keep 2.5
         * counts clear of the ambiguous half-turn for a varying table. */
        const int32_t twice_limit = counts - 5;
        int32_t raw;
        if (2 * corrected >= twice_limit || 2 * corrected <= -twice_limit ||
            !foc_encoder_short_arc(counts, context->previous_code, raw_code, &raw) ||
            (raw > 0 && corrected < 0) || (raw < 0 && corrected > 0)) {
            return false;
        }
    }
    *delta_counts = (int64_t)corrected * (int64_t)context->config.direction;
    return true;
}

static int foc_encoder_validate_lut(
    const struct foc_encoder_lut *lut,
    uint32_t counts_per_revolution,
    uint32_t encoder_identity,
    bool *varies
)
{
    if (lut == NULL || lut->offset_counts == NULL) {
        return FOC_ENCODER_STATUS_ERR_NULL;
    }
    if (encoder_identity == 0U || lut->encoder_identity != encoder_identity ||
        lut->counts_per_revolution != counts_per_revolution) {
        return FOC_ENCODER_STATUS_ERR_LUT_IDENTITY;
    }
    const uint32_t length = lut->length;
    if (counts_per_revolution < 2U || counts_per_revolution > UINT32_C(65536) || length < 2U ||
        length > FOC_ENCODER_MAX_LUT_LENGTH || length > counts_per_revolution) {
        return FOC_ENCODER_STATUS_ERR_CONFIG;
    }
    const int32_t amplitude = (int32_t)(counts_per_revolution / 8U);
    const int32_t max_step = (int32_t)(counts_per_revolution / (2U * length));
    bool changing = false;
    int32_t previous = lut->offset_counts[length - 1U];
    for (uint32_t i = 0U; i < length; ++i) {
        const int32_t offset = lut->offset_counts[i];
        if (offset < -amplitude || offset > amplitude) {
            return FOC_ENCODER_STATUS_ERR_LUT_RANGE;
        }
        const int32_t step = offset - previous;
        if (step < -max_step || step > max_step) {
            return FOC_ENCODER_STATUS_ERR_LUT_SLOPE;
        }
        changing |= step != 0;
        previous = offset;
    }
    *varies = changing;
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

int foc_encoder_lut_validate(
    const struct foc_encoder_lut *lut,
    uint32_t counts_per_revolution,
    uint32_t encoder_identity
)
{
    bool varies;
    return foc_encoder_validate_lut(lut, counts_per_revolution, encoder_identity, &varies);
}

int foc_encoder_init_with_lut(
    struct foc_encoder_context *context,
    const struct foc_encoder_config *config,
    uint32_t encoder_identity,
    const struct foc_encoder_lut *lut
)
{
    if (context == NULL || config == NULL) {
        return FOC_ENCODER_STATUS_ERR_NULL;
    }
    bool varies;
    const int validation =
        foc_encoder_validate_lut(lut, config->counts_per_revolution, encoder_identity, &varies);
    if (validation != FOC_ENCODER_STATUS_OK) {
        return validation;
    }
    const int status = foc_encoder_init(context, config);
    if (status == FOC_ENCODER_STATUS_OK) {
        context->lut_varies = varies;
        context->lut_offsets = lut->offset_counts;
        context->lut_length = lut->length;
    }
    return status;
}

int foc_encoder_correct_code(
    const struct foc_encoder_context *context,
    uint32_t raw_code,
    uint32_t *corrected_code
)
{
    if (context == NULL || corrected_code == NULL) {
        return FOC_ENCODER_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_ENCODER_STATUS_ERR_NOT_INITIALIZED;
    }
    if (raw_code >= context->config.counts_per_revolution) {
        return FOC_ENCODER_STATUS_ERR_SAMPLE;
    }
    *corrected_code = foc_encoder_apply_lut(context, raw_code);
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
        .lut_varies = context->lut_varies,
        .lut_offsets = context->lut_offsets,
        .lut_length = context->lut_length,
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

    const uint32_t corrected = foc_encoder_apply_lut(context, raw_code);
    const uint32_t directed = foc_encoder_directed_code(context, corrected);
    if (!context->previous_seen) {
        context->previous_seen = true;
        context->previous_us = captured_us;
        context->previous_code = raw_code;
        context->previous_corrected_code = corrected;
        context->position_counts = (int64_t)directed;
        feedback->corrected_code = corrected;
        feedback->mechanical_phase_count = directed;
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
    if (!foc_encoder_delta_counts(context, raw_code, corrected, &delta_counts)) {
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
    float tracker_offset = 0.0f;
    float tracker_velocity = raw_velocity;
    if (context->tracker_seen) {
        const float predicted =
            context->tracker_local_offset_rad + context->tracker_velocity_rad_s * dt_s;
        const float residual = delta_rad - predicted;
        /* Re-anchor the estimate to the current measurement. */
        tracker_offset = (context->config.tracker_alpha - 1.0f) * residual;
        tracker_velocity =
            context->tracker_velocity_rad_s + context->config.tracker_beta / dt_s * residual;
    }
    if (!foc_encoder_float_finite(position_rad) || !foc_encoder_float_finite(filtered_velocity) ||
        !foc_encoder_float_finite(tracker_offset) || !foc_encoder_float_finite(tracker_velocity)) {
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
    context->previous_corrected_code = corrected;
    context->position_counts = next_position;
    context->lpf_velocity_rad_s = filtered_velocity;
    context->tracker_local_offset_rad = tracker_offset;
    context->tracker_velocity_rad_s = tracker_velocity;
    context->velocity_seen = true;
    context->tracker_seen = true;

    feedback->single_turn_rad = foc_encoder_code_to_rad(context, directed);
    feedback->mechanical_rad = position_rad;
    feedback->corrected_code = corrected;
    feedback->mechanical_phase_count = directed;
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
