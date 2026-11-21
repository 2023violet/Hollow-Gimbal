/**
 * @file    foc_bootstrap.c
 * @brief   Qualify a reviewed reference or fit one from open-circuit apertures.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Implementation role
 * -------------------
 * Independent signed shaft speed establishes the mechanical direction and
 * removes the reverse-spin sign from the measured back-EMF. Each sweep uses
 * bounded integer raw-code unwrapping before conversion to estimator radians.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_bootstrap.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Macros and Constants ------------------------------------------------- */

#define FOC_BOOTSTRAP_PI         3.14159265358979323846f
#define FOC_BOOTSTRAP_TWO_PI     6.28318530717958647692f
#define FOC_BOOTSTRAP_HALF_RANGE UINT32_C(0x80000000)

/* Private Helpers -------------------------------------------------------------- */

static bool bootstrap_identity_valid(const struct foc_cal_workflow_identity *identity)
{
    if (identity == NULL || identity->motor_setup_id == 0U || identity->encoder_id == 0U ||
        identity->mechanical_frame_id == 0U) {
        return false;
    }
    const struct foc_calibration_identity *acquisition = &identity->acquisition;
    return acquisition->adc_full_scale_counts > 0U && isfinite(acquisition->current_a_per_count) &&
        acquisition->current_a_per_count > 0.0f &&
        acquisition->encoder_counts_per_revolution >= 2U &&
        acquisition->encoder_counts_per_revolution <= 65536U &&
        acquisition->encoder_direction >= -1 && acquisition->encoder_direction <= 1 &&
        acquisition->pole_pairs <= FOC_BOOTSTRAP_MAX_POLE_PAIRS;
}

static bool bootstrap_positive(float value)
{
    return isfinite(value) && value > 0.0f;
}

static bool bootstrap_config_valid(const struct foc_bootstrap_config *config)
{
    return config != NULL && bootstrap_identity_valid(&config->identity) && config->capture > 0U &&
        (config->source == FOC_CAL_WORKFLOW_HOST_SYNTHETIC ||
         config->source == FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT ||
         config->source == FOC_CAL_WORKFLOW_EXTERNAL_RECORDED_DATA) &&
        config->minimum_pole_pairs >= 1U &&
        config->maximum_pole_pairs >= config->minimum_pole_pairs &&
        config->maximum_pole_pairs <= FOC_BOOTSTRAP_MAX_POLE_PAIRS &&
        (config->expected_pole_pairs == 0U ||
         (config->expected_pole_pairs >= config->minimum_pole_pairs &&
          config->expected_pole_pairs <= config->maximum_pole_pairs)) &&
        (config->identity.acquisition.pole_pairs == 0U || config->expected_pole_pairs == 0U ||
         config->identity.acquisition.pole_pairs == config->expected_pole_pairs) &&
        config->expected_encoder_direction >= -1 && config->expected_encoder_direction <= 1 &&
        config->minimum_samples_per_sweep >= 4U &&
        config->minimum_samples_per_sweep <= FOC_CALIBRATION_MAX_REFERENCE_SAMPLES &&
        config->maximum_interval_us > 0U &&
        config->maximum_interval_us < FOC_BOOTSTRAP_HALF_RANGE &&
        bootstrap_positive(config->minimum_mechanical_span_rad) &&
        bootstrap_positive(config->maximum_residual_rad) &&
        config->maximum_residual_rad < FOC_BOOTSTRAP_PI &&
        bootstrap_positive(config->minimum_candidate_separation_rad) &&
        config->minimum_candidate_separation_rad < FOC_BOOTSTRAP_PI &&
        bootstrap_positive(config->maximum_offset_disagreement_rad) &&
        config->maximum_offset_disagreement_rad < 0.5f * FOC_BOOTSTRAP_PI &&
        bootstrap_positive(config->minimum_emf_v) &&
        bootstrap_positive(config->minimum_abs_speed_rad_s) &&
        bootstrap_positive(config->maximum_abs_speed_rad_s) &&
        config->maximum_abs_speed_rad_s >= config->minimum_abs_speed_rad_s &&
        isfinite(config->maximum_motion_error_rad) && config->maximum_motion_error_rad >= 0.0f;
}

static bool bootstrap_newer(uint32_t value, uint32_t previous)
{
    const uint32_t delta = value - previous;
    return delta > 0U && delta < FOC_BOOTSTRAP_HALF_RANGE;
}

static int32_t bootstrap_raw_step(uint32_t code, uint32_t previous, int32_t cpr)
{
    int32_t delta = (int32_t)code - (int32_t)previous;
    if (2 * delta > cpr) {
        delta -= cpr;
    } else if (2 * delta < -cpr) {
        delta += cpr;
    }
    return delta;
}

static enum foc_bootstrap_status bootstrap_fit_sweep(
    const struct foc_bootstrap_config *config,
    const struct foc_calibration_rotor_config *rotor_config,
    const struct foc_bootstrap_sample *samples,
    uint32_t count,
    struct foc_calibration_rotor_result *fit
)
{
    if (count < config->minimum_samples_per_sweep) {
        return FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION;
    }
    if (count > rotor_config->maximum_samples) {
        return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
    }
    struct foc_calibration_rotor_context rotor;
    if (foc_calibration_rotor_init(&rotor, rotor_config) != 0) {
        return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
    }

    const int32_t cpr = (int32_t)config->identity.acquisition.encoder_counts_per_revolution;
    const float rad_per_count = FOC_BOOTSTRAP_TWO_PI / (float)cpr;
    const float motion_tolerance = config->maximum_motion_error_rad + rad_per_count;
    const float alias_step = FOC_BOOTSTRAP_PI / (float)config->maximum_pole_pairs;
    const int32_t shaft_sign = samples[0].shaft_rad_s > 0.0f ? 1 : -1;
    const int32_t first_count = (int32_t)samples[0].raw_code;
    int32_t unwrapped_count = first_count;
    int32_t raw_sign = 0;

    for (uint32_t index = 0U; index < count; ++index) {
        const struct foc_bootstrap_sample *sample = &samples[index];
        const float speed = fabsf(sample->shaft_rad_s);
        const float magnitude = hypotf(sample->emf_alpha_v, sample->emf_beta_v);
        if (!isfinite(magnitude) || speed > config->maximum_abs_speed_rad_s) {
            return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
        }
        if (magnitude < config->minimum_emf_v || speed < config->minimum_abs_speed_rad_s ||
            (float)shaft_sign * sample->shaft_rad_s <= 0.0f) {
            return FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION;
        }

        if (index > 0U) {
            const struct foc_bootstrap_sample *previous = &samples[index - 1U];
            const uint32_t interval_us = sample->captured_us - previous->captured_us;
            if (interval_us > config->maximum_interval_us) {
                return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
            }
            const float dt_s = (float)interval_us * 1.0e-6f;
            const float previous_speed = fabsf(previous->shaft_rad_s);
            const float upper_motion = fmaxf(speed, previous_speed) * dt_s + motion_tolerance;
            const int32_t delta = bootstrap_raw_step(sample->raw_code, previous->raw_code, cpr);
            const float raw_motion = fabsf((float)delta * rad_per_count);
            if (config->maximum_abs_speed_rad_s * dt_s + rad_per_count >= FOC_BOOTSTRAP_PI ||
                upper_motion >= alias_step || raw_motion >= alias_step) {
                return FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE;
            }
            const float expected_motion = 0.5f * (speed + previous_speed) * dt_s;
            if (fabsf(raw_motion - expected_motion) > motion_tolerance ||
                (delta != 0 && raw_sign != 0 && (delta > 0) != (raw_sign > 0))) {
                return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
            }
            if (delta != 0) {
                raw_sign = delta > 0 ? 1 : -1;
            }
            unwrapped_count += delta;
        }

        const struct foc_calibration_angle_sample angle = {
            .mechanical_rad = (float)unwrapped_count * rad_per_count,
            .electrical_rad = foc_bootstrap_reference_angle(sample),
            .captured_us = sample->captured_us,
            .sequence = sample->sequence,
            .valid = true,
        };
        if (foc_calibration_rotor_collect(&rotor, &angle) != 0) {
            return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
        }
    }

    const float span = fabsf((float)(unwrapped_count - first_count) * rad_per_count);
    if (span < config->minimum_mechanical_span_rad) {
        return FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION;
    }
    if (foc_calibration_rotor_finalize(&rotor, fit) != 0 || !fit->identifiable ||
        fit->direction != shaft_sign * raw_sign ||
        fit->residual_max_rad > config->maximum_residual_rad) {
        return FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE;
    }
    return FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED;
}

/* Public Functions ------------------------------------------------------------- */

enum foc_bootstrap_status foc_bootstrap_qualify(
    const struct foc_cal_workflow_identity *expected_identity,
    const struct foc_cal_workflow_identity *observed_identity,
    const struct foc_bootstrap_prerequisites *prerequisites,
    const struct foc_bootstrap_trusted_reference *trusted_reference
)
{
    if (prerequisites == NULL) {
        return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
    }
    if (!prerequisites->safety_permitted) {
        return FOC_BOOTSTRAP_SAFETY_BLOCKED;
    }
    if (!bootstrap_identity_valid(expected_identity) ||
        !bootstrap_identity_valid(observed_identity) ||
        !prerequisites->current_mapping_and_zero_verified ||
        !prerequisites->phase_and_direction_verified ||
        !prerequisites->encoder_and_clock_verified || !prerequisites->bus_and_limits_verified ||
        !prerequisites->acquisition_verified) {
        return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
    }
    if (!foc_cal_workflow_identity_matches(expected_identity, observed_identity) ||
        (trusted_reference != NULL &&
         !foc_cal_workflow_identity_matches(expected_identity, &trusted_reference->identity))) {
        return FOC_BOOTSTRAP_IDENTITY_MISMATCH;
    }
    if (trusted_reference == NULL) {
        return FOC_BOOTSTRAP_REFERENCE_ACQUISITION_REQUIRED;
    }
    if (trusted_reference->pole_pairs == 0U ||
        trusted_reference->pole_pairs > FOC_BOOTSTRAP_MAX_POLE_PAIRS ||
        (trusted_reference->encoder_direction != -1 && trusted_reference->encoder_direction != 1) ||
        !isfinite(trusted_reference->electrical_offset_rad) ||
        trusted_reference->electrical_offset_rad < 0.0f ||
        trusted_reference->electrical_offset_rad >= FOC_BOOTSTRAP_TWO_PI) {
        return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
    }
    if (expected_identity->acquisition.pole_pairs != 0U &&
        expected_identity->acquisition.pole_pairs != trusted_reference->pole_pairs) {
        return FOC_BOOTSTRAP_IDENTITY_MISMATCH;
    }
    return trusted_reference->reviewed ? FOC_BOOTSTRAP_READY_WITH_TRUSTED_REFERENCE
                                       : FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED;
}

float foc_bootstrap_reference_angle(const struct foc_bootstrap_sample *sample)
{
    if (sample == NULL || !sample->valid || !sample->output_inhibited ||
        !isfinite(sample->shaft_rad_s) || sample->shaft_rad_s == 0.0f ||
        !isfinite(sample->emf_alpha_v) || !isfinite(sample->emf_beta_v) ||
        (sample->emf_alpha_v == 0.0f && sample->emf_beta_v == 0.0f)) {
        return NAN;
    }
    const float shaft_sign = sample->shaft_rad_s > 0.0f ? 1.0f : -1.0f;
    return atan2f(-shaft_sign * sample->emf_alpha_v, shaft_sign * sample->emf_beta_v);
}

enum foc_bootstrap_status foc_bootstrap_fit(
    const struct foc_bootstrap_config *config,
    const struct foc_bootstrap_sample *samples,
    uint32_t sample_count,
    struct foc_bootstrap_result *result
)
{
    if (result == NULL) {
        return FOC_BOOTSTRAP_MEASUREMENT_INVALID;
    }
    *result = (struct foc_bootstrap_result){.status = FOC_BOOTSTRAP_MEASUREMENT_INVALID};
    if (!bootstrap_config_valid(config) || samples == NULL ||
        sample_count > FOC_BOOTSTRAP_SWEEPS * FOC_CALIBRATION_MAX_REFERENCE_SAMPLES) {
        return result->status;
    }
    result->identity = config->identity;
    result->capture = config->capture;
    result->source = config->source;

    uint32_t sweep_count[FOC_BOOTSTRAP_SWEEPS] = {0U};
    uint32_t sweep = 0U;
    for (uint32_t index = 0U; index < sample_count; ++index) {
        const struct foc_bootstrap_sample *sample = &samples[index];
        if (!sample->output_inhibited) {
            return result->status = FOC_BOOTSTRAP_SAFETY_BLOCKED;
        }
        if (sample->capture != config->capture ||
            !foc_cal_workflow_identity_matches(&config->identity, &sample->identity)) {
            return result->status = FOC_BOOTSTRAP_IDENTITY_MISMATCH;
        }
        if (!sample->valid ||
            sample->raw_code >= config->identity.acquisition.encoder_counts_per_revolution ||
            !isfinite(sample->shaft_rad_s) || !isfinite(sample->emf_alpha_v) ||
            !isfinite(sample->emf_beta_v) || sample->sweep >= FOC_BOOTSTRAP_SWEEPS) {
            return result->status;
        }
        if (index > 0U &&
            (!bootstrap_newer(sample->sequence, samples[index - 1U].sequence) ||
             !bootstrap_newer(sample->captured_us, samples[index - 1U].captured_us))) {
            return result->status;
        }
        if (sample->sweep == sweep + 1U && sweep_count[sweep] > 0U) {
            ++sweep;
        }
        if (sample->sweep != sweep) {
            return result->status;
        }
        ++sweep_count[sweep];
    }
    if (sweep + 1U != FOC_BOOTSTRAP_SWEEPS) {
        return result->status = FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION;
    }

    const struct foc_calibration_rotor_config rotor_config = {
        .identity = config->identity.acquisition,
        .minimum_pole_pairs = config->minimum_pole_pairs,
        .maximum_pole_pairs = config->maximum_pole_pairs,
        .minimum_mechanical_span_rad = config->minimum_mechanical_span_rad,
        .maximum_residual_rad = config->maximum_residual_rad,
        .minimum_candidate_separation_rad = config->minimum_candidate_separation_rad,
        .maximum_interval_us = config->maximum_interval_us,
        .maximum_samples = FOC_CALIBRATION_MAX_REFERENCE_SAMPLES,
    };
    const uint16_t expected_poles = config->expected_pole_pairs != 0U
        ? config->expected_pole_pairs
        : config->identity.acquisition.pole_pairs;
    struct foc_calibration_rotor_result fits[FOC_BOOTSTRAP_SWEEPS];
    uint32_t start = 0U;
    const bool first_positive = samples[0].shaft_rad_s > 0.0f;
    float sum_sine = 0.0f;
    float sum_cosine = 0.0f;
    float max_residual = 0.0f;
    float min_separation = FLT_MAX;
    float min_span = FLT_MAX;
    float max_disagreement = 0.0f;
    for (sweep = 0U; sweep < FOC_BOOTSTRAP_SWEEPS; ++sweep) {
        const bool positive = samples[start].shaft_rad_s > 0.0f;
        if ((positive == first_positive) != (sweep % 2U == 0U)) {
            return result->status = FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION;
        }
        struct foc_calibration_rotor_result *fit = &fits[sweep];
        const enum foc_bootstrap_status status =
            bootstrap_fit_sweep(config, &rotor_config, &samples[start], sweep_count[sweep], fit);
        if (status != FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED) {
            return result->status = status;
        }
        if ((expected_poles != 0U && fit->pole_pairs != expected_poles) ||
            (config->expected_encoder_direction != 0 &&
             fit->direction != config->expected_encoder_direction) ||
            fit->pole_pairs != fits[0].pole_pairs || fit->direction != fits[0].direction) {
            return result->status = FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE;
        }
        for (uint32_t index = 0U; index < sweep; ++index) {
            const float delta = fit->electrical_offset_rad - fits[index].electrical_offset_rad;
            const float disagreement = fabsf(remainderf(delta, FOC_BOOTSTRAP_TWO_PI));
            max_disagreement = fmaxf(max_disagreement, disagreement);
        }
        if (max_disagreement > config->maximum_offset_disagreement_rad) {
            return result->status = FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE;
        }
        sum_sine += sinf(fit->electrical_offset_rad);
        sum_cosine += cosf(fit->electrical_offset_rad);
        max_residual = fmaxf(max_residual, fit->residual_max_rad);
        min_separation = fminf(min_separation, fit->candidate_separation_rad);
        min_span = fminf(min_span, fit->mechanical_span_rad);
        start += sweep_count[sweep];
    }

    const float offset = atan2f(sum_sine, sum_cosine);
    const float positive_offset = offset < 0.0f ? offset + FOC_BOOTSTRAP_TWO_PI : offset;
    result->electrical_offset_rad =
        positive_offset >= FOC_BOOTSTRAP_TWO_PI ? 0.0f : positive_offset;
    result->encoder_direction = fits[0].direction;
    result->pole_pairs = fits[0].pole_pairs;
    result->maximum_residual_rad = max_residual;
    result->minimum_candidate_separation_rad = min_separation;
    result->minimum_mechanical_span_rad = min_span;
    result->maximum_offset_disagreement_rad = max_disagreement;
    result->quality = FOC_BOOTSTRAP_COARSE_ELECTRICAL_REFERENCE;
    return result->status = FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED;
}

/* ---------------------------------------- end of file ----------------------------------------- */
