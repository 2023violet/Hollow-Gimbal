/**
 * @file    foc_current.c
 * @brief   Portable d/q current PI and transactional modulation step.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Owns current-loop PI state and feeds the existing transforms and SVPWM.
 * Integral admission uses the ideal voltage reconstructed after duty limiting.
 * All calculations precede the single state commit at the end of the step.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_current.h"
#include <float.h>
#include <math.h>

/* Private Constants ------------------------------------------------------------ */

static const float FOC_CURRENT_INV_SQRT3 = 0.57735026918962576451f;

/* Private Helpers -------------------------------------------------------------- */

static int foc_current_dq_finite(struct foc_vector_dq vector)
{
    return isfinite(vector.d) && isfinite(vector.q);
}

static unsigned int foc_current_limit_vector(struct foc_vector_dq *vector, float limit)
{
    const float largest = fmaxf(fabsf(vector->d), fabsf(vector->q));

    if (largest > 0.0f) {
        const float d = vector->d / largest;
        const float q = vector->q / largest;
        const float unit_length = sqrtf(d * d + q * q);
        const float allowed = limit / unit_length;

        if (largest > allowed) {
            vector->d = d * allowed;
            vector->q = q * allowed;
            return 1U;
        }
    }
    return 0U;
}

static int foc_current_modulate(
    const struct foc_config *config,
    const struct foc_current_input *input,
    struct foc_vector_dq requested_v,
    float voltage_limit_v,
    struct foc_current_output *result
)
{
    struct foc_vector_alpha_beta voltage;
    unsigned int limited;
    int status;

    if (!foc_current_dq_finite(requested_v)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    limited = foc_current_limit_vector(&requested_v, voltage_limit_v);
    status = foc_inverse_park(&requested_v, input->theta_elec_rad, &voltage);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    status = foc_svpwm(&voltage, input->vbus_v, config, &result->svpwm);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    result->svpwm.saturated |= limited;

    /* Subtract the floating neutral before multiplying by bus voltage. */
    const float a = result->svpwm.duty_a;
    const float b = result->svpwm.duty_b;
    const float c = result->svpwm.duty_c;
    voltage.alpha = ((a - b) + (a - c)) / 3.0f * input->vbus_v;
    voltage.beta = (b - c) * FOC_CURRENT_INV_SQRT3 * input->vbus_v;
    status = foc_park(&voltage, input->theta_elec_rad, &result->voltage_dq_v);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    /* Permit float roundoff at the envelope, but reject materially coarse duties. */
    const float voltage_magnitude = hypotf(voltage.alpha, voltage.beta);
    const float rounding_slack = 16.0f * FLT_EPSILON * voltage_limit_v;
    if (voltage_magnitude - voltage_limit_v > rounding_slack) {
        return FOC_STATUS_ERR_VOLTAGE_LIMIT;
    }
    return FOC_STATUS_OK;
}

static float
foc_current_admit_integral(float previous, float trial, float proportional, float applied)
{
    /* Limit the increment to achievable voltage without moving past the old integral. */
    if (trial > previous) {
        return fmaxf(previous, fminf(trial, applied - proportional));
    }
    if (trial < previous) {
        return fminf(previous, fmaxf(trial, applied - proportional));
    }
    return previous;
}

/* Public Functions ------------------------------------------------------------- */

int foc_current_init(struct foc_current_context *context, const struct foc_current_config *config)
{
    if (context == NULL || config == NULL) {
        return FOC_STATUS_ERR_NULL;
    }

    const int status = foc_config_validate(&config->pwm);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    if (!foc_current_dq_finite(config->kp_v_per_a) ||
        !foc_current_dq_finite(config->ki_v_per_a_s) || !isfinite(config->current_limit_a)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    if (config->kp_v_per_a.d < 0.0f || config->kp_v_per_a.q < 0.0f ||
        config->ki_v_per_a_s.d < 0.0f || config->ki_v_per_a_s.q < 0.0f ||
        config->current_limit_a <= 0.0f || config->pwm.duty_min >= 0.5f ||
        config->pwm.duty_max <= 0.5f) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }

    *context = (struct foc_current_context){.config = *config, .initialized = 1U};
    return FOC_STATUS_OK;
}

int foc_current_reset(struct foc_current_context *context)
{
    if (context == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_STATUS_ERR_NOT_INITIALIZED;
    }

    context->integral_v = (struct foc_vector_dq){0};
    return FOC_STATUS_OK;
}

int foc_current_step(
    struct foc_current_context *context,
    const struct foc_current_input *input,
    struct foc_current_output *output
)
{
    struct foc_current_output result = {0};
    struct foc_vector_alpha_beta current;
    struct foc_vector_dq target;
    int status;

    if (output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *output = result;
    if (context == NULL || input == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_STATUS_ERR_NOT_INITIALIZED;
    }
    const struct foc_current_config *config = &context->config;
    if (!isfinite(input->dt_s) || input->dt_s <= 0.0f || input->dt_s > config->pwm.max_dt_s) {
        return FOC_STATUS_ERR_INVALID_DT;
    }
    if (!isfinite(input->vbus_v) || input->vbus_v < config->pwm.vbus_min_v) {
        return FOC_STATUS_ERR_INVALID_VBUS;
    }
    if (!foc_current_dq_finite(input->target_a)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    /* The maximum phase spread is sqrt(3) times the vector magnitude. */
    const float voltage_limit_v =
        fminf(config->pwm.voltage_limit_v, input->vbus_v * FOC_CURRENT_INV_SQRT3);
    if (voltage_limit_v <= 0.0f) {
        return FOC_STATUS_ERR_INVALID_VBUS;
    }

    status = foc_clarke(&input->phase_current_a, &current);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    status = foc_park(&current, input->theta_elec_rad, &result.measured_dq_a);
    if (status != FOC_STATUS_OK) {
        return status;
    }

    target = input->target_a;
    result.target_limited = foc_current_limit_vector(&target, config->current_limit_a);
    const struct foc_vector_dq error = {
        target.d - result.measured_dq_a.d,
        target.q - result.measured_dq_a.q
    };
    const struct foc_vector_dq proportional = {
        config->kp_v_per_a.d * error.d,
        config->kp_v_per_a.q * error.q
    };
    const struct foc_vector_dq previous = context->integral_v;
    struct foc_vector_dq integral = {
        previous.d + config->ki_v_per_a_s.d * input->dt_s * error.d,
        previous.q + config->ki_v_per_a_s.q * input->dt_s * error.q
    };
    if (!foc_current_dq_finite(integral)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    const struct foc_vector_dq requested = {
        proportional.d + integral.d,
        proportional.q + integral.q
    };
    status = foc_current_modulate(&config->pwm, input, requested, voltage_limit_v, &result);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    if (result.svpwm.saturated != 0U) {
        const struct foc_vector_dq admitted = {
            foc_current_admit_integral(
                previous.d,
                integral.d,
                proportional.d,
                result.voltage_dq_v.d
            ),
            foc_current_admit_integral(
                previous.q,
                integral.q,
                proportional.q,
                result.voltage_dq_v.q
            )
        };

        if (admitted.d != integral.d || admitted.q != integral.q) {
            const struct foc_vector_dq corrected = {
                proportional.d + admitted.d,
                proportional.q + admitted.q
            };

            status = foc_current_modulate(&config->pwm, input, corrected, voltage_limit_v, &result);
            if (status != FOC_STATUS_OK) {
                return status;
            }
            result.svpwm.saturated = 1U;
            integral = admitted;
        }
    }

    result.output_enable = 1U;
    context->integral_v = integral;
    *output = result;
    return FOC_STATUS_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
