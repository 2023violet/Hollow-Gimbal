/**
 * @file    foc_virtual_motor.c
 * @brief   Deterministic Host-only PMSM plant and virtual sensor boundary.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Implementation role
 * -------------------
 * Integrate one average-value PMSM instance and translate its state through
 * the existing Device ADC and encoder input contracts.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_motor.h"

#include <math.h>
#include <stddef.h>

/* Private Constants ------------------------------------------------------------ */

static const double FOC_VIRTUAL_TWO_PI = 6.28318530717958647692;
static const double FOC_VIRTUAL_SQRT3 = 1.73205080756887729353;
static const double FOC_VIRTUAL_ADC_MAX = 4095.0;

/* Private Types ---------------------------------------------------------------- */

struct foc_virtual_motor_derivative {
    double id_a;
    double iq_a;
    double mechanical_position_rad;
    double mechanical_velocity_rad_s;
};

/* Private Helpers -------------------------------------------------------------- */

static bool foc_virtual_finite_state(const struct foc_virtual_motor_state *state)
{
    return state != NULL && isfinite(state->id_a) && isfinite(state->iq_a) &&
        isfinite(state->mechanical_position_rad) && isfinite(state->mechanical_velocity_rad_s);
}

static bool foc_virtual_finite_params(const struct foc_virtual_motor_params *params)
{
    return params != NULL && isfinite(params->rs_ohm) && params->rs_ohm > 0.0 &&
        isfinite(params->ld_h) && params->ld_h > 0.0 && isfinite(params->lq_h) &&
        params->lq_h > 0.0 && isfinite(params->psi_f_wb) && params->psi_f_wb >= 0.0 &&
        params->pole_pairs > 0U && isfinite(params->electrical_offset_rad) &&
        isfinite(params->inertia_kg_m2) && params->inertia_kg_m2 > 0.0 &&
        isfinite(params->damping_nm_s) && params->damping_nm_s >= 0.0;
}

static double foc_virtual_angle(
    const struct foc_virtual_motor *motor,
    const struct foc_virtual_motor_state *state
)
{
    return (double)motor->params.pole_pairs * state->mechanical_position_rad +
        motor->params.electrical_offset_rad;
}

static struct foc_virtual_motor_derivative foc_virtual_derivative(
    const struct foc_virtual_motor *motor,
    const struct foc_virtual_motor_state *state,
    double v_alpha_v,
    double v_beta_v
)
{
    const double theta = foc_virtual_angle(motor, state);
    const double sine = sin(theta);
    const double cosine = cos(theta);
    const double vd_v = cosine * v_alpha_v + sine * v_beta_v;
    const double vq_v = cosine * v_beta_v - sine * v_alpha_v;
    const double omega_e_rad_s =
        (double)motor->params.pole_pairs * state->mechanical_velocity_rad_s;
    const double torque_nm = 1.5 * (double)motor->params.pole_pairs *
        (motor->params.psi_f_wb * state->iq_a +
         (motor->params.ld_h - motor->params.lq_h) * state->id_a * state->iq_a);

    return (struct foc_virtual_motor_derivative){
        .id_a = (vd_v - motor->params.rs_ohm * state->id_a +
                 omega_e_rad_s * motor->params.lq_h * state->iq_a) /
            motor->params.ld_h,
        .iq_a = (vq_v - motor->params.rs_ohm * state->iq_a -
                 omega_e_rad_s * (motor->params.ld_h * state->id_a + motor->params.psi_f_wb)) /
            motor->params.lq_h,
        .mechanical_position_rad = state->mechanical_velocity_rad_s,
        .mechanical_velocity_rad_s =
            (torque_nm - motor->params.damping_nm_s * state->mechanical_velocity_rad_s -
             motor->load_torque_nm) /
            motor->params.inertia_kg_m2,
    };
}

static struct foc_virtual_motor_state foc_virtual_add(
    const struct foc_virtual_motor_state *state,
    const struct foc_virtual_motor_derivative *derivative,
    double scale
)
{
    return (struct foc_virtual_motor_state){
        .id_a = state->id_a + derivative->id_a * scale,
        .iq_a = state->iq_a + derivative->iq_a * scale,
        .mechanical_position_rad =
            state->mechanical_position_rad + derivative->mechanical_position_rad * scale,
        .mechanical_velocity_rad_s =
            state->mechanical_velocity_rad_s + derivative->mechanical_velocity_rad_s * scale,
    };
}

static bool foc_virtual_request_duty_valid(const struct foc_device_pwm_request *request)
{
    return request != NULL && request->action == FOC_DEVICE_OUTPUT_UPDATE &&
        request->output_enable_request != 0U && request->duty_a >= 0.0f &&
        request->duty_a <= 1.0f && request->duty_b >= 0.0f && request->duty_b <= 1.0f &&
        request->duty_c >= 0.0f && request->duty_c <= 1.0f;
}

static uint16_t foc_virtual_clamp_adc(double value, bool *valid)
{
    if (!isfinite(value) || value < 0.0 || value > FOC_VIRTUAL_ADC_MAX) {
        *valid = false;
    }
    if (value < 0.0) {
        value = 0.0;
    } else if (value > FOC_VIRTUAL_ADC_MAX) {
        value = FOC_VIRTUAL_ADC_MAX;
    }
    return (uint16_t)lround(value);
}

static double foc_virtual_wrap(double angle_rad)
{
    double wrapped = fmod(angle_rad, FOC_VIRTUAL_TWO_PI);
    if (wrapped < 0.0) {
        wrapped += FOC_VIRTUAL_TWO_PI;
    }
    return wrapped;
}

/* Public Functions ------------------------------------------------------------- */

int foc_virtual_motor_init(
    struct foc_virtual_motor *motor,
    const struct foc_virtual_motor_params *params,
    const struct foc_virtual_motor_state *initial_state
)
{
    if (motor == NULL || !foc_virtual_finite_params(params)) {
        return -1;
    }
    const struct foc_virtual_motor_state state =
        initial_state == NULL ? (struct foc_virtual_motor_state){0} : *initial_state;
    if (!foc_virtual_finite_state(&state)) {
        return -1;
    }
    *motor = (struct foc_virtual_motor){
        .params = *params,
        .state = state,
        .load_torque_nm = 0.0,
        .initialized = true,
    };
    return 0;
}

int foc_virtual_motor_set_load(struct foc_virtual_motor *motor, double load_torque_nm)
{
    if (motor == NULL || !motor->initialized || !isfinite(load_torque_nm)) {
        return -1;
    }
    motor->load_torque_nm = load_torque_nm;
    return 0;
}

int foc_virtual_motor_step(
    struct foc_virtual_motor *motor,
    double v_alpha_v,
    double v_beta_v,
    double dt_s
)
{
    if (motor == NULL || !motor->initialized || !isfinite(v_alpha_v) || !isfinite(v_beta_v) ||
        !isfinite(dt_s) || dt_s <= 0.0 || dt_s > 1.0) {
        return -1;
    }

    const struct foc_virtual_motor_derivative k1 =
        foc_virtual_derivative(motor, &motor->state, v_alpha_v, v_beta_v);
    const struct foc_virtual_motor_state s2 = foc_virtual_add(&motor->state, &k1, dt_s / 2.0);
    const struct foc_virtual_motor_derivative k2 =
        foc_virtual_derivative(motor, &s2, v_alpha_v, v_beta_v);
    const struct foc_virtual_motor_state s3 = foc_virtual_add(&motor->state, &k2, dt_s / 2.0);
    const struct foc_virtual_motor_derivative k3 =
        foc_virtual_derivative(motor, &s3, v_alpha_v, v_beta_v);
    const struct foc_virtual_motor_state s4 = foc_virtual_add(&motor->state, &k3, dt_s);
    const struct foc_virtual_motor_derivative k4 =
        foc_virtual_derivative(motor, &s4, v_alpha_v, v_beta_v);
    const struct foc_virtual_motor_state trial = {
        .id_a =
            motor->state.id_a + dt_s * (k1.id_a + 2.0 * k2.id_a + 2.0 * k3.id_a + k4.id_a) / 6.0,
        .iq_a =
            motor->state.iq_a + dt_s * (k1.iq_a + 2.0 * k2.iq_a + 2.0 * k3.iq_a + k4.iq_a) / 6.0,
        .mechanical_position_rad = motor->state.mechanical_position_rad +
            dt_s *
                (k1.mechanical_position_rad + 2.0 * k2.mechanical_position_rad +
                 2.0 * k3.mechanical_position_rad + k4.mechanical_position_rad) /
                6.0,
        .mechanical_velocity_rad_s = motor->state.mechanical_velocity_rad_s +
            dt_s *
                (k1.mechanical_velocity_rad_s + 2.0 * k2.mechanical_velocity_rad_s +
                 2.0 * k3.mechanical_velocity_rad_s + k4.mechanical_velocity_rad_s) /
                6.0,
    };
    if (!foc_virtual_finite_state(&trial)) {
        return -1;
    }
    motor->state = trial;
    return 0;
}

int foc_virtual_motor_apply_duty(
    struct foc_virtual_motor *motor,
    const struct foc_device_pwm_request *request,
    float vbus_v,
    double dt_s
)
{
    if (!foc_virtual_request_duty_valid(request) || !isfinite(vbus_v) || vbus_v <= 0.0f) {
        return -1;
    }
    const double alpha_v = (double)vbus_v *
        (2.0 * (double)request->duty_a - (double)request->duty_b - (double)request->duty_c) / 3.0;
    const double beta_v =
        (double)vbus_v * ((double)request->duty_b - (double)request->duty_c) / FOC_VIRTUAL_SQRT3;
    return foc_virtual_motor_step(motor, alpha_v, beta_v, dt_s);
}

int foc_virtual_motor_make_adc(
    const struct foc_virtual_motor *motor,
    const struct foc_device_adc_config *config,
    float vbus_v,
    uint32_t sequence,
    uint32_t timestamp_us,
    struct foc_device_adc_raw *raw
)
{
    if (motor == NULL || !motor->initialized || config == NULL || raw == NULL ||
        foc_device_adc_config_validate(config) != FOC_DEVICE_STATUS_OK || !isfinite(vbus_v) ||
        vbus_v <= 0.0f) {
        return -1;
    }
    const double theta = foc_virtual_angle(motor, &motor->state);
    const double sine = sin(theta);
    const double cosine = cos(theta);
    const double alpha = cosine * motor->state.id_a - sine * motor->state.iq_a;
    const double beta = sine * motor->state.id_a + cosine * motor->state.iq_a;
    bool vbus_valid = true;
    bool phase_a_valid = true;
    bool phase_b_valid = true;
    bool phase_c_valid = true;
    const double vbus_raw = (double)vbus_v / config->vbus_v_per_count;
    const double phase_a_raw =
        (double)config->phase_a_offset_count + alpha / config->current_a_per_count;
    const double phase_b = -0.5 * alpha + 0.5 * FOC_VIRTUAL_SQRT3 * beta;
    const double phase_c = -0.5 * alpha - 0.5 * FOC_VIRTUAL_SQRT3 * beta;
    const double phase_b_raw =
        (double)config->phase_b_offset_count + phase_b / config->current_a_per_count;
    const double phase_c_raw =
        (double)config->phase_c_offset_count + phase_c / config->current_a_per_count;

    const uint16_t phase_a_code = foc_virtual_clamp_adc(phase_a_raw, &phase_a_valid);
    const uint16_t phase_b_code = foc_virtual_clamp_adc(phase_b_raw, &phase_b_valid);
    const uint16_t phase_c_code = foc_virtual_clamp_adc(phase_c_raw, &phase_c_valid);
    *raw = (struct foc_device_adc_raw){
        .vbus_raw = foc_virtual_clamp_adc(vbus_raw, &vbus_valid),
        .rank2_raw = phase_a_code,
        .rank3_raw =
            config->phase_order == FOC_DEVICE_PHASE_ORDER_ABC ? phase_b_code : phase_c_code,
        .rank4_raw =
            config->phase_order == FOC_DEVICE_PHASE_ORDER_ABC ? phase_c_code : phase_b_code,
        .valid_mask = (vbus_valid ? FOC_DEVICE_SAMPLE_VBUS_VALID : 0U) |
            (phase_a_valid ? FOC_DEVICE_SAMPLE_RANK2_VALID : 0U) |
            (phase_b_valid ? FOC_DEVICE_SAMPLE_RANK3_VALID : 0U) |
            (phase_c_valid ? FOC_DEVICE_SAMPLE_RANK4_VALID : 0U),
        .sample_seq = sequence,
        .sample_tick = timestamp_us,
    };
    return 0;
}

int foc_virtual_motor_make_encoder_code(
    const struct foc_virtual_motor *motor,
    const struct foc_encoder_config *config,
    uint16_t *raw_code
)
{
    if (motor == NULL || !motor->initialized || config == NULL || raw_code == NULL ||
        config->counts_per_revolution == 0U || config->counts_per_revolution > 65536U ||
        (config->direction != FOC_ENCODER_DIRECTION_NORMAL &&
         config->direction != FOC_ENCODER_DIRECTION_REVERSED)) {
        return -1;
    }
    struct foc_encoder_context validation;
    if (foc_encoder_init(&validation, config) != FOC_ENCODER_STATUS_OK) {
        return -1;
    }
    const double mechanical = foc_virtual_wrap(motor->state.mechanical_position_rad);
    const double directed = config->direction == FOC_ENCODER_DIRECTION_NORMAL
        ? mechanical
        : FOC_VIRTUAL_TWO_PI - mechanical;
    uint32_t code =
        (uint32_t)llround(directed * (double)config->counts_per_revolution / FOC_VIRTUAL_TWO_PI);
    if (code >= config->counts_per_revolution) {
        code = 0U;
    }
    *raw_code = (uint16_t)code;
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
