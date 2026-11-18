/**
 * @file    test_foc_encoder_calibration.c
 * @brief   Capture admission, periodic fit and exact holdout quality tests.
 * @version v0.1.0
 * @date    2026.11.27
 *
 * Test role
 * ---------
 * Exercise independent signed reference observations through the public API.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_encoder_calibration.h"

#include <math.h>
#include <stdio.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            fprintf(stderr, "encoder cal: %d: %s\n", __LINE__, #c); \
            return 1;                                               \
        }                                                           \
    } while (0)

static const double tau = 6.28318530717958647692;
static const int16_t known[] = {0, 4, 8, 4, 0, -4, -8, -4};
static const int16_t bad_slope[] = {0, 80, -80, 0, 0, 0, 0, 0};

static const struct foc_encoder_cal_config policy = {
    .encoder = {1024U, FOC_ENCODER_DIRECTION_NORMAL, 1000.0f, 1U, 2000U, 80.0f, 0.85f, 0.2f},
    .encoder_identity = 31U,
    .reference_identity = 41U,
    .source = FOC_ENCODER_CAL_SOURCE_VIRTUAL,
    .nodes = 8U,
    .min_samples_per_segment = 8U,
    .reference_zero_rad = 0.31,
    .max_zero_bias_rad = 0.004,
    .max_training_segment_rms_rad = 0.004,
    .max_holdout_rms_rad = 0.004,
    .max_holdout_peak_rad = 0.006,
};

static struct foc_encoder_cal_sample sample(
    const struct foc_encoder_cal_config *config,
    uint32_t raw,
    uint32_t id,
    uint32_t index,
    uint32_t origin,
    const int16_t *truth,
    double error_counts
)
{
    const uint32_t bin = raw / 128U;
    const double x = (double)(raw % 128U) / 128.0;
    const double count = raw + (1.0 - x) * truth[bin] + x * truth[(bin + 1U) % 8U] + error_counts;
    const double a = config->reference_zero_rad + config->encoder.direction * count * tau / 1024.0;
    const double reference = a - floor(a / tau) * tau;
    const uint32_t time = origin + index * 10U;
    return (struct foc_encoder_cal_sample){
        .setup =
            {
                config->encoder_identity,
                config->reference_identity,
                1024U,
                config->encoder.direction,
                config->source,
            },
        .capture_id = id,
        .sequence = UINT32_MAX - 50U + index,
        .encoder_us = time,
        .reference_us = time,
        .raw_code = raw,
        .reference_rad = reference,
        .encoder_valid = true,
        .reference_valid = true,
        .reference_confirmed = true,
    };
}

static int fill(
    struct foc_encoder_cal_context *context,
    uint32_t id,
    uint32_t origin,
    unsigned stride,
    unsigned start,
    const int16_t *truth,
    double bias,
    bool spike
)
{
    unsigned index = 0U;
    for (uint32_t code = start; code < 1024U; code += stride) {
        const struct foc_encoder_cal_sample s = sample(
            &context->config,
            code,
            id,
            index,
            origin,
            truth,
            bias + ((spike && index == 20U) ? 20.0 : 0.0)
        );
        CHECK(foc_encoder_cal_push(context, &s) == FOC_ENCODER_CAL_OK);
        ++index;
    }
    return 0;
}

static int fit_export(void)
{
    struct foc_encoder_cal_context fit = {0};
    struct foc_encoder_cal_result saved;
    struct foc_encoder_lut lut;
    struct foc_encoder_cal_config config = policy;
    for (int direction = -1; direction <= 1; direction += 2) {
        config.encoder.direction = (enum foc_encoder_direction)direction;
        CHECK(foc_encoder_cal_init(&fit, &config, 1U) == 0);
        CHECK(fill(&fit, 1U, UINT32_MAX - 100U, 3U, 0U, known, 0.0, false) == 0);
        CHECK(foc_encoder_cal_freeze(&fit) == 0);
        CHECK(fit.result.state == FOC_CALIBRATION_STATE_CANDIDATE);
        CHECK(foc_encoder_cal_export(&fit, &saved) == FOC_ENCODER_CAL_ERR_STATE);
        CHECK(foc_encoder_cal_begin_holdout(&fit, 1U) == FOC_ENCODER_CAL_ERR_CONFIG);
        CHECK(foc_encoder_cal_begin_holdout(&fit, 2U) == 0);
        CHECK(fill(&fit, 2U, 10000U, 5U, 2U, known, 0.0, false) == 0);
        CHECK(foc_encoder_cal_finish(&fit) == 0);
        CHECK(foc_encoder_cal_export(&fit, &saved) == 0);
        CHECK(foc_encoder_cal_make_lut(&saved, &lut) == 0);
        CHECK(
            lut.encoder_identity == 31U && lut.length == 8U && lut.offset_counts == saved.offsets
        );
        for (unsigned i = 0U; i < lut.length; ++i) {
            CHECK(lut.offset_counts[i] == known[i]);
            CHECK(saved.training.coverage[i] == 15U && saved.holdout.coverage[i] == 15U);
        }
        /* Report a conservative roundoff floor for nonzero fitted terms. */
        CHECK(saved.training_rms_rad >= 1e-8 && saved.training_rms_rad < 1e-7);
        CHECK(fabs(saved.zero_bias_rad) < 1e-10);
        CHECK(saved.holdout_peak_rad <= tau / 2048.0 + 1e-12);
        CHECK(saved.holdout_rms_rad < saved.holdout_raw_rms_rad);
        struct foc_encoder_cal_context other = {0};
        CHECK(foc_encoder_cal_init(&other, &config, 3U) == 0);
        struct foc_encoder_cal_sample foreign = sample(&config, 0U, 3U, 0U, 100U, known, 0.0);
        foreign.setup.reference_identity++;
        CHECK(foc_encoder_cal_push(&other, &foreign) == FOC_ENCODER_CAL_ERR_SAMPLE);
        CHECK(foc_encoder_cal_export(&other, &saved) == FOC_ENCODER_CAL_ERR_STATE);
        CHECK(fit.result.state == FOC_CALIBRATION_STATE_VALID && fit.result.offsets[2] == 8);
        CHECK(foc_encoder_cal_reset(&fit, 9U) == 0);
        CHECK(saved.offsets[2] == 8 && saved.state == FOC_CALIBRATION_STATE_VALID);
    }
    return 0;
}

static int invalid_observations(void)
{
    struct foc_encoder_cal_context fit = {0};
    const enum foc_calibration_failure failures[] = {
        FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
        FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
        FOC_CALIBRATION_FAILURE_PRECONDITION,
        FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
        FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
        FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
        FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
        FOC_CALIBRATION_FAILURE_TIME_ORDER,
        FOC_CALIBRATION_FAILURE_TIME_ORDER,
        FOC_CALIBRATION_FAILURE_TIME_ORDER,
    };
    for (unsigned trial = 0U; trial < sizeof failures / sizeof failures[0]; ++trial) {
        CHECK(foc_encoder_cal_init(&fit, &policy, 81U) == 0);
        struct foc_encoder_cal_sample s = sample(&policy, 1U, 81U, 0U, 40U, known, 0.0);
        CHECK(foc_encoder_cal_push(&fit, &s) == 0);
        s = sample(&policy, 2U, 81U, 1U, 40U, known, 0.0);
        switch (trial) {
        case 0:
            s.reference_rad = NAN;
            break;
        case 1:
            s.raw_code = 1024U;
            break;
        case 2:
            s.reference_confirmed = false;
            break;
        case 3:
            s.setup.source = FOC_ENCODER_CAL_SOURCE_EXTERNAL;
            break;
        case 4:
            s.setup.direction = FOC_ENCODER_DIRECTION_REVERSED;
            break;
        case 5:
            s.setup.counts_per_revolution++;
            break;
        case 6:
            s.capture_id++;
            break;
        case 7:
            ++s.reference_us;
            break;
        case 8:
            ++s.sequence;
            break;
        case 9:
            s.encoder_us = s.reference_us = 39U;
            break;
        }
        CHECK(foc_encoder_cal_push(&fit, &s) != 0);
        CHECK(fit.result.failure == failures[trial]);
        CHECK(
            fit.result.state == FOC_CALIBRATION_STATE_INVALID && fit.result.training.samples == 1U
        );
        CHECK(fit.result.training.last_us == 40U && fit.result.offsets[0] == 0);
        CHECK(foc_encoder_cal_push(&fit, &s) == FOC_ENCODER_CAL_ERR_STATE);
    }
    return 0;
}

static int fit_rejection(void)
{
    struct foc_encoder_cal_context fit = {0};
    CHECK(foc_encoder_cal_init(&fit, &policy, 1U) == 0);
    for (unsigned i = 0U; i < 1024U; ++i) {
        const struct foc_encoder_cal_sample s = sample(&policy, i % 128U, 1U, i, 40U, known, 0.0);
        CHECK(foc_encoder_cal_push(&fit, &s) == 0);
    }
    CHECK(foc_encoder_cal_freeze(&fit) == FOC_ENCODER_CAL_ERR_QUALITY);
    CHECK(fit.result.failure == FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE);
    CHECK(fit.result.training.segment_samples[0] == 1024U && fit.result.training.coverage[1] == 0U);
    CHECK(foc_encoder_cal_reset(&fit, 2U) == 0);
    CHECK(fill(&fit, 2U, 40U, 3U, 0U, known, 4.0, false) == 0);
    CHECK(foc_encoder_cal_freeze(&fit) == FOC_ENCODER_CAL_ERR_QUALITY);
    CHECK(fit.result.failure == FOC_CALIBRATION_FAILURE_CONFIG_MISMATCH);
    CHECK(fit.result.zero_bias_rad > 0.024);
    CHECK(foc_encoder_cal_reset(&fit, 3U) == 0);
    CHECK(fill(&fit, 3U, 40U, 3U, 0U, known, 0.0, true) == 0);
    CHECK(foc_encoder_cal_freeze(&fit) == FOC_ENCODER_CAL_ERR_QUALITY);
    CHECK(fit.result.failure == FOC_CALIBRATION_FAILURE_RESIDUAL);
    CHECK(foc_encoder_cal_reset(&fit, 4U) == 0);
    CHECK(fill(&fit, 4U, 40U, 3U, 0U, bad_slope, 0.0, false) == 0);
    CHECK(foc_encoder_cal_freeze(&fit) == FOC_ENCODER_CAL_ERR_QUALITY);
    CHECK(fit.result.sensor_status == FOC_ENCODER_STATUS_ERR_LUT_SLOPE);
    CHECK(foc_encoder_cal_reset(&fit, 5U) == 0);
    CHECK(fill(&fit, 5U, 40U, 3U, 0U, known, 0.0, false) == 0);
    CHECK(foc_encoder_cal_freeze(&fit) == 0);
    CHECK(foc_encoder_cal_begin_holdout(&fit, 6U) == 0);
    const struct foc_encoder_cal_sample replay = sample(&policy, 1U, 6U, 0U, 40U, known, 0.0);
    CHECK(foc_encoder_cal_push(&fit, &replay) == FOC_ENCODER_CAL_ERR_SAMPLE);
    CHECK(fit.result.failure == FOC_CALIBRATION_FAILURE_TIME_ORDER);
    CHECK(foc_encoder_cal_reset(&fit, 7U) == 0);
    CHECK(fill(&fit, 7U, 40U, 3U, 0U, known, 0.0, false) == 0);
    CHECK(foc_encoder_cal_freeze(&fit) == 0);
    CHECK(foc_encoder_cal_begin_holdout(&fit, 8U) == 0);
    CHECK(fill(&fit, 8U, 10000U, 5U, 2U, known, 0.0, true) == 0);
    CHECK(foc_encoder_cal_finish(&fit) == FOC_ENCODER_CAL_ERR_QUALITY);
    CHECK(
        fit.result.failure == FOC_CALIBRATION_FAILURE_RESIDUAL && fit.result.holdout_peak_rad > 0.1
    );
    struct foc_encoder_cal_result untouched = {.nodes = 90U};
    struct foc_encoder_lut lut = {.length = 90U};
    CHECK(foc_encoder_cal_export(&fit, &untouched) == FOC_ENCODER_CAL_ERR_STATE);
    CHECK(untouched.nodes == 90U);
    CHECK(foc_encoder_cal_make_lut(&fit.result, &lut) == FOC_ENCODER_CAL_ERR_STATE);
    CHECK(lut.length == 90U && fit.result.offsets[2] == 0);

    CHECK(foc_encoder_cal_reset(&fit, 9U) == 0);
    CHECK(fill(&fit, 9U, 40U, 3U, 0U, known, 0.0, false) == 0);
    CHECK(foc_encoder_cal_freeze(&fit) == 0);
    CHECK(foc_encoder_cal_begin_holdout(&fit, 10U) == 0);
    for (unsigned i = 0U; i < 1024U; ++i) {
        const struct foc_encoder_cal_sample s =
            sample(&policy, i % 128U, 10U, i, 10000U, known, 0.0);
        CHECK(foc_encoder_cal_push(&fit, &s) == 0);
    }
    CHECK(foc_encoder_cal_finish(&fit) == FOC_ENCODER_CAL_ERR_QUALITY);
    CHECK(fit.result.failure == FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE);
    CHECK(fit.result.holdout.samples == 1024U && fit.result.offsets[2] == 0);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return fit_export() || invalid_observations() || fit_rejection();
}

/* ---------------------------------------- end of file ----------------------------------------- */
