/**
 * @file    foc_flux_id.c
 * @brief   Stream adjacent q-axis voltage intervals into signed magnet-flux fits.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Implementation role
 * -------------------
 * Validate acquisition evidence and co-time aligned endpoints before accumulating
 * the original window averages and no-intercept least squares.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_flux_id.h"
#include "foc_calibration_math.h"

#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

static bool positive(double value)
{
    return isfinite(value) && value > 0.0;
}

static bool identity_valid(const struct foc_flux_id_identity *id)
{
    return id->session != 0U && id->capture != 0U && id->motor != 0U && id->dq_frame != 0U &&
        id->current_chain != 0U && id->voltage_chain != 0U && id->speed_chain != 0U &&
        id->clock != 0U && id->rl_revision != 0U && id->poles_revision != 0U &&
        id->phase_order <= 1U && id->pole_pairs != 0U;
}

static bool
same_identity(const struct foc_flux_id_identity *a, const struct foc_flux_id_identity *b)
{
    return a->session == b->session && a->capture == b->capture && a->motor == b->motor &&
        a->dq_frame == b->dq_frame && a->current_chain == b->current_chain &&
        a->voltage_chain == b->voltage_chain && a->speed_chain == b->speed_chain &&
        a->clock == b->clock && a->rl_revision == b->rl_revision &&
        a->poles_revision == b->poles_revision && a->phase_order == b->phase_order &&
        a->pole_pairs == b->pole_pairs;
}

static bool config_valid(const struct foc_flux_id_config *c)
{
    const bool voltage = c->source == FOC_MOTOR_ID_VIRTUAL
        ? c->voltage_source == FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL
        : c->source == FOC_MOTOR_ID_MEASURED &&
            (c->voltage_source == FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL ||
             c->voltage_source == FOC_MOTOR_ID_VOLTAGE_CALIBRATED_TERMINAL ||
             c->voltage_source == FOC_MOTOR_ID_VOLTAGE_COMMANDED);
    return voltage && identity_valid(&c->identity) && c->rl_qualified && positive(c->rl.rs_ohm) &&
        positive(c->rl.ld_h) && positive(c->rl.lq_h) && c->interval_us != 0U &&
        c->intervals_per_window >= 2U &&
        (uint64_t)c->interval_us * c->intervals_per_window < UINT32_C(0x80000000) &&
        c->min_windows_per_direction >= 2U &&
        (uint64_t)c->min_windows_per_direction * 2U <= UINT32_MAX / c->intervals_per_window &&
        positive(c->max_abs_current_a) && positive(c->max_abs_vq_v) &&
        positive(c->max_abs_speed_rad_s) && positive(c->min_abs_window_speed_rad_s) &&
        c->min_abs_window_speed_rad_s < c->max_abs_speed_rad_s &&
        positive(c->min_speed_span_rad_s) && c->min_speed_span_rad_s < c->max_abs_speed_rad_s &&
        positive(c->min_speed_rms_rad_s) && c->min_speed_rms_rad_s <= c->max_abs_speed_rad_s &&
        positive(c->min_flux_wb) && positive(c->max_flux_wb) && c->min_flux_wb < c->max_flux_wb &&
        positive(c->max_residual_rms_v) && positive(c->max_direction_difference_relative) &&
        c->max_direction_difference_relative < 1.0;
}

static enum foc_calibration_failure
reject(struct foc_flux_id *context, enum foc_calibration_failure reason)
{
    context->result.state = FOC_CALIBRATION_STATE_INVALID;
    context->result.failure = reason;
    context->result.flux_provenance = FOC_FLUX_ID_PROVENANCE_NONE;
    context->result.kt_provenance = FOC_FLUX_ID_PROVENANCE_NONE;
    context->result.flux_wb = 0.0f;
    context->result.ke_phase_peak_v_per_mechanical_rad_s = 0.0f;
    context->result.kt_nm_per_peak_q_amp = 0.0f;
    return reason;
}

static enum foc_calibration_failure
check_interval(const struct foc_flux_id *ctx, const struct foc_flux_id_interval *s)
{
    const struct foc_flux_id_config *c = &ctx->config;
    if (!same_identity(&c->identity, &s->identity) || c->source != s->source ||
        c->voltage_source != s->voltage_source) {
        return FOC_CALIBRATION_FAILURE_CONFIG_MISMATCH;
    }
    if (s->end_us - s->begin_us != c->interval_us || s->voltage_begin_us != s->begin_us ||
        s->voltage_end_us != s->end_us || s->speed_begin_us != s->begin_us ||
        s->speed_end_us != s->end_us ||
        ((uint64_t)ctx->result.intervals + 1U) * c->interval_us >= UINT32_C(0x80000000) ||
        (ctx->result.intervals != 0U && !ctx->segment_break &&
         (s->begin_us != ctx->result.last_us || s->sequence != ctx->last_sequence + UINT32_C(1)))) {
        return FOC_CALIBRATION_FAILURE_TIME_ORDER;
    }
    if (!s->current_valid || !s->voltage_valid || !s->speed_valid || !isfinite(s->id_begin_a) ||
        !isfinite(s->iq_begin_a) || !isfinite(s->id_end_a) || !isfinite(s->iq_end_a) ||
        !isfinite(s->mean_vq_v) || !isfinite(s->mean_omega_e_rad_s) ||
        foc_calibration_sqrt(
            (double)s->id_begin_a * s->id_begin_a + (double)s->iq_begin_a * s->iq_begin_a
        ) > c->max_abs_current_a ||
        foc_calibration_sqrt(
            (double)s->id_end_a * s->id_end_a + (double)s->iq_end_a * s->iq_end_a
        ) > c->max_abs_current_a ||
        fabs(s->mean_vq_v) > c->max_abs_vq_v ||
        fabs(s->mean_omega_e_rad_s) > c->max_abs_speed_rad_s) {
        return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
    }
    /* A COMMANDED capture declares its voltage unwitnessed; others must witness it. */
    if (!s->frame_confirmed || !s->output_confirmed ||
        (!s->terminal_voltage_witnessed && s->voltage_source != FOC_MOTOR_ID_VOLTAGE_COMMANDED) ||
        s->saturated) {
        return FOC_CALIBRATION_FAILURE_PRECONDITION;
    }
    if (ctx->result.intervals != 0U && !ctx->segment_break &&
        (s->id_begin_a != ctx->previous_id_a || s->iq_begin_a != ctx->previous_iq_a)) {
        return FOC_CALIBRATION_FAILURE_INPUT_CHANGED;
    }
    if (s->mean_omega_e_rad_s == 0.0 ||
        (ctx->window_intervals != 0U &&
         ((ctx->speed_sum > 0.0) != (s->mean_omega_e_rad_s > 0.0)))) {
        return FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE;
    }
    return FOC_CALIBRATION_FAILURE_NONE;
}

static enum foc_calibration_failure close_window(struct foc_flux_id *ctx)
{
    const struct foc_flux_id_config *c = &ctx->config;
    const double n = c->intervals_per_window;
    const double dt = (double)c->interval_us / 1000000.0;
    const double emf =
        (ctx->voltage_sum - c->rl.lq_h * (ctx->previous_iq_a - ctx->window_iq_begin_a) / dt) / n;
    const double speed = ctx->speed_sum / n;
    const double magnitude = fabs(speed);
    if (!isfinite(emf) || !isfinite(speed)) {
        return reject(ctx, FOC_CALIBRATION_FAILURE_NUMERIC);
    }
    if (magnitude < c->min_abs_window_speed_rad_s) {
        return reject(ctx, FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE);
    }
    struct foc_flux_id_fit *fit = &ctx->fits[speed > 0.0 ? 0U : 1U];
    struct foc_flux_id_direction *direction =
        speed > 0.0 ? &ctx->result.positive : &ctx->result.negative;
    if (direction->windows == 0U) {
        direction->minimum_abs_speed_rad_s = magnitude;
        direction->maximum_abs_speed_rad_s = magnitude;
    } else {
        direction->minimum_abs_speed_rad_s =
            foc_calibration_fmin(direction->minimum_abs_speed_rad_s, magnitude);
        direction->maximum_abs_speed_rad_s =
            foc_calibration_fmax(direction->maximum_abs_speed_rad_s, magnitude);
    }
    ++direction->windows;
    fit->speed2 += speed * speed;
    fit->voltage2 += emf * emf;
    fit->voltage_speed += emf * speed;
    if (!isfinite(fit->speed2) || !isfinite(fit->voltage2) || !isfinite(fit->voltage_speed)) {
        return reject(ctx, FOC_CALIBRATION_FAILURE_NUMERIC);
    }
    ctx->window_intervals = 0U;
    ctx->voltage_sum = 0.0;
    ctx->speed_sum = 0.0;
    return FOC_CALIBRATION_FAILURE_NONE;
}

static bool direction_result(const struct foc_flux_id_fit *fit, struct foc_flux_id_direction *out)
{
    const double flux = fit->voltage_speed / fit->speed2;
    const double error_energy = fit->voltage2 - flux * fit->voltage_speed;
    if (!isfinite(flux) || !isfinite(error_energy)) {
        return false;
    }
    out->flux_wb = flux;
    out->residual_rms_v =
        foc_calibration_sqrt(foc_calibration_fmax(0.0, error_energy) / out->windows);
    return true;
}

static enum foc_calibration_failure calculate(struct foc_flux_id *ctx)
{
    struct foc_flux_id_result *out = &ctx->result;
    const struct foc_flux_id_config *c = &ctx->config;
    const struct foc_flux_id_fit *fit = ctx->fits;
    if (ctx->window_intervals != 0U) {
        return FOC_CALIBRATION_FAILURE_SAMPLE_COUNT;
    }
    if (out->positive.windows < c->min_windows_per_direction ||
        out->negative.windows < c->min_windows_per_direction) {
        return FOC_CALIBRATION_FAILURE_SAMPLE_COUNT;
    }
    const double windows = (double)out->positive.windows + out->negative.windows;
    const double speed2 = fit[0].speed2 + fit[1].speed2;
    if (!isfinite(speed2)) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    out->omega_e_rms_rad_s = foc_calibration_sqrt(speed2 / windows);
    if (fit[0].speed2 <= 0.0 || fit[1].speed2 <= 0.0 ||
        out->omega_e_rms_rad_s < c->min_speed_rms_rad_s ||
        out->positive.maximum_abs_speed_rad_s - out->positive.minimum_abs_speed_rad_s <
            c->min_speed_span_rad_s ||
        out->negative.maximum_abs_speed_rad_s - out->negative.minimum_abs_speed_rad_s <
            c->min_speed_span_rad_s) {
        return FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE;
    }
    const double dot = fit[0].voltage_speed + fit[1].voltage_speed;
    const double flux = dot / speed2;
    const double energy = fit[0].voltage2 + fit[1].voltage2;
    const double error_energy = energy - flux * dot;
    if (!isfinite(flux) || !isfinite(error_energy) || !direction_result(&fit[0], &out->positive) ||
        !direction_result(&fit[1], &out->negative)) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    out->residual_rms_v = foc_calibration_sqrt(foc_calibration_fmax(0.0, error_energy) / windows);
    if (flux < c->min_flux_wb || flux > c->max_flux_wb) {
        return FOC_CALIBRATION_FAILURE_RANGE;
    }
    const double difference = fabs(out->positive.flux_wb - out->negative.flux_wb);
    out->direction_difference_relative = difference / flux;
    if (out->residual_rms_v > c->max_residual_rms_v ||
        difference > c->max_direction_difference_relative * flux) {
        return FOC_CALIBRATION_FAILURE_RESIDUAL;
    }
    const double poles = c->identity.pole_pairs;
    out->flux_wb = (float)flux;
    out->ke_phase_peak_v_per_mechanical_rad_s = (float)(poles * flux);
    out->kt_nm_per_peak_q_amp = (float)(1.5 * poles * flux);
    if (!positive(out->flux_wb) || !positive(out->ke_phase_peak_v_per_mechanical_rad_s) ||
        !positive(out->kt_nm_per_peak_q_amp)) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    out->state = FOC_CALIBRATION_STATE_VALID;
    out->flux_provenance = FOC_FLUX_FROM_MEASUREMENT;
    out->kt_provenance = FOC_KT_DERIVED_FROM_FLUX;
    return FOC_CALIBRATION_FAILURE_NONE;
}

/* Public Functions ------------------------------------------------------------- */

enum foc_calibration_failure
foc_flux_id_init(struct foc_flux_id *context, const struct foc_flux_id_config *config)
{
    if (context == NULL) {
        return FOC_CALIBRATION_FAILURE_INVALID_CONFIG;
    }
    if (config == NULL || !config_valid(config)) {
        *context = (struct foc_flux_id){0};
        return reject(context, FOC_CALIBRATION_FAILURE_INVALID_CONFIG);
    }
    *context = (struct foc_flux_id){
        .config = *config,
        .result = {
            .identity = config->identity,
            .rl = config->rl,
            .source = config->source,
            .voltage_source = config->voltage_source,
            .state = FOC_CALIBRATION_STATE_COLLECTING,
        },
    };
    return FOC_CALIBRATION_FAILURE_NONE;
}

enum foc_calibration_failure
foc_flux_id_push(struct foc_flux_id *context, const struct foc_flux_id_interval *sample)
{
    if (context == NULL || context->result.state != FOC_CALIBRATION_STATE_COLLECTING) {
        return FOC_CALIBRATION_FAILURE_PRECONDITION;
    }
    if (sample == NULL) {
        return reject(context, FOC_CALIBRATION_FAILURE_INVALID_SAMPLE);
    }
    const enum foc_calibration_failure failure = check_interval(context, sample);
    if (failure != FOC_CALIBRATION_FAILURE_NONE) {
        return reject(context, failure);
    }
    if (context->result.intervals == 0U) {
        context->result.first_us = sample->begin_us;
    }
    if (context->window_intervals == 0U) {
        context->window_iq_begin_a = sample->iq_begin_a;
    }
    const struct foc_motor_rl_model *rl = &context->config.rl;
    const double omega = sample->mean_omega_e_rad_s;
    const double id = 0.5 * (sample->id_begin_a + sample->id_end_a);
    const double iq = 0.5 * (sample->iq_begin_a + sample->iq_end_a);
    context->voltage_sum += sample->mean_vq_v - rl->rs_ohm * iq - omega * rl->ld_h * id;
    context->speed_sum += omega;
    context->previous_id_a = sample->id_end_a;
    context->previous_iq_a = sample->iq_end_a;
    context->result.last_us = sample->end_us;
    context->last_sequence = sample->sequence;
    context->segment_break = false;
    ++context->result.intervals;
    ++context->window_intervals;
    return context->window_intervals == context->config.intervals_per_window
        ? close_window(context)
        : FOC_CALIBRATION_FAILURE_NONE;
}

void foc_flux_id_break(struct foc_flux_id *context)
{
    if (context == NULL || context->result.state != FOC_CALIBRATION_STATE_COLLECTING) {
        return;
    }
    context->window_intervals = 0U;
    context->voltage_sum = 0.0;
    context->speed_sum = 0.0;
    context->segment_break = true;
}

enum foc_calibration_failure
foc_flux_id_finish(struct foc_flux_id *context, struct foc_flux_id_result *result)
{
    if (context == NULL || result == NULL) {
        return FOC_CALIBRATION_FAILURE_INVALID_CONFIG;
    }
    if (context->result.state == FOC_CALIBRATION_STATE_COLLECTING) {
        const enum foc_calibration_failure failure = calculate(context);
        if (failure != FOC_CALIBRATION_FAILURE_NONE) {
            reject(context, failure);
        }
    } else if (context->result.state == FOC_CALIBRATION_STATE_UNINITIALIZED) {
        reject(context, FOC_CALIBRATION_FAILURE_PRECONDITION);
    }
    *result = context->result;
    return result->failure;
}

void foc_flux_id_cancel(struct foc_flux_id *context)
{
    if (context != NULL) {
        reject(context, FOC_CALIBRATION_FAILURE_PRECONDITION);
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */

