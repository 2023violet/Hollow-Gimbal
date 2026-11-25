/**
 * @file    foc_motor_id.c
 * @brief   Window-integral least squares for R/L (q: optional back-EMF) and ideal PI design.
 * @version v0.3.0
 * @date    2026.11.23
 *
 * Implementation role
 * -------------------
 * Accumulate bounded regression products and publish validated offline candidates.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_motor_id.h"
#include "foc_calibration_math.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

#define FOC_MOTOR_ID_MAX_SAMPLES      (65535U)
#define FOC_MOTOR_ID_MIN_EXCITATION   (0.02)
#define FOC_MOTOR_PI_TWO_PI           (6.2831853071795864769)
#define FOC_MOTOR_PI_MAX_SAMPLE_PHASE (0.3)
#define FOC_MOTOR_ID_MAX_RL_STEP      (0.3)
#define FOC_MOTOR_PI_MAX_DELAY_PHASE  (0.5235987755982988731)

/* Private Helpers -------------------------------------------------------------- */

static bool foc_motor_id_positive(float value)
{
    return isfinite(value) && value > 0.0f;
}

static bool foc_motor_id_bounds(float minimum, float maximum)
{
    return foc_motor_id_positive(minimum) && isfinite(maximum) && maximum >= minimum;
}

static bool foc_motor_id_voltage_source_valid(
    enum foc_motor_id_source source,
    enum foc_motor_id_voltage_source voltage_source
)
{
    return (source == FOC_MOTOR_ID_VIRTUAL &&
            voltage_source == FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL) ||
        (source == FOC_MOTOR_ID_MEASURED &&
         (voltage_source == FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL ||
          voltage_source == FOC_MOTOR_ID_VOLTAGE_CALIBRATED_TERMINAL ||
          voltage_source == FOC_MOTOR_ID_VOLTAGE_COMMANDED));
}

static bool foc_motor_id_config_valid(const struct foc_motor_id_config *config)
{
    return (config->axis == FOC_MOTOR_ID_D || config->axis == FOC_MOTOR_ID_Q) &&
        foc_motor_id_voltage_source_valid(config->source, config->voltage_source) &&
        config->setup_id != 0U && config->min_interval_us > 0U &&
        config->min_interval_us <= config->max_interval_us &&
        config->max_interval_us < UINT32_C(0x80000000) && config->intervals_per_window >= 2U &&
        config->min_windows >= 3U &&
        config->min_windows <= (FOC_MOTOR_ID_MAX_SAMPLES - 1U) / config->intervals_per_window &&
        foc_motor_id_bounds(config->min_current_rms_a, config->max_abs_current_a) &&
        foc_motor_id_bounds(config->min_voltage_rms_v, config->max_abs_voltage_v) &&
        foc_motor_id_positive(config->min_slew_rms_a_per_s) &&
        foc_motor_id_positive(config->max_residual_rms_v) &&
        foc_motor_id_bounds(config->min_rs_ohm, config->max_rs_ohm) &&
        foc_motor_id_bounds(config->min_inductance_h, config->max_inductance_h) &&
        (!config->fit_back_emf ||
         (config->axis == FOC_MOTOR_ID_Q && isfinite(config->min_speed_rms_rad_s) &&
          config->min_speed_rms_rad_s >= 0.0f &&
          foc_motor_id_positive(config->max_abs_speed_rad_s) &&
          config->min_speed_rms_rad_s < config->max_abs_speed_rad_s)) &&
        isfinite(config->known_rs_ohm) && config->known_rs_ohm >= 0.0f &&
        (config->known_rs_ohm == 0.0f || config->fit_back_emf) &&
        !(config->fit_dead_time && config->fit_back_emf);
}

static int foc_motor_id_invalidate(
    struct foc_motor_id_context *context,
    enum foc_calibration_failure failure,
    int status
)
{
    context->result.state = FOC_CALIBRATION_STATE_INVALID;
    context->result.failure = failure;
    return status;
}

/* False when the window's mean speed exceeds the configured bound. */
static bool
foc_motor_id_close_window(struct foc_motor_id_context *context, float current_a, float angle_rad)
{
    /* One row for the whole window: mean(v) = R mean(i) + L (i_end-i_start)/T, plus
     * ke (angle_end-angle_start)/T when the shaft may turn (the back-EMF integral). */
    const double inverse_s = 1.0 / ((double)context->window_us * 1.0e-6);
    const double i = context->current_as * inverse_s;
    const double slew = ((double)current_a - context->first_current_a) * inverse_s;
    const double v = context->voltage_vs * inverse_s;
    if (context->config.fit_dead_time) {
        const double u = context->dead_time_s * inverse_s;
        context->u2 += u * u;
        context->i_u += i * u;
        context->slew_u += slew * u;
        context->u_v += u * v;
        context->dead_time_s = 0.0;
    }
    if (context->config.fit_back_emf) {
        const double m = ((double)angle_rad - context->first_angle_rad) * inverse_s;
        if (!(fabs(m) <= context->config.max_abs_speed_rad_s)) {
            return false;
        }
        context->m2 += m * m;
        context->i_m += i * m;
        context->slew_m += slew * m;
        context->m_v += m * v;
    }
    context->i2 += i * i;
    context->slew2 += slew * slew;
    context->i_slew += i * slew;
    context->i_v += i * v;
    context->slew_v += slew * v;
    context->v2 += v * v;
    context->result.windows++;
    context->window_us = 0U;
    context->intervals = 0U;
    context->current_as = 0.0;
    context->voltage_vs = 0.0;
    context->first_current_a = current_a;
    context->first_angle_rad = angle_rad;
    return true;
}

/* Solve a normalized 3x3 Gram system by its adjugate. Columns have norms n[k], pairwise
 * correlations c01, c02, c12 and normalized projections y[k]; returns the excitation
 * (Gram determinant) and writes the coefficients. */
static double foc_motor_id_solve3(
    const double n[3],
    double c01,
    double c02,
    double c12,
    const double y[3],
    double x[3]
)
{
    const double excitation = 1.0 + 2.0 * c01 * c02 * c12 - c01 * c01 - c02 * c02 - c12 * c12;
    const double a00 = 1.0 - c12 * c12;
    const double a11 = 1.0 - c02 * c02;
    const double a22 = 1.0 - c01 * c01;
    const double a01 = c02 * c12 - c01;
    const double a02 = c01 * c12 - c02;
    const double a12 = c01 * c02 - c12;
    x[0] = (a00 * y[0] + a01 * y[1] + a02 * y[2]) / (excitation * n[0]);
    x[1] = (a01 * y[0] + a11 * y[1] + a12 * y[2]) / (excitation * n[1]);
    x[2] = (a02 * y[0] + a12 * y[1] + a22 * y[2]) / (excitation * n[2]);
    return excitation;
}

static enum foc_calibration_failure foc_motor_id_fit(struct foc_motor_id_context *context)
{
    const struct foc_motor_id_config *config = &context->config;
    struct foc_motor_id_result *result = &context->result;
    const double count = (double)result->windows;
    const double min_i = config->min_current_rms_a;
    const double min_slew = config->min_slew_rms_a_per_s;
    const double min_v = config->min_voltage_rms_v;
    if (!isfinite(context->i2) || !isfinite(context->slew2) || !isfinite(context->v2)) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    if (context->i2 < count * min_i * min_i || context->slew2 < count * min_slew * min_slew ||
        context->v2 < count * min_v * min_v) {
        return FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE;
    }

    const double m_floor = (double)config->min_speed_rms_rad_s;
    const bool motion =
        config->fit_back_emf && context->m2 > 0.0 && context->m2 >= count * m_floor * m_floor;
    if (motion && !isfinite(context->m2)) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }

    /* Normalize the columns before testing their mutual independence. */
    const double i_norm = foc_calibration_sqrt(context->i2);
    const double slew_norm = foc_calibration_sqrt(context->slew2);
    const double correlation = context->i_slew / (i_norm * slew_norm);
    const double iv = context->i_v / i_norm;
    const double sv = context->slew_v / slew_norm;
    double excitation = 1.0 - correlation * correlation;
    double rs = 0.0;
    double l = 0.0;
    double ke = 0.0;
    double vdt = 0.0;
    bool rs_fixed = false;
    bool dead_time = false;
    if (motion && config->known_rs_ohm > 0.0f) {
        /* The shaft turns: R comes from the still d axis, the q data fit L and ke. On a light
         * rotor i follows di/dt within a long level, so a free R separates poorly from L. */
        const double m_norm = foc_calibration_sqrt(context->m2);
        const double sm = context->slew_m / (slew_norm * m_norm);
        rs = config->known_rs_ohm;
        const double ys = (context->slew_v - rs * context->i_slew) / slew_norm;
        const double ym = (context->m_v - rs * context->i_m) / m_norm;
        excitation = 1.0 - sm * sm;
        l = (ys - sm * ym) / (excitation * slew_norm);
        ke = (ym - sm * ys) / (excitation * m_norm);
        rs_fixed = true;
    } else if (motion) {
        /* Three terms R, L, ke. */
        const double m_norm = foc_calibration_sqrt(context->m2);
        const double norms[3] = {i_norm, slew_norm, m_norm};
        const double y[3] = {iv, sv, context->m_v / m_norm};
        double x[3];
        excitation = foc_motor_id_solve3(
            norms,
            correlation,
            context->i_m / (i_norm * m_norm),
            context->slew_m / (slew_norm * m_norm),
            y,
            x
        );
        rs = x[0];
        l = x[1];
        ke = x[2];
    } else {
        rs = (iv - correlation * sv) / (excitation * i_norm);
        l = (sv - correlation * iv) / (excitation * slew_norm);
        if (config->fit_dead_time && context->u2 > 0.0 && isfinite(context->u2)) {
            /* Three terms R, L, Vdt; kept only when the columns are independent enough. */
            const double u_norm = foc_calibration_sqrt(context->u2);
            const double norms[3] = {i_norm, slew_norm, u_norm};
            const double y[3] = {iv, sv, context->u_v / u_norm};
            double x[3];
            const double three = foc_motor_id_solve3(
                norms,
                correlation,
                context->i_u / (i_norm * u_norm),
                context->slew_u / (slew_norm * u_norm),
                y,
                x
            );
            if (isfinite(three) && three >= FOC_MOTOR_ID_MIN_EXCITATION) {
                excitation = three;
                rs = x[0];
                l = x[1];
                vdt = x[2];
                dead_time = true;
            }
        }
    }
    if (!isfinite(excitation)) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    result->excitation = excitation > 0.0 ? excitation : 0.0;
    if (excitation < FOC_MOTOR_ID_MIN_EXCITATION) {
        return FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE;
    }
    if (!isfinite(rs) || !isfinite(l) || !isfinite(ke) || !isfinite(vdt)) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    if (rs < config->min_rs_ohm || rs > config->max_rs_ohm || l < config->min_inductance_h ||
        l > config->max_inductance_h) {
        return FOC_CALIBRATION_FAILURE_RANGE;
    }
    /* Endpoint quadrature holds while the identified pole is resolved by the longest
     * admitted interval (interval x Rs / L at most 0.3). The bound applies to the fitted
     * pair, so a range wide in both Rs and L stays usable for any resolvable motor. */
    if (((double)config->max_interval_us * 1.0e-6) * rs > FOC_MOTOR_ID_MAX_RL_STEP * l) {
        return FOC_CALIBRATION_FAILURE_RANGE;
    }
    /* Evaluate the published Float32 values, allowing for accumulated roundoff
     * in the residual so an unresolved near-zero value cannot pass a tighter limit.
     * Without motion or dead time those terms are exact zeros and the sums are unchanged. */
    const float published_rs = (float)rs;
    const float published_l = (float)l;
    const float published_ke = (float)ke;
    const float published_vdt = (float)vdt;
    const double r_iv = 2.0 * published_rs * context->i_v;
    const double l_sv = 2.0 * published_l * context->slew_v;
    const double rr = (double)published_rs * published_rs * context->i2;
    const double ll = (double)published_l * published_l * context->slew2;
    const double rl = 2.0 * published_rs * published_l * context->i_slew;
    const double k_mv = 2.0 * published_ke * context->m_v;
    const double kk = (double)published_ke * published_ke * context->m2;
    const double rk = 2.0 * published_rs * published_ke * context->i_m;
    const double lk = 2.0 * published_l * published_ke * context->slew_m;
    const double d_uv = 2.0 * published_vdt * context->u_v;
    const double dd = (double)published_vdt * published_vdt * context->u2;
    const double rd = 2.0 * published_rs * published_vdt * context->i_u;
    const double ld = 2.0 * published_l * published_vdt * context->slew_u;
    const double residual2 =
        context->v2 - r_iv - l_sv + rr + ll + rl - k_mv + kk + rk + lk - d_uv + dd + rd + ld;
    const double scale = context->v2 + fabs(r_iv) + fabs(l_sv) + rr + ll + fabs(rl) + fabs(k_mv) +
        kk + fabs(rk) + fabs(lk) + fabs(d_uv) + dd + fabs(rd) + fabs(ld);
    const double roundoff = (8.0 * count + 64.0) * DBL_EPSILON * scale;
    if (!isfinite(residual2) || !isfinite(roundoff) || residual2 < -roundoff) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    const double residual_upper = (residual2 > 0.0 ? residual2 : 0.0) + roundoff;
    result->residual_rms_v = foc_calibration_sqrt(residual_upper / count);
    if (result->residual_rms_v > config->max_residual_rms_v) {
        return FOC_CALIBRATION_FAILURE_RESIDUAL;
    }
    result->rs_ohm = published_rs;
    result->inductance_h = published_l;
    result->back_emf_v_s_per_rad = published_ke;
    result->back_emf_fitted = motion;
    result->rs_fixed = rs_fixed;
    result->dead_time_v = published_vdt;
    result->dead_time_fitted = dead_time;
    return FOC_CALIBRATION_FAILURE_NONE;
}

/* Public Functions ----------------------------------------------------------- */

int foc_motor_id_init(
    struct foc_motor_id_context *context,
    const struct foc_motor_id_config *config,
    uint32_t capture_id
)
{
    if (context == NULL || config == NULL) {
        return FOC_MOTOR_ID_ERR_NULL;
    }
    if (!foc_motor_id_config_valid(config)) {
        return FOC_MOTOR_ID_ERR_CONFIG;
    }
    *context = (struct foc_motor_id_context){
        .config = *config,
        .result = {
            .state = FOC_CALIBRATION_STATE_COLLECTING,
            .axis = config->axis,
            .source = config->source,
            .voltage_source = config->voltage_source,
            .setup_id = config->setup_id,
            .capture_id = capture_id,
        },
    };
    return FOC_MOTOR_ID_OK;
}

int foc_motor_id_reset(struct foc_motor_id_context *context, uint32_t capture_id)
{
    if (context == NULL) {
        return FOC_MOTOR_ID_ERR_NULL;
    }
    if (context->result.state == FOC_CALIBRATION_STATE_UNINITIALIZED) {
        return FOC_MOTOR_ID_ERR_STATE;
    }
    return foc_motor_id_init(context, &context->config, capture_id);
}

int foc_motor_id_push(
    struct foc_motor_id_context *context,
    const struct foc_motor_id_sample *sample
)
{
    if (context == NULL || sample == NULL) {
        return FOC_MOTOR_ID_ERR_NULL;
    }
    struct foc_motor_id_result *result = &context->result;
    if (result->state != FOC_CALIBRATION_STATE_COLLECTING) {
        return FOC_MOTOR_ID_ERR_STATE;
    }
    const struct foc_motor_id_config *config = &context->config;
    const bool has_interval = result->samples != 0U;

    if (sample->capture_id != result->capture_id || sample->setup_id != config->setup_id ||
        sample->source != config->source) {
        return foc_motor_id_invalidate(
            context,
            FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
            FOC_MOTOR_ID_ERR_SAMPLE
        );
    }
    if (!sample->current_valid || !isfinite(sample->current_a) ||
        !isfinite(sample->omega_e_rad_s) ||
        (config->fit_back_emf && !isfinite(sample->rotor_angle_rad)) ||
        (config->fit_dead_time && has_interval && !isfinite(sample->dead_time_basis)) ||
        (has_interval && (!sample->voltage_valid || !isfinite(sample->mean_voltage_v)))) {
        return foc_motor_id_invalidate(
            context,
            FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
            FOC_MOTOR_ID_ERR_SAMPLE
        );
    }
    /* A turning shaft is admitted only when the back-EMF is fitted. */
    if (!sample->frame_confirmed ||
        (!config->fit_back_emf &&
         (!sample->stationary_confirmed || sample->omega_e_rad_s != 0.0f)) ||
        sample->saturated ||
        (has_interval &&
         (!sample->output_confirmed || sample->voltage_source != config->voltage_source))) {
        return foc_motor_id_invalidate(
            context,
            FOC_CALIBRATION_FAILURE_PRECONDITION,
            FOC_MOTOR_ID_ERR_PRECONDITION
        );
    }
    if (fabsf(sample->current_a) > config->max_abs_current_a ||
        (has_interval && fabsf(sample->mean_voltage_v) > config->max_abs_voltage_v)) {
        return foc_motor_id_invalidate(
            context,
            FOC_CALIBRATION_FAILURE_RANGE,
            FOC_MOTOR_ID_ERR_SAMPLE
        );
    }
    if (result->samples == FOC_MOTOR_ID_MAX_SAMPLES) {
        return foc_motor_id_invalidate(
            context,
            FOC_CALIBRATION_FAILURE_SAMPLE_COUNT,
            FOC_MOTOR_ID_ERR_QUALITY
        );
    }
    if (has_interval) {
        const uint32_t elapsed_us = sample->captured_us - result->last_us;
        if (sample->sequence != result->last_sequence + 1U ||
            elapsed_us < config->min_interval_us || elapsed_us > config->max_interval_us ||
            sample->voltage_begin_us != result->last_us ||
            sample->voltage_end_us != sample->captured_us) {
            return foc_motor_id_invalidate(
                context,
                FOC_CALIBRATION_FAILURE_TIME_ORDER,
                FOC_MOTOR_ID_ERR_SAMPLE
            );
        }
        const double elapsed_s = (double)elapsed_us * 1.0e-6;
        context->current_as +=
            0.5 * ((double)context->previous_current_a + sample->current_a) * elapsed_s;
        context->voltage_vs += (double)sample->mean_voltage_v * elapsed_s;
        context->dead_time_s += (double)sample->dead_time_basis * elapsed_s;
        context->window_us += elapsed_us;
        context->intervals++;
        if (context->intervals == config->intervals_per_window &&
            !foc_motor_id_close_window(context, sample->current_a, sample->rotor_angle_rad)) {
            return foc_motor_id_invalidate(
                context,
                FOC_CALIBRATION_FAILURE_PRECONDITION,
                FOC_MOTOR_ID_ERR_PRECONDITION
            );
        }
    } else {
        context->first_current_a = sample->current_a;
        context->first_angle_rad = sample->rotor_angle_rad;
        result->first_sequence = sample->sequence;
        result->first_us = sample->captured_us;
    }
    context->previous_current_a = sample->current_a;
    result->last_sequence = sample->sequence;
    result->last_us = sample->captured_us;
    result->samples++;
    return FOC_MOTOR_ID_OK;
}

int foc_motor_id_finish(struct foc_motor_id_context *context, struct foc_motor_id_result *result)
{
    if (result == NULL) {
        return FOC_MOTOR_ID_ERR_NULL;
    }
    if (context == NULL) {
        *result = (struct foc_motor_id_result){0};
        return FOC_MOTOR_ID_ERR_NULL;
    }
    if (context->result.state == FOC_CALIBRATION_STATE_COLLECTING) {
        const enum foc_calibration_failure failure =
            context->result.windows < context->config.min_windows || context->intervals != 0U
            ? FOC_CALIBRATION_FAILURE_SAMPLE_COUNT
            : foc_motor_id_fit(context);
        context->result.failure = failure;
        context->result.usable = failure == FOC_CALIBRATION_FAILURE_NONE;
        context->result.state =
            context->result.usable ? FOC_CALIBRATION_STATE_VALID : FOC_CALIBRATION_STATE_INVALID;
    }
    *result = context->result;
    if (result->state == FOC_CALIBRATION_STATE_UNINITIALIZED) {
        return FOC_MOTOR_ID_ERR_STATE;
    }
    return result->usable ? FOC_MOTOR_ID_OK : FOC_MOTOR_ID_ERR_QUALITY;
}

static bool
foc_motor_id_fit_usable(const struct foc_motor_id_result *fit, enum foc_motor_id_axis axis)
{
    return fit->usable && fit->state == FOC_CALIBRATION_STATE_VALID &&
        fit->failure == FOC_CALIBRATION_FAILURE_NONE && fit->axis == axis &&
        foc_motor_id_positive(fit->rs_ohm) && foc_motor_id_positive(fit->inductance_h);
}

int foc_motor_id_make_model(
    const struct foc_motor_id_result *d,
    const struct foc_motor_id_result *q,
    float max_rs_relative_difference,
    struct foc_motor_rl_model *model
)
{
    if (model == NULL) {
        return FOC_MOTOR_ID_ERR_NULL;
    }
    *model = (struct foc_motor_rl_model){0};
    if (d == NULL || q == NULL) {
        return FOC_MOTOR_ID_ERR_NULL;
    }
    if (!isfinite(max_rs_relative_difference) || max_rs_relative_difference < 0.0f ||
        max_rs_relative_difference >= 1.0f) {
        return FOC_MOTOR_ID_ERR_CONFIG;
    }
    if (!foc_motor_id_fit_usable(d, FOC_MOTOR_ID_D) ||
        !foc_motor_id_fit_usable(q, FOC_MOTOR_ID_Q) ||
        !foc_motor_id_voltage_source_valid(d->source, d->voltage_source) ||
        d->source != q->source || d->voltage_source != q->voltage_source || d->setup_id == 0U ||
        d->setup_id != q->setup_id || d->capture_id == q->capture_id) {
        return FOC_MOTOR_ID_ERR_QUALITY;
    }
    const double maximum_rs = d->rs_ohm > q->rs_ohm ? d->rs_ohm : q->rs_ohm;
    if (fabs((double)d->rs_ohm - q->rs_ohm) > max_rs_relative_difference * maximum_rs) {
        return FOC_MOTOR_ID_ERR_QUALITY;
    }
    *model = (struct foc_motor_rl_model){
        .rs_ohm = (float)(0.5 * ((double)d->rs_ohm + q->rs_ohm)),
        .ld_h = d->inductance_h,
        .lq_h = q->inductance_h,
    };
    return FOC_MOTOR_ID_OK;
}

int foc_motor_pi_design(
    const struct foc_motor_rl_model *model,
    const struct foc_motor_pi_request *request,
    struct foc_motor_pi_candidate *candidate
)
{
    if (candidate == NULL) {
        return FOC_MOTOR_ID_ERR_NULL;
    }
    *candidate = (struct foc_motor_pi_candidate){0};
    if (model == NULL || request == NULL) {
        return FOC_MOTOR_ID_ERR_NULL;
    }
    if (!foc_motor_id_positive(model->rs_ohm) || !foc_motor_id_positive(model->ld_h) ||
        !foc_motor_id_positive(model->lq_h) || !foc_motor_id_positive(request->bandwidth_hz) ||
        !foc_motor_id_positive(request->sample_period_s) || !isfinite(request->update_delay_s) ||
        request->update_delay_s < 0.0f || !foc_motor_id_positive(request->initial_error_a) ||
        !foc_motor_id_positive(request->available_voltage_v)) {
        return FOC_MOTOR_ID_ERR_CONFIG;
    }
    const double omega = FOC_MOTOR_PI_TWO_PI * request->bandwidth_hz;
    const double sample_phase = omega * request->sample_period_s;
    const double delay_phase = omega * request->update_delay_s + 0.5 * sample_phase;
    const double d = omega * model->ld_h;
    const double q = omega * model->lq_h;
    const double integral = omega * model->rs_ohm;
    const double l_min = model->ld_h < model->lq_h ? model->ld_h : model->lq_h;
    const double plant_step = ((double)model->rs_ohm * request->sample_period_s) / l_min;
    /* The continuous design assumes both the loop and the fastest RL pole
     * are resolved by the supplied worst-case sample interval. */
    if (plant_step > FOC_MOTOR_ID_MAX_RL_STEP || sample_phase > FOC_MOTOR_PI_MAX_SAMPLE_PHASE ||
        delay_phase > FOC_MOTOR_PI_MAX_DELAY_PHASE || d > FLT_MAX || q > FLT_MAX ||
        integral > FLT_MAX || d < FLT_MIN || q < FLT_MIN || integral < FLT_MIN) {
        return FOC_MOTOR_ID_ERR_QUALITY;
    }

    /* Current adds this cycle's integral increment before modulation. */
    const float kp = d > q ? (float)d : (float)q;
    const float ki = (float)integral;
    const double integral_step = (double)ki * request->sample_period_s;
    if (integral_step < FLT_MIN || integral_step > FLT_MAX) {
        return FOC_MOTOR_ID_ERR_QUALITY;
    }
    const double first_update = ((double)kp + (float)integral_step) * request->initial_error_a;
    if (first_update > request->available_voltage_v) {
        return FOC_MOTOR_ID_ERR_QUALITY;
    }
    *candidate = (struct foc_motor_pi_candidate){
        .kp_d_v_per_a = (float)d,
        .kp_q_v_per_a = (float)q,
        .ki_v_per_a_s = ki,
        .estimated_initial_voltage_v = (float)first_update,
        .estimated_delay_phase_rad = (float)delay_phase,
    };
    return FOC_MOTOR_ID_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
