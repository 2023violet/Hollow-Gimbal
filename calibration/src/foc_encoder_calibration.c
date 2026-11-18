/**
 * @file    foc_encoder_calibration.c
 * @brief   Bounded periodic LUT generation from independent angular observations.
 * @version v0.1.0
 * @date    2026.11.27
 *
 * Implementation role
 * -------------------
 * Keep per-segment moments, join neighboring linear estimates at shared
 * knots, then qualify the frozen integer table using the Sensor API.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_encoder_calibration.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

#define FOC_ENCODER_CAL_TWO_PI      6.28318530717958647692
#define FOC_ENCODER_CAL_HALF_RANGE  UINT32_C(0x80000000)
#define FOC_ENCODER_CAL_COVERED     15U
#define FOC_ENCODER_CAL_MAX_SAMPLES UINT16_MAX

/* Private Helpers -------------------------------------------------------------- */

static bool foc_encoder_cal_limit(double value)
{
    return isfinite(value) && value > 0.0 && value <= FOC_ENCODER_CAL_TWO_PI / 8.0;
}

static bool foc_encoder_cal_config_valid(const struct foc_encoder_cal_config *config)
{
    struct foc_encoder_context probe;
    return config->encoder_identity != 0U && config->reference_identity != 0U &&
        config->encoder_identity != config->reference_identity &&
        (config->source == FOC_ENCODER_CAL_SOURCE_VIRTUAL ||
         config->source == FOC_ENCODER_CAL_SOURCE_EXTERNAL) &&
        config->nodes >= 4U && config->nodes <= FOC_ENCODER_CAL_MAX_NODES &&
        config->encoder.counts_per_revolution >= 4U * config->nodes &&
        config->min_samples_per_segment >= 4U &&
        config->min_samples_per_segment <= FOC_ENCODER_CAL_MAX_SAMPLES / config->nodes &&
        isfinite(config->reference_zero_rad) && config->reference_zero_rad >= 0.0 &&
        config->reference_zero_rad < FOC_ENCODER_CAL_TWO_PI &&
        foc_encoder_cal_limit(config->max_zero_bias_rad) &&
        foc_encoder_cal_limit(config->max_training_segment_rms_rad) &&
        foc_encoder_cal_limit(config->max_holdout_rms_rad) &&
        foc_encoder_cal_limit(config->max_holdout_peak_rad) &&
        config->max_holdout_rms_rad <= config->max_holdout_peak_rad &&
        foc_encoder_init(&probe, &config->encoder) == FOC_ENCODER_STATUS_OK;
}

static int foc_encoder_cal_reject(
    struct foc_encoder_cal_context *context,
    enum foc_calibration_failure failure,
    int status
)
{
    context->result.state = FOC_CALIBRATION_STATE_INVALID;
    context->result.failure = failure;
    memset(context->result.offsets, 0, sizeof context->result.offsets);
    return status;
}

static bool foc_encoder_cal_setup_equal(
    const struct foc_encoder_cal_setup *a,
    const struct foc_encoder_cal_setup *b
)
{
    return a->encoder_identity == b->encoder_identity &&
        a->reference_identity == b->reference_identity &&
        a->counts_per_revolution == b->counts_per_revolution && a->direction == b->direction &&
        a->source == b->source;
}

static bool foc_encoder_cal_newer(uint32_t time, uint32_t previous)
{
    const uint32_t elapsed = time - previous;
    return elapsed != 0U && elapsed < FOC_ENCODER_CAL_HALF_RANGE;
}

static double foc_encoder_cal_wrap_error(double difference, double counts)
{
    if (difference >= counts * 0.5) {
        return difference - counts;
    }
    return difference < -counts * 0.5 ? difference + counts : difference;
}

static bool foc_encoder_cal_covered(
    const struct foc_encoder_cal_config *config,
    const struct foc_encoder_cal_capture *capture
)
{
    for (uint32_t i = 0U; i < config->nodes; ++i) {
        if (capture->segment_samples[i] < config->min_samples_per_segment ||
            capture->coverage[i] != FOC_ENCODER_CAL_COVERED) {
            return false;
        }
    }
    return true;
}

static struct foc_encoder_lut foc_encoder_cal_descriptor(
    const struct foc_encoder_cal_result *result
)
{
    return (struct foc_encoder_lut){
        .offset_counts = result->offsets,
        .length = result->nodes,
        .counts_per_revolution = result->setup.counts_per_revolution,
        .encoder_identity = result->setup.encoder_identity,
    };
}

static bool
foc_encoder_cal_estimate_knots(const struct foc_encoder_cal_context *context, double *knots)
{
    for (uint32_t i = 0U; i < context->config.nodes; ++i) {
        const struct foc_encoder_cal_segment *segment = &context->segments[i];
        const double n = context->result.training.segment_samples[i];
        const double mean_x = segment->fraction / n;
        const double mean_y = segment->correction / n;
        const double variance = segment->fraction2 - mean_x * segment->fraction;
        /* Count and quarter occupancy alone do not constrain a dense sample
         * cluster. Require at least 0.01 normalized variance in each segment. */
        if (variance < 0.01 * n) {
            return false;
        }
        const double slope =
            (segment->fraction_correction - mean_x * segment->correction) / variance;
        const double left = mean_y - slope * mean_x;
        const uint32_t next = (i + 1U) % context->config.nodes;
        knots[i] += 0.5 * left;
        knots[next] += 0.5 * (left + slope);
    }
    return true;
}

static void foc_encoder_cal_training_residual(struct foc_encoder_cal_context *context)
{
    struct foc_encoder_cal_result *result = &context->result;
    double total = 0.0;
    double worst_mean_square = 0.0;
    for (uint32_t i = 0U; i < result->nodes; ++i) {
        const struct foc_encoder_cal_segment *segment = &context->segments[i];
        const double n = result->training.segment_samples[i];
        const double a = result->offsets[i];
        const double b = result->offsets[(i + 1U) % result->nodes] - a;
        const double y_a = 2.0 * a * segment->correction;
        const double y_b = 2.0 * b * segment->fraction_correction;
        const double a2 = n * a * a;
        const double ab = 2.0 * a * b * segment->fraction;
        const double b2 = b * b * segment->fraction2;
        const double sum_square = segment->correction2 - y_a - y_b + a2 + ab + b2;
        /* Moment accumulation and cancellation limit the precision of this
         * estimate. Charge a scale-dependent roundoff allowance to the gate. */
        const double magnitude = segment->correction2 + fabs(y_a) + fabs(y_b) + a2 + fabs(ab) + b2;
        const double allowance = 8.0 * (n + 8.0) * DBL_EPSILON * magnitude;
        const double upper = (sum_square > 0.0 ? sum_square : 0.0) + allowance;
        total += upper;
        const double mean_square = upper / n;
        if (mean_square > worst_mean_square) {
            worst_mean_square = mean_square;
        }
    }
    result->training_rms_rad = context->radians_per_count * sqrt(total / result->training.samples);
    result->training_max_segment_rms_rad = context->radians_per_count * sqrt(worst_mean_square);
}

/* Public Functions ------------------------------------------------------------- */

int foc_encoder_cal_init(
    struct foc_encoder_cal_context *context,
    const struct foc_encoder_cal_config *config,
    uint32_t training_capture_id
)
{
    if (context == NULL || config == NULL) {
        return FOC_ENCODER_CAL_ERR_NULL;
    }
    if (training_capture_id == 0U || !foc_encoder_cal_config_valid(config)) {
        return FOC_ENCODER_CAL_ERR_CONFIG;
    }
    const struct foc_encoder_cal_config copy = *config;
    memset(context, 0, sizeof *context);
    context->config = copy;
    context->result.state = FOC_CALIBRATION_STATE_COLLECTING;
    context->result.setup = (struct foc_encoder_cal_setup){
        .encoder_identity = copy.encoder_identity,
        .reference_identity = copy.reference_identity,
        .counts_per_revolution = copy.encoder.counts_per_revolution,
        .direction = copy.encoder.direction,
        .source = copy.source,
    };
    context->result.nodes = copy.nodes;
    context->result.reference_zero_rad = copy.reference_zero_rad;
    context->result.training.capture_id = training_capture_id;
    context->radians_per_count = FOC_ENCODER_CAL_TWO_PI / copy.encoder.counts_per_revolution;
    return FOC_ENCODER_CAL_OK;
}

int foc_encoder_cal_reset(struct foc_encoder_cal_context *context, uint32_t training_capture_id)
{
    if (context == NULL) {
        return FOC_ENCODER_CAL_ERR_NULL;
    }
    if (context->result.state == FOC_CALIBRATION_STATE_UNINITIALIZED) {
        return FOC_ENCODER_CAL_ERR_STATE;
    }
    return foc_encoder_cal_init(context, &context->config, training_capture_id);
}

int foc_encoder_cal_push(
    struct foc_encoder_cal_context *context,
    const struct foc_encoder_cal_sample *sample
)
{
    if (context == NULL || sample == NULL) {
        return FOC_ENCODER_CAL_ERR_NULL;
    }
    struct foc_encoder_cal_result *result = &context->result;
    struct foc_encoder_cal_capture *capture;
    const bool training = result->state == FOC_CALIBRATION_STATE_COLLECTING;
    if (training) {
        capture = &result->training;
    } else if (
        result->state == FOC_CALIBRATION_STATE_CANDIDATE && result->holdout.capture_id != 0U
    ) {
        capture = &result->holdout;
    } else {
        return FOC_ENCODER_CAL_ERR_STATE;
    }
    if (sample->capture_id != capture->capture_id ||
        !foc_encoder_cal_setup_equal(&sample->setup, &result->setup)) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
            FOC_ENCODER_CAL_ERR_SAMPLE
        );
    }
    const uint32_t cpr = result->setup.counts_per_revolution;
    if (!sample->encoder_valid || !sample->reference_valid || sample->raw_code >= cpr ||
        !isfinite(sample->reference_rad) || sample->reference_rad < 0.0 ||
        sample->reference_rad >= FOC_ENCODER_CAL_TWO_PI) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
            FOC_ENCODER_CAL_ERR_SAMPLE
        );
    }
    if (!sample->reference_confirmed) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_PRECONDITION,
            FOC_ENCODER_CAL_ERR_SAMPLE
        );
    }
    if (sample->encoder_us != sample->reference_us ||
        (capture->samples != 0U &&
         (sample->sequence != capture->last_sequence + 1U ||
          !foc_encoder_cal_newer(sample->encoder_us, capture->last_us))) ||
        (!training && capture->samples == 0U &&
         !foc_encoder_cal_newer(sample->encoder_us, result->training.last_us))) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_TIME_ORDER,
            FOC_ENCODER_CAL_ERR_SAMPLE
        );
    }
    if (capture->samples == FOC_ENCODER_CAL_MAX_SAMPLES) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_SAMPLE_COUNT,
            FOC_ENCODER_CAL_ERR_QUALITY
        );
    }

    double reference = result->setup.direction *
        (sample->reference_rad - result->reference_zero_rad) / context->radians_per_count;
    if (reference < 0.0) {
        reference += cpr;
    }
    const double error = foc_encoder_cal_wrap_error(reference - sample->raw_code, cpr);
    if (fabs(error) > cpr / 8.0) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_RANGE,
            FOC_ENCODER_CAL_ERR_SAMPLE
        );
    }
    const uint32_t scaled = sample->raw_code * result->nodes;
    const uint32_t index = scaled / cpr;
    const uint32_t remainder = scaled % cpr;

    if (training) {
        const double x = (double)remainder / cpr;
        struct foc_encoder_cal_segment *segment = &context->segments[index];
        segment->fraction += x;
        segment->fraction2 += x * x;
        segment->correction += error;
        segment->fraction_correction += x * error;
        segment->correction2 += error * error;
    } else {
        uint32_t corrected;
        result->sensor_status =
            foc_encoder_correct_code(&context->evaluator, sample->raw_code, &corrected);
        if (result->sensor_status != FOC_ENCODER_STATUS_OK) {
            return foc_encoder_cal_reject(
                context,
                FOC_CALIBRATION_FAILURE_NUMERIC,
                FOC_ENCODER_CAL_ERR_QUALITY
            );
        }
        const double corrected_error =
            foc_encoder_cal_wrap_error(reference - corrected, cpr) * context->radians_per_count;
        const double raw_error = error * context->radians_per_count;
        context->holdout_sse += corrected_error * corrected_error;
        context->holdout_raw_sse += raw_error * raw_error;
        if (fabs(corrected_error) > result->holdout_peak_rad) {
            result->holdout_peak_rad = fabs(corrected_error);
        }
    }
    if (capture->samples == 0U) {
        capture->first_us = sample->encoder_us;
    }
    capture->last_us = sample->encoder_us;
    capture->last_sequence = sample->sequence;
    ++capture->samples;
    ++capture->segment_samples[index];
    capture->coverage[index] |= (uint8_t)(1U << (4U * remainder / cpr));
    return FOC_ENCODER_CAL_OK;
}

int foc_encoder_cal_freeze(struct foc_encoder_cal_context *context)
{
    if (context == NULL) {
        return FOC_ENCODER_CAL_ERR_NULL;
    }
    struct foc_encoder_cal_result *result = &context->result;
    if (result->state != FOC_CALIBRATION_STATE_COLLECTING) {
        return FOC_ENCODER_CAL_ERR_STATE;
    }
    double knots[FOC_ENCODER_CAL_MAX_NODES] = {0};
    if (!foc_encoder_cal_covered(&context->config, &result->training) ||
        !foc_encoder_cal_estimate_knots(context, knots)) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE,
            FOC_ENCODER_CAL_ERR_QUALITY
        );
    }
    double mean = 0.0;
    for (uint32_t i = 0U; i < result->nodes; ++i) {
        mean += knots[i];
    }
    mean /= result->nodes;
    result->zero_bias_rad = mean * context->radians_per_count;
    if (fabs(result->zero_bias_rad) > context->config.max_zero_bias_rad) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_CONFIG_MISMATCH,
            FOC_ENCODER_CAL_ERR_QUALITY
        );
    }
    for (uint32_t i = 0U; i < result->nodes; ++i) {
        const double offset = floor(knots[i] - mean + 0.5);
        if (!isfinite(offset) || offset < INT16_MIN || offset > INT16_MAX) {
            return foc_encoder_cal_reject(
                context,
                FOC_CALIBRATION_FAILURE_RANGE,
                FOC_ENCODER_CAL_ERR_QUALITY
            );
        }
        result->offsets[i] = (int16_t)offset;
    }
    const struct foc_encoder_lut lut = foc_encoder_cal_descriptor(result);
    result->sensor_status = foc_encoder_init_with_lut(
        &context->evaluator,
        &context->config.encoder,
        result->setup.encoder_identity,
        &lut
    );
    if (result->sensor_status != FOC_ENCODER_STATUS_OK) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_RANGE,
            FOC_ENCODER_CAL_ERR_QUALITY
        );
    }
    foc_encoder_cal_training_residual(context);
    if (result->training_max_segment_rms_rad > context->config.max_training_segment_rms_rad) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_RESIDUAL,
            FOC_ENCODER_CAL_ERR_QUALITY
        );
    }
    result->state = FOC_CALIBRATION_STATE_CANDIDATE;
    return FOC_ENCODER_CAL_OK;
}

int foc_encoder_cal_begin_holdout(struct foc_encoder_cal_context *context, uint32_t capture_id)
{
    if (context == NULL) {
        return FOC_ENCODER_CAL_ERR_NULL;
    }
    struct foc_encoder_cal_result *result = &context->result;
    if (result->state != FOC_CALIBRATION_STATE_CANDIDATE || result->holdout.capture_id != 0U) {
        return FOC_ENCODER_CAL_ERR_STATE;
    }
    if (capture_id == 0U || capture_id == result->training.capture_id) {
        return FOC_ENCODER_CAL_ERR_CONFIG;
    }
    result->holdout.capture_id = capture_id;
    return FOC_ENCODER_CAL_OK;
}

int foc_encoder_cal_finish(struct foc_encoder_cal_context *context)
{
    if (context == NULL) {
        return FOC_ENCODER_CAL_ERR_NULL;
    }
    struct foc_encoder_cal_result *result = &context->result;
    if (result->state != FOC_CALIBRATION_STATE_CANDIDATE || result->holdout.capture_id == 0U) {
        return FOC_ENCODER_CAL_ERR_STATE;
    }
    if (result->holdout.samples != 0U) {
        result->holdout_rms_rad = sqrt(context->holdout_sse / result->holdout.samples);
        result->holdout_raw_rms_rad = sqrt(context->holdout_raw_sse / result->holdout.samples);
    }
    if (!foc_encoder_cal_covered(&context->config, &result->holdout)) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE,
            FOC_ENCODER_CAL_ERR_QUALITY
        );
    }
    if (result->holdout_rms_rad > context->config.max_holdout_rms_rad ||
        result->holdout_peak_rad > context->config.max_holdout_peak_rad ||
        context->holdout_sse > context->holdout_raw_sse) {
        return foc_encoder_cal_reject(
            context,
            FOC_CALIBRATION_FAILURE_RESIDUAL,
            FOC_ENCODER_CAL_ERR_QUALITY
        );
    }
    result->state = FOC_CALIBRATION_STATE_VALID;
    return FOC_ENCODER_CAL_OK;
}

int foc_encoder_cal_export(
    const struct foc_encoder_cal_context *context,
    struct foc_encoder_cal_result *result
)
{
    if (context == NULL || result == NULL) {
        return FOC_ENCODER_CAL_ERR_NULL;
    }
    if (context->result.state != FOC_CALIBRATION_STATE_VALID) {
        return FOC_ENCODER_CAL_ERR_STATE;
    }
    *result = context->result;
    return FOC_ENCODER_CAL_OK;
}

int foc_encoder_cal_make_lut(
    const struct foc_encoder_cal_result *result,
    struct foc_encoder_lut *lut
)
{
    if (result == NULL || lut == NULL) {
        return FOC_ENCODER_CAL_ERR_NULL;
    }
    if (result->state != FOC_CALIBRATION_STATE_VALID || result->nodes < 4U ||
        result->nodes > FOC_ENCODER_CAL_MAX_NODES) {
        return FOC_ENCODER_CAL_ERR_STATE;
    }
    *lut = foc_encoder_cal_descriptor(result);
    return FOC_ENCODER_CAL_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
