/**
 * @file    foc_calibration.c
 * @brief   Portable Float32 current and rotor calibration candidates.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Implementation role
 * -------------------
 * Collect bounded, source-identified data and produce explicitly validated
 * calibration candidates. This translation unit never applies a result.
 *
 * Includes
 * --------
 * Only the public calibration contract and standard C math/integer facilities
 * are used. There are no MCU, SPI, Runtime, Safety, Device, or Flash includes.
 *
 * Private Helpers
 * ---------------
 * The helpers implement modular source checks, circular residuals, Welford
 * statistics, and bounded candidate fitting for the public sessions.
 *
 * Public Functions
 * ----------------
 * Identity comparison plus ADC, rotor, and electrical-offset session APIs are
 * the complete module boundary.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration.h"
#include "foc_trig.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Macros and Constants ------------------------------------------------- */

#define FOC_CALIBRATION_PI         3.14159265358979323846f
#define FOC_CALIBRATION_TWO_PI     6.28318530717958647692f
#define FOC_CALIBRATION_HALF_RANGE UINT32_C(0x80000000)
#define FOC_CALIBRATION_PHASE_MASK UINT32_C(0x07)

enum {
    FOC_CALIBRATION_OK = 0,
    FOC_CALIBRATION_ERR_NULL = -1,
    FOC_CALIBRATION_ERR_CONFIG = -2,
    FOC_CALIBRATION_ERR_STATE = -3,
    FOC_CALIBRATION_ERR_SAMPLE = -4,
    FOC_CALIBRATION_ERR_PRECONDITION = -5,
    FOC_CALIBRATION_ERR_QUALITY = -6,
};

/* Private Helpers -------------------------------------------------------------- */

static bool foc_calibration_finite(float value)
{
    return isfinite(value) != 0;
}

static float foc_calibration_wrap_2pi(float angle)
{
    float wrapped = fmodf(angle, FOC_CALIBRATION_TWO_PI);

    if (wrapped < 0.0f) {
        wrapped += FOC_CALIBRATION_TWO_PI;
    }
    return wrapped >= FOC_CALIBRATION_TWO_PI ? 0.0f : wrapped;
}

static float foc_calibration_wrap_pi(float angle)
{
    float wrapped = fmodf(angle + FOC_CALIBRATION_PI, FOC_CALIBRATION_TWO_PI);

    if (wrapped < 0.0f) {
        wrapped += FOC_CALIBRATION_TWO_PI;
    }
    return wrapped - FOC_CALIBRATION_PI;
}

static bool foc_calibration_newer(uint32_t now, uint32_t previous)
{
    const uint32_t delta = now - previous;

    return delta != 0U && delta < FOC_CALIBRATION_HALF_RANGE;
}

static bool foc_calibration_interval_ok(uint32_t now, uint32_t previous, uint32_t maximum)
{
    const uint32_t delta = now - previous;

    return delta != 0U && delta < FOC_CALIBRATION_HALF_RANGE && delta <= maximum;
}

static bool foc_calibration_identity_valid(
    const struct foc_calibration_identity *identity,
    bool require_direction,
    bool require_poles
)
{
    if (identity == NULL || identity->adc_full_scale_counts == 0U ||
        !foc_calibration_finite(identity->current_a_per_count) ||
        identity->current_a_per_count <= 0.0f || identity->encoder_counts_per_revolution < 2U ||
        identity->encoder_counts_per_revolution > UINT32_C(65536) ||
        (require_direction && identity->encoder_direction != 1 &&
         identity->encoder_direction != -1) ||
        (!require_direction && identity->encoder_direction != 0 &&
         identity->encoder_direction != 1 && identity->encoder_direction != -1) ||
        (require_poles && (identity->pole_pairs == 0U || identity->pole_pairs > 1000U)) ||
        (!require_poles && identity->pole_pairs > 1000U)) {
        return false;
    }
    return true;
}

static void foc_calibration_set_adc_failure(
    struct foc_calibration_adc_context *context,
    enum foc_calibration_failure failure
)
{
    context->result.identity = context->config.identity;
    context->result.samples = context->samples;
    context->result.state = FOC_CALIBRATION_STATE_INVALID;
    context->result.failure = failure;
    context->result.usable = false;
}

static void foc_calibration_set_rotor_failure(
    struct foc_calibration_rotor_context *context,
    enum foc_calibration_failure failure
)
{
    context->result.identity = context->config.identity;
    context->result.samples = context->sample_count;
    context->result.state = FOC_CALIBRATION_STATE_INVALID;
    context->result.failure = failure;
    context->result.identifiable = false;
}

static void foc_calibration_set_offset_failure(
    struct foc_calibration_offset_context *context,
    enum foc_calibration_failure failure
)
{
    context->result.identity = context->config.identity;
    context->result.samples = context->sample_count;
    context->result.state = FOC_CALIBRATION_STATE_INVALID;
    context->result.failure = failure;
    context->result.usable = false;
}

static bool foc_calibration_reference_sample_valid(
    const struct foc_calibration_angle_sample *sample
)
{
    return sample != NULL && sample->valid && foc_calibration_finite(sample->mechanical_rad) &&
        foc_calibration_finite(sample->electrical_rad);
}

static int foc_calibration_reference_sequence_ok(
    const struct foc_calibration_angle_sample *previous,
    const struct foc_calibration_angle_sample *sample,
    uint32_t maximum_interval_us
)
{
    if (!foc_calibration_newer(sample->sequence, previous->sequence) ||
        !foc_calibration_interval_ok(
            sample->captured_us,
            previous->captured_us,
            maximum_interval_us
        )) {
        return FOC_CALIBRATION_ERR_SAMPLE;
    }
    return FOC_CALIBRATION_OK;
}

static float foc_calibration_candidate_offset(
    const struct foc_calibration_angle_sample *samples,
    uint32_t count,
    int32_t direction,
    uint16_t pole_pairs
)
{
    float sum_sine = 0.0f;
    float sum_cosine = 0.0f;

    for (uint32_t index = 0U; index < count; ++index) {
        const float phase = samples[index].electrical_rad -
            (float)direction * (float)pole_pairs * samples[index].mechanical_rad;
        float sine;
        float cosine;

        foc_sincosf(phase, &sine, &cosine);
        sum_sine += sine;
        sum_cosine += cosine;
    }
    return atan2f(sum_sine, sum_cosine);
}

static void foc_calibration_candidate_residual(
    const struct foc_calibration_angle_sample *samples,
    uint32_t count,
    int32_t direction,
    uint16_t pole_pairs,
    float offset,
    float *rms,
    float *maximum
)
{
    float sum_square = 0.0f;
    float max_error = 0.0f;

    for (uint32_t index = 0U; index < count; ++index) {
        const float predicted =
            (float)direction * (float)pole_pairs * samples[index].mechanical_rad + offset;
        const float error =
            fabsf(foc_calibration_wrap_pi(samples[index].electrical_rad - predicted));

        sum_square += error * error;
        if (error > max_error) {
            max_error = error;
        }
    }
    *rms = sqrtf(sum_square / (float)count);
    *maximum = max_error;
}

static bool foc_calibration_rotor_config_valid(const struct foc_calibration_rotor_config *config)
{
    return config != NULL && foc_calibration_identity_valid(&config->identity, false, false) &&
        config->minimum_pole_pairs >= 1U &&
        config->maximum_pole_pairs >= config->minimum_pole_pairs &&
        config->maximum_pole_pairs <= 1000U &&
        foc_calibration_finite(config->minimum_mechanical_span_rad) &&
        config->minimum_mechanical_span_rad > 0.0f &&
        foc_calibration_finite(config->maximum_residual_rad) &&
        config->maximum_residual_rad > 0.0f && config->maximum_residual_rad < FOC_CALIBRATION_PI &&
        foc_calibration_finite(config->minimum_candidate_separation_rad) &&
        config->minimum_candidate_separation_rad > 0.0f &&
        config->minimum_candidate_separation_rad < FOC_CALIBRATION_PI &&
        config->maximum_interval_us > 0U &&
        config->maximum_interval_us < FOC_CALIBRATION_HALF_RANGE && config->maximum_samples >= 4U &&
        config->maximum_samples <= FOC_CALIBRATION_MAX_REFERENCE_SAMPLES;
}

static bool foc_calibration_offset_config_valid(const struct foc_calibration_offset_config *config)
{
    return config != NULL && foc_calibration_identity_valid(&config->identity, true, true) &&
        foc_calibration_finite(config->maximum_residual_rad) &&
        config->maximum_residual_rad > 0.0f && config->maximum_residual_rad < FOC_CALIBRATION_PI &&
        config->maximum_interval_us > 0U &&
        config->maximum_interval_us < FOC_CALIBRATION_HALF_RANGE && config->maximum_samples >= 3U &&
        config->maximum_samples <= FOC_CALIBRATION_MAX_REFERENCE_SAMPLES;
}

static bool foc_calibration_identity_static_matches(
    const struct foc_calibration_identity *expected,
    const struct foc_calibration_identity *actual
)
{
    return expected != NULL && actual != NULL &&
        expected->adc_full_scale_counts == actual->adc_full_scale_counts &&
        expected->current_a_per_count == actual->current_a_per_count &&
        expected->encoder_counts_per_revolution == actual->encoder_counts_per_revolution &&
        expected->phase_order == actual->phase_order;
}

static int foc_calibration_admission_fail(
    enum foc_calibration_admission_failure *failure,
    enum foc_calibration_admission_failure reason,
    int status
)
{
    if (failure != NULL) {
        *failure = reason;
    }
    return status;
}

static bool foc_calibration_admission_limits_valid(
    const struct foc_calibration_admission_limits *limits,
    uint32_t full_scale
)
{
    return limits != NULL && limits->minimum_adc_samples >= 2U &&
        foc_calibration_finite(limits->minimum_offset_counts) &&
        foc_calibration_finite(limits->maximum_offset_counts) &&
        limits->minimum_offset_counts >= 0.0f &&
        limits->maximum_offset_counts <= (float)full_scale &&
        limits->maximum_offset_counts >= limits->minimum_offset_counts &&
        foc_calibration_finite(limits->maximum_noise_rms_counts) &&
        limits->maximum_noise_rms_counts >= 0.0f &&
        foc_calibration_finite(limits->maximum_rotor_residual_rad) &&
        limits->maximum_rotor_residual_rad > 0.0f &&
        limits->maximum_rotor_residual_rad < FOC_CALIBRATION_PI &&
        foc_calibration_finite(limits->maximum_offset_residual_rad) &&
        limits->maximum_offset_residual_rad > 0.0f &&
        limits->maximum_offset_residual_rad < FOC_CALIBRATION_PI;
}

/* Public Functions ------------------------------------------------------------- */

bool foc_calibration_identity_matches(
    const struct foc_calibration_identity *expected,
    const struct foc_calibration_identity *actual
)
{
    return expected != NULL && actual != NULL &&
        expected->adc_full_scale_counts == actual->adc_full_scale_counts &&
        expected->current_a_per_count == actual->current_a_per_count &&
        expected->encoder_counts_per_revolution == actual->encoder_counts_per_revolution &&
        expected->encoder_direction == actual->encoder_direction &&
        expected->phase_order == actual->phase_order && expected->pole_pairs == actual->pole_pairs;
}

int foc_calibration_admit(
    const struct foc_calibration_admission_input *input,
    struct foc_calibration_snapshot *snapshot,
    enum foc_calibration_admission_failure *failure
)
{
    if (failure != NULL) {
        *failure = FOC_CALIBRATION_ADMISSION_NONE;
    }
    if (input == NULL || snapshot == NULL || input->adc == NULL || input->rotor == NULL ||
        input->offset == NULL || input->expected_identity == NULL) {
        return foc_calibration_admission_fail(
            failure,
            FOC_CALIBRATION_ADMISSION_NULL,
            FOC_CALIBRATION_ADMISSION_ERR_NULL
        );
    }
    if (!foc_calibration_identity_valid(input->expected_identity, true, true) ||
        input->expected_identity->phase_order > 1U ||
        !foc_calibration_admission_limits_valid(
            &input->limits,
            input->expected_identity->adc_full_scale_counts
        )) {
        return foc_calibration_admission_fail(
            failure,
            FOC_CALIBRATION_ADMISSION_INVALID_CONFIG,
            FOC_CALIBRATION_ADMISSION_ERR_CONFIG
        );
    }

    const struct foc_calibration_adc_result *adc = input->adc;
    if (!adc->usable || adc->state != FOC_CALIBRATION_STATE_VALID ||
        adc->samples < input->limits.minimum_adc_samples ||
        !foc_calibration_identity_matches(input->expected_identity, &adc->identity)) {
        return foc_calibration_admission_fail(
            failure,
            !foc_calibration_identity_matches(input->expected_identity, &adc->identity)
                ? FOC_CALIBRATION_ADMISSION_ADC_IDENTITY
                : FOC_CALIBRATION_ADMISSION_ADC_QUALITY,
            FOC_CALIBRATION_ADMISSION_ERR_CANDIDATE
        );
    }
    const float offsets[3] = {adc->offset_a_counts, adc->offset_b_counts, adc->offset_c_counts};
    const float noises[3] = {
        adc->noise_rms_a_counts,
        adc->noise_rms_b_counts,
        adc->noise_rms_c_counts,
    };
    for (unsigned int index = 0U; index < 3U; ++index) {
        if (!foc_calibration_finite(offsets[index]) || !foc_calibration_finite(noises[index]) ||
            offsets[index] < input->limits.minimum_offset_counts ||
            offsets[index] > input->limits.maximum_offset_counts ||
            noises[index] > input->limits.maximum_noise_rms_counts) {
            return foc_calibration_admission_fail(
                failure,
                FOC_CALIBRATION_ADMISSION_ADC_RANGE,
                FOC_CALIBRATION_ADMISSION_ERR_CANDIDATE
            );
        }
    }

    const struct foc_calibration_rotor_result *rotor = input->rotor;
    if (!rotor->identifiable || rotor->state != FOC_CALIBRATION_STATE_VALID ||
        !foc_calibration_identity_static_matches(input->expected_identity, &rotor->identity)) {
        return foc_calibration_admission_fail(
            failure,
            !foc_calibration_identity_static_matches(input->expected_identity, &rotor->identity)
                ? FOC_CALIBRATION_ADMISSION_ROTOR_IDENTITY
                : FOC_CALIBRATION_ADMISSION_ROTOR_QUALITY,
            FOC_CALIBRATION_ADMISSION_ERR_CANDIDATE
        );
    }
    if ((rotor->direction != 1 && rotor->direction != -1) ||
        rotor->pole_pairs != input->expected_identity->pole_pairs ||
        !foc_calibration_finite(rotor->electrical_offset_rad) ||
        !foc_calibration_finite(rotor->residual_rms_rad) ||
        !foc_calibration_finite(rotor->residual_max_rad) ||
        !foc_calibration_finite(rotor->candidate_separation_rad) ||
        rotor->residual_rms_rad > input->limits.maximum_rotor_residual_rad ||
        rotor->residual_max_rad > input->limits.maximum_rotor_residual_rad ||
        rotor->candidate_separation_rad <= 0.0f ||
        rotor->identity.encoder_direction != rotor->direction ||
        rotor->identity.pole_pairs != rotor->pole_pairs) {
        return foc_calibration_admission_fail(
            failure,
            FOC_CALIBRATION_ADMISSION_ROTOR_MAPPING,
            FOC_CALIBRATION_ADMISSION_ERR_IDENTITY
        );
    }

    const struct foc_calibration_offset_result *offset = input->offset;
    if (!offset->usable || offset->state != FOC_CALIBRATION_STATE_VALID ||
        !foc_calibration_identity_static_matches(input->expected_identity, &offset->identity)) {
        return foc_calibration_admission_fail(
            failure,
            !foc_calibration_identity_static_matches(input->expected_identity, &offset->identity)
                ? FOC_CALIBRATION_ADMISSION_OFFSET_IDENTITY
                : FOC_CALIBRATION_ADMISSION_OFFSET_QUALITY,
            FOC_CALIBRATION_ADMISSION_ERR_CANDIDATE
        );
    }
    if (offset->identity.encoder_direction != rotor->direction ||
        offset->identity.pole_pairs != rotor->pole_pairs ||
        !foc_calibration_finite(offset->electrical_offset_rad) ||
        !foc_calibration_finite(offset->residual_rms_rad) ||
        !foc_calibration_finite(offset->residual_max_rad) ||
        offset->residual_rms_rad > input->limits.maximum_offset_residual_rad ||
        offset->residual_max_rad > input->limits.maximum_offset_residual_rad ||
        fabsf(
            foc_calibration_wrap_pi(offset->electrical_offset_rad - rotor->electrical_offset_rad)
        ) > input->limits.maximum_offset_residual_rad) {
        return foc_calibration_admission_fail(
            failure,
            FOC_CALIBRATION_ADMISSION_OFFSET_IDENTITY,
            FOC_CALIBRATION_ADMISSION_ERR_IDENTITY
        );
    }

    const int32_t runtime_direction =
        input->expected_identity->encoder_direction * rotor->direction;
    if (runtime_direction != 1 && runtime_direction != -1) {
        return foc_calibration_admission_fail(
            failure,
            FOC_CALIBRATION_ADMISSION_UNSUPPORTED_MAPPING,
            FOC_CALIBRATION_ADMISSION_ERR_IDENTITY
        );
    }
    const float electrical_offset = foc_calibration_wrap_2pi(offset->electrical_offset_rad);
    if (!foc_calibration_finite(electrical_offset)) {
        return foc_calibration_admission_fail(
            failure,
            FOC_CALIBRATION_ADMISSION_NUMERIC,
            FOC_CALIBRATION_ADMISSION_ERR_NUMERIC
        );
    }

    struct foc_calibration_snapshot candidate = {
        .source_identity = *input->expected_identity,
        .phase_a_offset_count = offsets[0],
        .phase_b_offset_count = offsets[1],
        .phase_c_offset_count = offsets[2],
        .calibration_direction = rotor->direction,
        .runtime_encoder_direction = runtime_direction,
        .pole_pairs = rotor->pole_pairs,
        .electrical_offset_rad = electrical_offset,
        .accepted = true,
    };
    *snapshot = candidate;
    return FOC_CALIBRATION_ADMISSION_OK;
}

bool foc_calibration_snapshot_valid(const struct foc_calibration_snapshot *snapshot)
{
    return snapshot != NULL && snapshot->accepted &&
        foc_calibration_identity_valid(&snapshot->source_identity, true, true) &&
        snapshot->source_identity.phase_order <= 1U &&
        snapshot->source_identity.pole_pairs == snapshot->pole_pairs &&
        foc_calibration_finite(snapshot->phase_a_offset_count) &&
        foc_calibration_finite(snapshot->phase_b_offset_count) &&
        foc_calibration_finite(snapshot->phase_c_offset_count) &&
        snapshot->phase_a_offset_count >= 0.0f && snapshot->phase_b_offset_count >= 0.0f &&
        snapshot->phase_c_offset_count >= 0.0f &&
        snapshot->phase_a_offset_count <= (float)snapshot->source_identity.adc_full_scale_counts &&
        snapshot->phase_b_offset_count <= (float)snapshot->source_identity.adc_full_scale_counts &&
        snapshot->phase_c_offset_count <= (float)snapshot->source_identity.adc_full_scale_counts &&
        (snapshot->calibration_direction == 1 || snapshot->calibration_direction == -1) &&
        (snapshot->runtime_encoder_direction == 1 || snapshot->runtime_encoder_direction == -1) &&
        snapshot->pole_pairs > 0U && snapshot->electrical_offset_rad >= 0.0f &&
        snapshot->electrical_offset_rad < FOC_CALIBRATION_TWO_PI;
}

int foc_calibration_adc_init(
    struct foc_calibration_adc_context *context,
    const struct foc_calibration_adc_config *config
)
{
    if (context == NULL || config == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    if (!foc_calibration_identity_valid(&config->identity, false, false) ||
        (config->required_valid_mask & FOC_CALIBRATION_PHASE_MASK) != FOC_CALIBRATION_PHASE_MASK ||
        config->minimum_samples < 2U || config->maximum_samples < config->minimum_samples ||
        config->maximum_samples > UINT32_C(1000000) ||
        !foc_calibration_finite(config->maximum_sample_step_counts) ||
        config->maximum_sample_step_counts <= 0.0f ||
        !foc_calibration_finite(config->maximum_noise_rms_counts) ||
        config->maximum_noise_rms_counts < 0.0f ||
        !foc_calibration_finite(config->minimum_offset_counts) ||
        !foc_calibration_finite(config->maximum_offset_counts) ||
        config->minimum_offset_counts < 0.0f ||
        config->maximum_offset_counts > (float)config->identity.adc_full_scale_counts ||
        config->maximum_offset_counts < config->minimum_offset_counts ||
        config->maximum_interval_us == 0U ||
        config->maximum_interval_us >= FOC_CALIBRATION_HALF_RANGE) {
        return FOC_CALIBRATION_ERR_CONFIG;
    }
    *context = (struct foc_calibration_adc_context){
        .config = *config,
        .result =
            {.identity = config->identity,
             .state = FOC_CALIBRATION_STATE_COLLECTING,
             .failure = FOC_CALIBRATION_FAILURE_NONE},
        .initialized = true,
    };
    return FOC_CALIBRATION_OK;
}

int foc_calibration_adc_reset(struct foc_calibration_adc_context *context)
{
    if (context == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_CALIBRATION_ERR_STATE;
    }
    const struct foc_calibration_adc_config config = context->config;
    return foc_calibration_adc_init(context, &config);
}

int foc_calibration_adc_collect(
    struct foc_calibration_adc_context *context,
    const struct foc_calibration_adc_sample *sample
)
{
    if (context == NULL || sample == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    if (!context->initialized || context->result.state == FOC_CALIBRATION_STATE_INVALID) {
        return FOC_CALIBRATION_ERR_STATE;
    }
    if (!sample->zero_current_confirmed) {
        foc_calibration_set_adc_failure(context, FOC_CALIBRATION_FAILURE_PRECONDITION);
        return FOC_CALIBRATION_ERR_PRECONDITION;
    }
    if ((sample->valid_mask & context->config.required_valid_mask) !=
            context->config.required_valid_mask ||
        sample->phase_a > context->config.identity.adc_full_scale_counts ||
        sample->phase_b > context->config.identity.adc_full_scale_counts ||
        sample->phase_c > context->config.identity.adc_full_scale_counts) {
        foc_calibration_set_adc_failure(context, FOC_CALIBRATION_FAILURE_INVALID_SAMPLE);
        return FOC_CALIBRATION_ERR_SAMPLE;
    }
    if (context->samples >= context->config.maximum_samples) {
        foc_calibration_set_adc_failure(context, FOC_CALIBRATION_FAILURE_SAMPLE_COUNT);
        return FOC_CALIBRATION_ERR_QUALITY;
    }
    if (context->previous_seen &&
        (!foc_calibration_newer(sample->sequence, context->previous_sequence) ||
         !foc_calibration_interval_ok(
             sample->captured_us,
             context->previous_captured_us,
             context->config.maximum_interval_us
         ))) {
        foc_calibration_set_adc_failure(context, FOC_CALIBRATION_FAILURE_TIME_ORDER);
        return FOC_CALIBRATION_ERR_SAMPLE;
    }
    const uint16_t values[3] = {sample->phase_a, sample->phase_b, sample->phase_c};
    if (context->previous_seen) {
        for (unsigned int channel = 0U; channel < 3U; ++channel) {
            if (fabsf((float)values[channel] - (float)context->previous[channel]) >
                context->config.maximum_sample_step_counts) {
                foc_calibration_set_adc_failure(context, FOC_CALIBRATION_FAILURE_INPUT_CHANGED);
                return FOC_CALIBRATION_ERR_QUALITY;
            }
        }
    }
    const uint32_t next = context->samples + 1U;
    for (unsigned int channel = 0U; channel < 3U; ++channel) {
        const float value = (float)values[channel];
        const float delta = value - context->mean[channel];
        context->mean[channel] += delta / (float)next;
        context->m2[channel] += delta * (value - context->mean[channel]);
        context->previous[channel] = values[channel];
    }
    context->samples = next;
    context->previous_sequence = sample->sequence;
    context->previous_captured_us = sample->captured_us;
    context->previous_seen = true;
    context->result.samples = next;
    context->result.state = FOC_CALIBRATION_STATE_COLLECTING;
    return FOC_CALIBRATION_OK;
}

int foc_calibration_adc_finalize(
    struct foc_calibration_adc_context *context,
    struct foc_calibration_adc_result *result
)
{
    if (result == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    *result = (struct foc_calibration_adc_result){0};
    if (context == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    *result = context->result;
    result->identity = context->config.identity;
    result->samples = context->samples;
    if (!context->initialized || context->result.state == FOC_CALIBRATION_STATE_INVALID) {
        result->state = FOC_CALIBRATION_STATE_INVALID;
        result->failure =
            context->initialized ? context->result.failure : FOC_CALIBRATION_FAILURE_INVALID_CONFIG;
        return FOC_CALIBRATION_ERR_STATE;
    }
    if (context->samples < context->config.minimum_samples) {
        foc_calibration_set_adc_failure(context, FOC_CALIBRATION_FAILURE_SAMPLE_COUNT);
        *result = context->result;
        return FOC_CALIBRATION_ERR_QUALITY;
    }
    result->offset_a_counts = context->mean[0];
    result->offset_b_counts = context->mean[1];
    result->offset_c_counts = context->mean[2];
    result->noise_rms_a_counts = sqrtf(context->m2[0] / (float)(context->samples - 1U));
    result->noise_rms_b_counts = sqrtf(context->m2[1] / (float)(context->samples - 1U));
    result->noise_rms_c_counts = sqrtf(context->m2[2] / (float)(context->samples - 1U));
    const float offsets[3] =
        {result->offset_a_counts, result->offset_b_counts, result->offset_c_counts};
    const float noises[3] =
        {result->noise_rms_a_counts, result->noise_rms_b_counts, result->noise_rms_c_counts};
    for (unsigned int channel = 0U; channel < 3U; ++channel) {
        if (!foc_calibration_finite(offsets[channel]) || !foc_calibration_finite(noises[channel])) {
            foc_calibration_set_adc_failure(context, FOC_CALIBRATION_FAILURE_NUMERIC);
            *result = context->result;
            return FOC_CALIBRATION_ERR_QUALITY;
        }
        if (offsets[channel] < context->config.minimum_offset_counts ||
            offsets[channel] > context->config.maximum_offset_counts) {
            foc_calibration_set_adc_failure(context, FOC_CALIBRATION_FAILURE_RANGE);
            *result = context->result;
            return FOC_CALIBRATION_ERR_QUALITY;
        }
        if (noises[channel] > context->config.maximum_noise_rms_counts) {
            foc_calibration_set_adc_failure(context, FOC_CALIBRATION_FAILURE_NOISE);
            *result = context->result;
            return FOC_CALIBRATION_ERR_QUALITY;
        }
    }
    context->result = *result;
    context->result.state = FOC_CALIBRATION_STATE_VALID;
    context->result.failure = FOC_CALIBRATION_FAILURE_NONE;
    context->result.usable = true;
    *result = context->result;
    return FOC_CALIBRATION_OK;
}

int foc_calibration_rotor_init(
    struct foc_calibration_rotor_context *context,
    const struct foc_calibration_rotor_config *config
)
{
    if (context == NULL || config == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    if (!foc_calibration_rotor_config_valid(config)) {
        return FOC_CALIBRATION_ERR_CONFIG;
    }
    *context = (struct foc_calibration_rotor_context){
        .config = *config,
        .result =
            {.identity = config->identity,
             .state = FOC_CALIBRATION_STATE_COLLECTING,
             .failure = FOC_CALIBRATION_FAILURE_NONE},
        .initialized = true,
    };
    return FOC_CALIBRATION_OK;
}

int foc_calibration_rotor_reset(struct foc_calibration_rotor_context *context)
{
    if (context == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_CALIBRATION_ERR_STATE;
    }
    const struct foc_calibration_rotor_config config = context->config;
    return foc_calibration_rotor_init(context, &config);
}

int foc_calibration_rotor_collect(
    struct foc_calibration_rotor_context *context,
    const struct foc_calibration_angle_sample *sample
)
{
    if (context == NULL || sample == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    if (!context->initialized || context->result.state == FOC_CALIBRATION_STATE_INVALID) {
        return FOC_CALIBRATION_ERR_STATE;
    }
    if (!foc_calibration_reference_sample_valid(sample)) {
        foc_calibration_set_rotor_failure(context, FOC_CALIBRATION_FAILURE_INVALID_SAMPLE);
        return FOC_CALIBRATION_ERR_SAMPLE;
    }
    if (context->sample_count >= context->config.maximum_samples) {
        foc_calibration_set_rotor_failure(context, FOC_CALIBRATION_FAILURE_SAMPLE_COUNT);
        return FOC_CALIBRATION_ERR_QUALITY;
    }
    if (context->sample_count != 0U &&
        foc_calibration_reference_sequence_ok(
            &context->samples[context->sample_count - 1U],
            sample,
            context->config.maximum_interval_us
        ) != FOC_CALIBRATION_OK) {
        foc_calibration_set_rotor_failure(context, FOC_CALIBRATION_FAILURE_TIME_ORDER);
        return FOC_CALIBRATION_ERR_SAMPLE;
    }
    context->samples[context->sample_count++] = *sample;
    context->result.samples = context->sample_count;
    return FOC_CALIBRATION_OK;
}

int foc_calibration_rotor_finalize(
    struct foc_calibration_rotor_context *context,
    struct foc_calibration_rotor_result *result
)
{
    if (result == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    *result = (struct foc_calibration_rotor_result){0};
    if (context == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    *result = context->result;
    result->identity = context->config.identity;
    result->samples = context->sample_count;
    if (!context->initialized || context->result.state == FOC_CALIBRATION_STATE_INVALID) {
        result->state = FOC_CALIBRATION_STATE_INVALID;
        result->failure =
            context->initialized ? context->result.failure : FOC_CALIBRATION_FAILURE_INVALID_CONFIG;
        return FOC_CALIBRATION_ERR_STATE;
    }
    if (context->sample_count < 4U) {
        foc_calibration_set_rotor_failure(context, FOC_CALIBRATION_FAILURE_SAMPLE_COUNT);
        *result = context->result;
        return FOC_CALIBRATION_ERR_QUALITY;
    }
    float minimum_mechanical = context->samples[0].mechanical_rad;
    float maximum_mechanical = minimum_mechanical;
    for (uint32_t index = 1U; index < context->sample_count; ++index) {
        minimum_mechanical = fminf(minimum_mechanical, context->samples[index].mechanical_rad);
        maximum_mechanical = fmaxf(maximum_mechanical, context->samples[index].mechanical_rad);
    }
    result->mechanical_span_rad = maximum_mechanical - minimum_mechanical;
    if (!foc_calibration_finite(result->mechanical_span_rad) ||
        result->mechanical_span_rad < context->config.minimum_mechanical_span_rad) {
        foc_calibration_set_rotor_failure(context, FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE);
        *result = context->result;
        return FOC_CALIBRATION_ERR_QUALITY;
    }

    float best_rms = FLT_MAX;
    float best_max = FLT_MAX;
    float best_offset = 0.0f;
    int32_t best_direction = 0;
    uint16_t best_poles = 0U;
    float second_rms = FLT_MAX;
    for (int32_t direction = -1; direction <= 1; direction += 2) {
        for (uint16_t poles = context->config.minimum_pole_pairs;
             poles <= context->config.maximum_pole_pairs;
             ++poles) {
            const float offset = foc_calibration_candidate_offset(
                context->samples,
                context->sample_count,
                direction,
                poles
            );
            float rms;
            float maximum;
            foc_calibration_candidate_residual(
                context->samples,
                context->sample_count,
                direction,
                poles,
                offset,
                &rms,
                &maximum
            );
            if (rms < best_rms) {
                second_rms = best_rms;
                best_rms = rms;
                best_max = maximum;
                best_offset = offset;
                best_direction = direction;
                best_poles = poles;
            } else if (rms < second_rms) {
                second_rms = rms;
            }
        }
    }
    result->candidate_separation_rad = second_rms - best_rms;
    result->residual_rms_rad = best_rms;
    result->residual_max_rad = best_max;
    result->electrical_offset_rad = foc_calibration_wrap_2pi(best_offset);
    result->direction = best_direction;
    result->pole_pairs = best_poles;
    result->identity.encoder_direction = best_direction;
    result->identity.pole_pairs = best_poles;
    if (best_direction == 0 || best_poles == 0U || !foc_calibration_finite(best_rms) ||
        best_rms > context->config.maximum_residual_rad ||
        result->candidate_separation_rad < context->config.minimum_candidate_separation_rad) {
        foc_calibration_set_rotor_failure(context, FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE);
        *result = context->result;
        return FOC_CALIBRATION_ERR_QUALITY;
    }
    context->result = *result;
    context->result.state = FOC_CALIBRATION_STATE_VALID;
    context->result.failure = FOC_CALIBRATION_FAILURE_NONE;
    context->result.identifiable = true;
    *result = context->result;
    return FOC_CALIBRATION_OK;
}

int foc_calibration_offset_init(
    struct foc_calibration_offset_context *context,
    const struct foc_calibration_offset_config *config
)
{
    if (context == NULL || config == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    if (!foc_calibration_offset_config_valid(config)) {
        return FOC_CALIBRATION_ERR_CONFIG;
    }
    *context = (struct foc_calibration_offset_context){
        .config = *config,
        .result =
            {.identity = config->identity,
             .state = FOC_CALIBRATION_STATE_COLLECTING,
             .failure = FOC_CALIBRATION_FAILURE_NONE},
        .initialized = true,
    };
    return FOC_CALIBRATION_OK;
}

int foc_calibration_offset_reset(struct foc_calibration_offset_context *context)
{
    if (context == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_CALIBRATION_ERR_STATE;
    }
    const struct foc_calibration_offset_config config = context->config;
    return foc_calibration_offset_init(context, &config);
}

int foc_calibration_offset_collect(
    struct foc_calibration_offset_context *context,
    const struct foc_calibration_angle_sample *sample
)
{
    if (context == NULL || sample == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    if (!context->initialized || context->result.state == FOC_CALIBRATION_STATE_INVALID) {
        return FOC_CALIBRATION_ERR_STATE;
    }
    if (!foc_calibration_reference_sample_valid(sample)) {
        foc_calibration_set_offset_failure(context, FOC_CALIBRATION_FAILURE_INVALID_SAMPLE);
        return FOC_CALIBRATION_ERR_SAMPLE;
    }
    if (context->sample_count >= context->config.maximum_samples) {
        foc_calibration_set_offset_failure(context, FOC_CALIBRATION_FAILURE_SAMPLE_COUNT);
        return FOC_CALIBRATION_ERR_QUALITY;
    }
    if (context->sample_count != 0U &&
        foc_calibration_reference_sequence_ok(
            &context->samples[context->sample_count - 1U],
            sample,
            context->config.maximum_interval_us
        ) != FOC_CALIBRATION_OK) {
        foc_calibration_set_offset_failure(context, FOC_CALIBRATION_FAILURE_TIME_ORDER);
        return FOC_CALIBRATION_ERR_SAMPLE;
    }
    context->samples[context->sample_count++] = *sample;
    context->result.samples = context->sample_count;
    return FOC_CALIBRATION_OK;
}

int foc_calibration_offset_finalize(
    struct foc_calibration_offset_context *context,
    struct foc_calibration_offset_result *result
)
{
    if (result == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    *result = (struct foc_calibration_offset_result){0};
    if (context == NULL) {
        return FOC_CALIBRATION_ERR_NULL;
    }
    *result = context->result;
    result->identity = context->config.identity;
    result->samples = context->sample_count;
    if (!context->initialized || context->result.state == FOC_CALIBRATION_STATE_INVALID) {
        result->state = FOC_CALIBRATION_STATE_INVALID;
        result->failure =
            context->initialized ? context->result.failure : FOC_CALIBRATION_FAILURE_INVALID_CONFIG;
        return FOC_CALIBRATION_ERR_STATE;
    }
    if (context->sample_count < 3U) {
        foc_calibration_set_offset_failure(context, FOC_CALIBRATION_FAILURE_SAMPLE_COUNT);
        *result = context->result;
        return FOC_CALIBRATION_ERR_QUALITY;
    }
    const float offset = foc_calibration_candidate_offset(
        context->samples,
        context->sample_count,
        context->config.identity.encoder_direction,
        context->config.identity.pole_pairs
    );
    foc_calibration_candidate_residual(
        context->samples,
        context->sample_count,
        context->config.identity.encoder_direction,
        context->config.identity.pole_pairs,
        offset,
        &result->residual_rms_rad,
        &result->residual_max_rad
    );
    result->electrical_offset_rad = foc_calibration_wrap_2pi(offset);
    if (!foc_calibration_finite(result->electrical_offset_rad) ||
        !foc_calibration_finite(result->residual_rms_rad) ||
        result->residual_rms_rad > context->config.maximum_residual_rad) {
        foc_calibration_set_offset_failure(context, FOC_CALIBRATION_FAILURE_RESIDUAL);
        *result = context->result;
        return FOC_CALIBRATION_ERR_QUALITY;
    }
    context->result = *result;
    context->result.state = FOC_CALIBRATION_STATE_VALID;
    context->result.failure = FOC_CALIBRATION_FAILURE_NONE;
    context->result.usable = true;
    *result = context->result;
    return FOC_CALIBRATION_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
