/**
 * @file    foc_core.c
 * @brief   Host-portable voltage-open-loop FOC control-core implementation.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_core.h` and owns only mathematical Core behavior
 * - keeps all intermediate results local until validation succeeds
 * - leaves ADC, PWM, sensor, safety, transport, and scheduling ownership to
 *   the caller's later Device and Runtime layers
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_core.h"

#include <math.h>

/* Private Macros and Constants ------------------------------------------------- */

static const float FOC_PI = 3.14159265358979323846f;
static const float FOC_TWO_PI = 6.28318530717958647692f;
static const float FOC_INV_SQRT3 = 0.57735026918962576451f;
static const float FOC_SQRT3_OVER_TWO = 0.86602540378443864676f;
static const float FOC_SVPWM_VECTOR_LIMIT = 0.66666666666666666667f;

/* Private Helpers -------------------------------------------------------------- */

static int foc_float_is_finite(float value)
{
    return isfinite(value) != 0;
}

static float foc_wrap_angle(float angle)
{
    float wrapped = fmodf(angle, FOC_TWO_PI);

    if (wrapped < 0.0f) {
        wrapped += FOC_TWO_PI;
    }

    return wrapped;
}

static void foc_set_safe_svpwm(struct foc_svpwm_output *output)
{
    /* Centered values are diagnostic metadata. Device owns bridge inhibition. */
    output->duty_a = 0.5f;
    output->duty_b = 0.5f;
    output->duty_c = 0.5f;
    output->sector = 0U;
    output->saturated = 1U;
}

static void foc_set_safe_output(struct foc_output *output, float theta_elec_rad)
{
    output->output_enable = 0U;
    output->theta_elec_rad = theta_elec_rad;
    output->voltage_alpha_beta_v.alpha = 0.0f;
    output->voltage_alpha_beta_v.beta = 0.0f;
    foc_set_safe_svpwm(&output->svpwm);
}

/* Public Functions ------------------------------------------------------------- */

int foc_angle_normalize(float angle_rad, float *wrapped_rad)
{
    if (wrapped_rad == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *wrapped_rad = 0.0f;
    if (!foc_float_is_finite(angle_rad)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    const float wrapped = foc_wrap_angle(angle_rad);
    /* A tiny negative angle can round up to one full turn. */
    *wrapped_rad = wrapped >= FOC_TWO_PI ? 0.0f : wrapped;
    return FOC_STATUS_OK;
}

int foc_config_validate(const struct foc_config *config)
{
    if (config == NULL) {
        return FOC_STATUS_ERR_NULL;
    }

    if (!foc_float_is_finite(config->vbus_min_v) || !foc_float_is_finite(config->voltage_limit_v) ||
        !foc_float_is_finite(config->duty_min) || !foc_float_is_finite(config->duty_max) ||
        !foc_float_is_finite(config->max_dt_s)) {
        return FOC_STATUS_ERR_NONFINITE;
    }

    if (config->vbus_min_v <= 0.0f || config->voltage_limit_v <= 0.0f || config->max_dt_s <= 0.0f ||
        config->duty_min < 0.0f || config->duty_max > 1.0f ||
        config->duty_min >= config->duty_max) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }

    return FOC_STATUS_OK;
}

int foc_clarke(const struct foc_vector_abc *input, struct foc_vector_alpha_beta *output)
{
    struct foc_vector_alpha_beta result;

    if (input == NULL || output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }

    if (!foc_float_is_finite(input->a) || !foc_float_is_finite(input->b) ||
        !foc_float_is_finite(input->c)) {
        return FOC_STATUS_ERR_NONFINITE;
    }

    result.alpha = input->a;
    result.beta = (input->b - input->c) * FOC_INV_SQRT3;
    if (!foc_float_is_finite(result.alpha) || !foc_float_is_finite(result.beta)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    *output = result;
    return FOC_STATUS_OK;
}

int foc_park(
    const struct foc_vector_alpha_beta *input,
    float theta_elec_rad,
    struct foc_vector_dq *output
)
{
    float sine;
    float cosine;
    struct foc_vector_dq result;

    if (input == NULL || output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }

    if (!foc_float_is_finite(input->alpha) || !foc_float_is_finite(input->beta) ||
        !foc_float_is_finite(theta_elec_rad)) {
        return FOC_STATUS_ERR_NONFINITE;
    }

    theta_elec_rad = foc_wrap_angle(theta_elec_rad);
    sine = sinf(theta_elec_rad);
    cosine = cosf(theta_elec_rad);
    result.d = input->alpha * cosine + input->beta * sine;
    result.q = input->beta * cosine - input->alpha * sine;
    if (!foc_float_is_finite(result.d) || !foc_float_is_finite(result.q)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    *output = result;
    return FOC_STATUS_OK;
}

int foc_inverse_park(
    const struct foc_vector_dq *input,
    float theta_elec_rad,
    struct foc_vector_alpha_beta *output
)
{
    float sine;
    float cosine;
    struct foc_vector_alpha_beta result;

    if (input == NULL || output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }

    if (!foc_float_is_finite(input->d) || !foc_float_is_finite(input->q) ||
        !foc_float_is_finite(theta_elec_rad)) {
        return FOC_STATUS_ERR_NONFINITE;
    }

    theta_elec_rad = foc_wrap_angle(theta_elec_rad);
    sine = sinf(theta_elec_rad);
    cosine = cosf(theta_elec_rad);
    result.alpha = input->d * cosine - input->q * sine;
    result.beta = input->d * sine + input->q * cosine;
    if (!foc_float_is_finite(result.alpha) || !foc_float_is_finite(result.beta)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    *output = result;
    return FOC_STATUS_OK;
}

int foc_svpwm(
    const struct foc_vector_alpha_beta *input,
    float vbus_v,
    const struct foc_config *config,
    struct foc_svpwm_output *output
)
{
    float alpha_normalized;
    float beta_normalized;
    float magnitude;
    float vector_limit;
    float phase_a;
    float phase_b;
    float phase_c;
    float phase_max;
    float phase_min;
    float common_mode;
    float duty_a;
    float duty_b;
    float duty_c;
    float angle;
    unsigned int sector;
    unsigned int saturated = 0U;
    struct foc_svpwm_output result;

    if (output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    foc_set_safe_svpwm(output);

    if (input == NULL || config == NULL) {
        return FOC_STATUS_ERR_NULL;
    }

    {
        const int config_status = foc_config_validate(config);

        if (config_status != FOC_STATUS_OK) {
            return config_status;
        }
    }

    if (!foc_float_is_finite(vbus_v) || vbus_v < config->vbus_min_v) {
        return FOC_STATUS_ERR_INVALID_VBUS;
    }

    if (!foc_float_is_finite(input->alpha) || !foc_float_is_finite(input->beta)) {
        return FOC_STATUS_ERR_NONFINITE;
    }

    alpha_normalized = input->alpha / vbus_v;
    beta_normalized = input->beta / vbus_v;
    if (!foc_float_is_finite(alpha_normalized) || !foc_float_is_finite(beta_normalized)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    magnitude = sqrtf(alpha_normalized * alpha_normalized + beta_normalized * beta_normalized);
    if (!foc_float_is_finite(magnitude)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    /* Keep the physical voltage limit below the linear SVPWM vector envelope. */
    vector_limit = config->voltage_limit_v / vbus_v;
    if (vector_limit > FOC_SVPWM_VECTOR_LIMIT) {
        vector_limit = FOC_SVPWM_VECTOR_LIMIT;
    }

    if (magnitude > vector_limit) {
        const float scale = vector_limit / magnitude;

        alpha_normalized *= scale;
        beta_normalized *= scale;
        saturated = 1U;
    }

    phase_a = alpha_normalized;
    phase_b = -0.5f * alpha_normalized + FOC_SQRT3_OVER_TWO * beta_normalized;
    phase_c = -0.5f * alpha_normalized - FOC_SQRT3_OVER_TWO * beta_normalized;
    phase_max = fmaxf(phase_a, fmaxf(phase_b, phase_c));
    phase_min = fminf(phase_a, fminf(phase_b, phase_c));
    common_mode = 0.5f * (phase_max + phase_min);

    duty_a = 0.5f + phase_a - common_mode;
    duty_b = 0.5f + phase_b - common_mode;
    duty_c = 0.5f + phase_c - common_mode;

    if (duty_a < config->duty_min) {
        duty_a = config->duty_min;
        saturated = 1U;
    } else if (duty_a > config->duty_max) {
        duty_a = config->duty_max;
        saturated = 1U;
    }
    if (duty_b < config->duty_min) {
        duty_b = config->duty_min;
        saturated = 1U;
    } else if (duty_b > config->duty_max) {
        duty_b = config->duty_max;
        saturated = 1U;
    }
    if (duty_c < config->duty_min) {
        duty_c = config->duty_min;
        saturated = 1U;
    } else if (duty_c > config->duty_max) {
        duty_c = config->duty_max;
        saturated = 1U;
    }

    angle = atan2f(input->beta, input->alpha);
    if (angle < 0.0f) {
        angle += FOC_TWO_PI;
    }
    sector = (unsigned int)(angle / (FOC_PI / 3.0f)) + 1U;
    if (sector > 6U) {
        sector = 6U;
    }

    if (!foc_float_is_finite(duty_a) || !foc_float_is_finite(duty_b) ||
        !foc_float_is_finite(duty_c)) {
        return FOC_STATUS_ERR_NONFINITE;
    }

    result.duty_a = duty_a;
    result.duty_b = duty_b;
    result.duty_c = duty_c;
    result.sector = sector;
    result.saturated = saturated;
    *output = result;
    return FOC_STATUS_OK;
}

int foc_init(struct foc_context *context, const struct foc_config *config)
{
    int status;

    if (context == NULL || config == NULL) {
        return FOC_STATUS_ERR_NULL;
    }

    status = foc_config_validate(config);
    if (status != FOC_STATUS_OK) {
        return status;
    }

    context->config = *config;
    context->theta_elec_rad = 0.0f;
    return FOC_STATUS_OK;
}

int foc_reset(struct foc_context *context)
{
    if (context == NULL) {
        return FOC_STATUS_ERR_NULL;
    }

    context->theta_elec_rad = 0.0f;
    return FOC_STATUS_OK;
}

int foc_step(
    struct foc_context *context,
    const struct foc_validated_motion_command *command,
    const struct foc_measurement *measurement,
    const struct foc_timing *timing,
    struct foc_output *output
)
{
    float next_theta;
    float voltage_magnitude;
    float safe_theta = 0.0f;
    unsigned int voltage_saturated = 0U;
    struct foc_vector_dq dq;
    struct foc_vector_alpha_beta alpha_beta;
    struct foc_svpwm_output svpwm;
    int status;

    if (output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    foc_set_safe_output(output, 0.0f);

    if (context == NULL || command == NULL || measurement == NULL || timing == NULL) {
        return FOC_STATUS_ERR_NULL;
    }

    if (foc_float_is_finite(context->theta_elec_rad)) {
        safe_theta = context->theta_elec_rad;
    }
    foc_set_safe_output(output, safe_theta);
    status = foc_config_validate(&context->config);
    if (status != FOC_STATUS_OK) {
        return status;
    }

    if (!foc_float_is_finite(context->theta_elec_rad) || !foc_float_is_finite(command->vd_v) ||
        !foc_float_is_finite(command->vq_v) ||
        !foc_float_is_finite(command->electrical_speed_rad_s) ||
        !foc_float_is_finite(timing->dt_s)) {
        return FOC_STATUS_ERR_NONFINITE;
    }

    if (timing->dt_s <= 0.0f || timing->dt_s > context->config.max_dt_s) {
        return FOC_STATUS_ERR_INVALID_DT;
    }

    if (!foc_float_is_finite(measurement->vbus_v) ||
        measurement->vbus_v < context->config.vbus_min_v) {
        return FOC_STATUS_ERR_INVALID_VBUS;
    }

    if (command->mode == FOC_CONTROL_DISABLED || command->enable == 0U) {
        return FOC_STATUS_OK;
    }

    if (command->mode != FOC_CONTROL_VOLTAGE_OPEN_LOOP) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }

    next_theta = context->theta_elec_rad + command->electrical_speed_rad_s * timing->dt_s;
    if (!foc_float_is_finite(next_theta)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    next_theta = foc_wrap_angle(next_theta);

    dq.d = command->vd_v;
    dq.q = command->vq_v;
    voltage_magnitude = sqrtf(dq.d * dq.d + dq.q * dq.q);
    if (!foc_float_is_finite(voltage_magnitude)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    if (voltage_magnitude > context->config.voltage_limit_v) {
        const float scale = context->config.voltage_limit_v / voltage_magnitude;

        dq.d *= scale;
        dq.q *= scale;
        voltage_saturated = 1U;
    }

    status = foc_inverse_park(&dq, next_theta, &alpha_beta);
    if (status != FOC_STATUS_OK) {
        return status;
    }

    status = foc_svpwm(&alpha_beta, measurement->vbus_v, &context->config, &svpwm);
    if (status != FOC_STATUS_OK) {
        return status;
    }

    /* Commit state only after every downstream calculation has succeeded. */
    context->theta_elec_rad = next_theta;
    output->output_enable = 1U;
    output->theta_elec_rad = next_theta;
    output->voltage_alpha_beta_v = alpha_beta;
    svpwm.saturated |= voltage_saturated;
    output->svpwm = svpwm;
    return FOC_STATUS_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
