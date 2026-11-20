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
#include "foc_core_internal.h"
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

static struct foc_vector_alpha_beta
foc_current_duty_voltage(const struct foc_svpwm_output *duty, float vbus_v)
{
    const float ab = duty->duty_a - duty->duty_b;
    const float ac = duty->duty_a - duty->duty_c;
    const float bc = duty->duty_b - duty->duty_c;

    return (struct foc_vector_alpha_beta){
        ((ab + ac) / 3.0f) * vbus_v,
        (bc * FOC_CURRENT_INV_SQRT3) * vbus_v,
    };
}

static int foc_current_modulate(
    const struct foc_current_config *config,
    const struct foc_current_input *input,
    const struct foc_angle_cache *angle_cache,
    struct foc_vector_dq requested_v,
    float voltage_limit_v,
    bool *voltage_repaired,
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
    status = foc_inverse_park_cached(&requested_v, angle_cache, &voltage);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    status = foc_svpwm_trusted(&voltage, input->vbus_v, &config->pwm, &result->svpwm);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    result->svpwm.saturated |= limited;

    /* Subtract the floating neutral before multiplying by bus voltage. */
    voltage = foc_current_duty_voltage(&result->svpwm, input->vbus_v);
    status = foc_park_cached(&voltage, angle_cache, &result->voltage_dq_v);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    /* Permit float roundoff at the envelope, but reject materially coarse duties. */
    const float voltage_magnitude = hypotf(voltage.alpha, voltage.beta);
    const float rounding_slack = 16.0f * FLT_EPSILON * voltage_limit_v;
    const float excess_v = voltage_magnitude - voltage_limit_v;
    if (excess_v > (*voltage_repaired ? -rounding_slack : rounding_slack)) {
        /* Near 50% duty the rounding error scales with VBUS, even at low modulation. */
        const float duty_rounding_v = 2.0f * FLT_EPSILON * input->vbus_v;
        /* Refuse coarse duty resolution. The backoff budget must be <= 2^-13 of the limit. */
        if (excess_v > duty_rounding_v || duty_rounding_v > 64.0f * rounding_slack) {
            return FOC_STATUS_ERR_VOLTAGE_LIMIT;
        }

        /* Reduce phase differences inside the existing duty bounds and recheck the result. */
        struct foc_svpwm_output *const duty = &result->svpwm;
        const float minimum = fminf(duty->duty_a, fminf(duty->duty_b, duty->duty_c));
        const float scale = (voltage_limit_v - duty_rounding_v) / voltage_magnitude;
        duty->duty_a = minimum + (duty->duty_a - minimum) * scale;
        duty->duty_b = minimum + (duty->duty_b - minimum) * scale;
        duty->duty_c = minimum + (duty->duty_c - minimum) * scale;
        duty->saturated = 1U;
        voltage = foc_current_duty_voltage(duty, input->vbus_v);
        if (hypotf(voltage.alpha, voltage.beta) > voltage_limit_v - rounding_slack) {
            return FOC_STATUS_ERR_VOLTAGE_LIMIT;
        }
        /* Preserve the inward voltage margin if anti-windup remodulates this step. */
        *voltage_repaired = true;
        return foc_park_cached(&voltage, angle_cache, &result->voltage_dq_v);
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

static int foc_current_feedforward_model_valid(const struct foc_current_feedforward_model *model)
{
    return model != NULL && isfinite(model->ld_h) && isfinite(model->lq_h) &&
        isfinite(model->psi_f_wb) && model->ld_h > 0.0f && model->lq_h > 0.0f &&
        model->psi_f_wb >= 0.0f;
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

int foc_current_feedforward_init(
    struct foc_current_feedforward_context *context,
    const struct foc_current_feedforward_model *model,
    bool enabled
)
{
    if (context == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (enabled && !foc_current_feedforward_model_valid(model)) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }
    *context = (struct foc_current_feedforward_context){
        .model = model != NULL ? *model : (struct foc_current_feedforward_model){0},
        .enabled = enabled,
        .initialized = true,
    };
    return FOC_STATUS_OK;
}

int foc_current_feedforward_reset(struct foc_current_feedforward_context *context)
{
    if (context == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_STATUS_ERR_NOT_INITIALIZED;
    }
    return FOC_STATUS_OK;
}

int foc_current_feedforward_compute(
    const struct foc_current_feedforward_context *context,
    float omega_e_rad_s,
    struct foc_vector_dq target_a,
    struct foc_current_feedforward_output *output
)
{
    if (output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *output = (struct foc_current_feedforward_output){0};
    if (context == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_STATUS_ERR_NOT_INITIALIZED;
    }
    if (!context->enabled) {
        return FOC_STATUS_OK;
    }
    if (!isfinite(omega_e_rad_s) || !foc_current_feedforward_model_valid(&context->model) ||
        !foc_current_dq_finite(target_a)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    output->voltage_d_v = -omega_e_rad_s * context->model.lq_h * target_a.q;
    output->voltage_q_v =
        omega_e_rad_s * (context->model.ld_h * target_a.d + context->model.psi_f_wb);
    if (!isfinite(output->voltage_d_v) || !isfinite(output->voltage_q_v)) {
        *output = (struct foc_current_feedforward_output){0};
        return FOC_STATUS_ERR_NONFINITE;
    }
    output->enabled = true;
    return FOC_STATUS_OK;
}

static int foc_current_step_internal(
    struct foc_current_context *context,
    const struct foc_current_input *input,
    const struct foc_current_feedforward_context *feedforward,
    float omega_e_rad_s,
    struct foc_current_output *output
)
{
    struct foc_current_output result = {0};
    struct foc_vector_alpha_beta current;
    struct foc_vector_dq target;
    struct foc_angle_cache angle_cache;
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
    status = foc_angle_cache_prepare(input->theta_elec_rad, &angle_cache);
    if (status != FOC_STATUS_OK) {
        return status;
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
    status = foc_park_cached(&current, &angle_cache, &result.measured_dq_a);
    if (status != FOC_STATUS_OK) {
        return status;
    }

    target = input->target_a;
    result.target_limited = foc_current_limit_vector(&target, config->current_limit_a);
    struct foc_vector_dq feedforward_voltage = {0};
    bool feedforward_enabled = false;
    if (feedforward != NULL && feedforward->enabled) {
        struct foc_current_feedforward_output feedforward_output;

        status = foc_current_feedforward_compute(
            feedforward,
            omega_e_rad_s,
            target,
            &feedforward_output
        );
        if (status != FOC_STATUS_OK) {
            return status;
        }
        feedforward_voltage = (struct foc_vector_dq){
            feedforward_output.voltage_d_v,
            feedforward_output.voltage_q_v,
        };
        feedforward_enabled = feedforward_output.enabled;
    }
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
    struct foc_vector_dq requested = {proportional.d + integral.d, proportional.q + integral.q};
    if (feedforward_enabled) {
        requested.d += feedforward_voltage.d;
        requested.q += feedforward_voltage.q;
    }
    bool voltage_repaired = false;
    status = foc_current_modulate(
        config,
        input,
        &angle_cache,
        requested,
        voltage_limit_v,
        &voltage_repaired,
        &result
    );
    if (status != FOC_STATUS_OK) {
        return status;
    }
    if (result.svpwm.saturated != 0U) {
        const struct foc_vector_dq proportional_total = feedforward_enabled
            ? (struct foc_vector_dq){
                  proportional.d + feedforward_voltage.d,
                  proportional.q + feedforward_voltage.q,
              }
            : proportional;
        const struct foc_vector_dq admitted = {
            foc_current_admit_integral(
                previous.d,
                integral.d,
                proportional_total.d,
                result.voltage_dq_v.d
            ),
            foc_current_admit_integral(
                previous.q,
                integral.q,
                proportional_total.q,
                result.voltage_dq_v.q
            )
        };

        if (admitted.d != integral.d || admitted.q != integral.q) {
            const struct foc_vector_dq corrected = {
                proportional_total.d + admitted.d,
                proportional_total.q + admitted.q
            };

            status = foc_current_modulate(
                config,
                input,
                &angle_cache,
                corrected,
                voltage_limit_v,
                &voltage_repaired,
                &result
            );
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

int foc_current_step(
    struct foc_current_context *context,
    const struct foc_current_input *input,
    struct foc_current_output *output
)
{
    return foc_current_step_internal(context, input, NULL, 0.0f, output);
}

int foc_current_step_with_feedforward(
    struct foc_current_context *context,
    const struct foc_current_input *input,
    const struct foc_current_feedforward_context *feedforward,
    float omega_e_rad_s,
    struct foc_current_output *output
)
{
    if (feedforward == NULL) {
        return foc_current_step(context, input, output);
    }
    if (!feedforward->initialized) {
        if (output != NULL) {
            *output = (struct foc_current_output){0};
        }
        return FOC_STATUS_ERR_NOT_INITIALIZED;
    }
    return foc_current_step_internal(context, input, feedforward, omega_e_rad_s, output);
}

/* ---------------------------------------- end of file ----------------------------------------- */
