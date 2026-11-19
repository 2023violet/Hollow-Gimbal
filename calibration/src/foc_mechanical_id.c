/**
 * @file    foc_mechanical_id.c
 * @brief   Bounded window-integral identification of motor-shaft J and B.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Admit source-qualified endpoints, accumulate normalized window rows without
 * storing history, and publish only a complete physical, identifiable fit.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_mechanical_id.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Macros --------------------------------------------------------------- */

#define FOC_MECHANICAL_ID_MAX_SAMPLES (65535U)
#define FOC_MECHANICAL_ID_HALF_RANGE  (UINT32_C(0x80000000))

/* Private Helpers -------------------------------------------------------------- */

static bool foc_mechanical_id_positive(float value)
{
    return isfinite(value) && value > 0.0f;
}

static bool foc_mechanical_id_config_valid(const struct foc_mechanical_id_config *cfg)
{
    return cfg->setup_id != 0U && cfg->torque_model_id != 0U &&
        (cfg->torque_source == FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ ||
         cfg->torque_source == FOC_MECHANICAL_ID_TORQUE_EQUIVALENT) &&
        (cfg->load_source == FOC_MECHANICAL_ID_LOAD_NEGLIGIBLE ||
         cfg->load_source == FOC_MECHANICAL_ID_LOAD_OBSERVED) &&
        cfg->min_interval_us != 0U && cfg->max_interval_us >= cfg->min_interval_us &&
        cfg->max_interval_us < FOC_MECHANICAL_ID_HALF_RANGE && cfg->intervals_per_window >= 2U &&
        cfg->min_windows >= 3U &&
        cfg->min_windows <= (FOC_MECHANICAL_ID_MAX_SAMPLES - 1U) / cfg->intervals_per_window &&
        (foc_mechanical_id_positive(cfg->torque_constant_nm_per_a) ||
         (cfg->torque_source == FOC_MECHANICAL_ID_TORQUE_EQUIVALENT &&
          cfg->torque_constant_nm_per_a == 0.0f)) &&
        isfinite(cfg->max_abs_id_a) && cfg->max_abs_id_a >= 0.0f &&
        isfinite(cfg->speed_resolution_rad_s) && cfg->speed_resolution_rad_s >= 0.0f &&
        foc_mechanical_id_positive(cfg->max_abs_speed_rad_s) &&
        foc_mechanical_id_positive(cfg->max_abs_torque_nm) &&
        foc_mechanical_id_positive(cfg->min_accel_rms_rad_s2) &&
        foc_mechanical_id_positive(cfg->min_speed_rms_rad_s) &&
        foc_mechanical_id_positive(cfg->min_excitation) && cfg->min_excitation <= 1.0f &&
        foc_mechanical_id_positive(cfg->max_residual_rms_nm) &&
        foc_mechanical_id_positive(cfg->min_inertia_kg_m2) && isfinite(cfg->max_inertia_kg_m2) &&
        cfg->max_inertia_kg_m2 >= cfg->min_inertia_kg_m2 && isfinite(cfg->max_damping_nm_s) &&
        cfg->max_damping_nm_s >= 0.0f &&
        (double)cfg->max_damping_nm_s * (double)cfg->max_interval_us * 1e-6 <=
        0.3 * (double)cfg->min_inertia_kg_m2;
}

static int foc_mechanical_id_invalidate(
    struct foc_mechanical_id_context *context,
    enum foc_calibration_failure failure,
    int status
)
{
    context->result.failure = failure;
    context->result.state = FOC_CALIBRATION_STATE_INVALID;
    return status;
}

static enum foc_calibration_failure foc_mechanical_id_endpoint(
    const struct foc_mechanical_id_context *context,
    const struct foc_mechanical_id_sample *sample,
    double *net_torque
)
{
    const struct foc_mechanical_id_config *cfg = &context->config;
    if (sample->setup_id != cfg->setup_id || sample->torque_model_id != cfg->torque_model_id ||
        sample->capture_id != context->result.capture_id) {
        return FOC_CALIBRATION_FAILURE_INPUT_CHANGED;
    }
    if (!sample->speed_valid || !sample->torque_valid || !sample->frame_confirmed ||
        !sample->interval_confirmed || sample->load_source == FOC_MECHANICAL_ID_LOAD_UNKNOWN ||
        sample->torque_source == FOC_MECHANICAL_ID_TORQUE_UNVERIFIED) {
        return FOC_CALIBRATION_FAILURE_PRECONDITION;
    }
    if (sample->torque_source != cfg->torque_source || sample->load_source != cfg->load_source) {
        return FOC_CALIBRATION_FAILURE_INPUT_CHANGED;
    }
    if (sample->torque_us != sample->speed_us) {
        return FOC_CALIBRATION_FAILURE_TIME_ORDER;
    }
    if (!isfinite(sample->speed_rad_s) || !isfinite(sample->load_nm) ||
        fabsf(sample->speed_rad_s) > cfg->max_abs_speed_rad_s ||
        fabsf(sample->load_nm) > cfg->max_abs_torque_nm) {
        return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
    }
    if (cfg->load_source == FOC_MECHANICAL_ID_LOAD_NEGLIGIBLE && sample->load_nm != 0.0f) {
        return FOC_CALIBRATION_FAILURE_PRECONDITION;
    }
    double torque;
    if (cfg->torque_source == FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ) {
        if (!isfinite(sample->id_a) || !isfinite(sample->iq_a)) {
            return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
        }
        if (fabsf(sample->id_a) > cfg->max_abs_id_a) {
            return FOC_CALIBRATION_FAILURE_PRECONDITION;
        }
        torque = (double)cfg->torque_constant_nm_per_a * sample->iq_a;
    } else {
        torque = sample->torque_nm;
    }
    if (!isfinite(torque) || fabs(torque) > cfg->max_abs_torque_nm) {
        return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
    }
    *net_torque = torque - sample->load_nm;
    return FOC_CALIBRATION_FAILURE_NONE;
}

static bool foc_mechanical_id_commit_window(
    struct foc_mechanical_id_context *context,
    double delta_speed,
    double x,
    double y,
    double z
)
{
    const struct foc_mechanical_id_gram *g = &context->gram;
    const struct foc_mechanical_id_gram next = {
        g->xx + x * x,
        g->yy + y * y,
        g->zz + z * z,
        g->xy + x * y,
        g->xz + x * z,
        g->yz + y * z,
    };
    if (!isfinite(next.xx) || !isfinite(next.yy) || !isfinite(next.zz) || !isfinite(next.xy) ||
        !isfinite(next.xz) || !isfinite(next.yz)) {
        return false;
    }
    context->gram = next;
    context->result.windows++;
    const double direction_floor = 2.0 * context->config.speed_resolution_rad_s;
    context->result.accel_windows += delta_speed > direction_floor;
    context->result.decel_windows += delta_speed < -direction_floor;
    return true;
}

static enum foc_calibration_failure foc_mechanical_id_fit(struct foc_mechanical_id_context *ctx)
{
    struct foc_mechanical_id_result *result = &ctx->result;
    const struct foc_mechanical_id_config *cfg = &ctx->config;
    const struct foc_mechanical_id_gram *g = &ctx->gram;
    const double count = result->windows;
    result->accel_rms_rad_s2 = sqrt(g->xx / count);
    result->speed_rms_rad_s = sqrt(g->yy / count);
    if (result->accel_rms_rad_s2 < cfg->min_accel_rms_rad_s2 ||
        result->speed_rms_rad_s < cfg->min_speed_rms_rad_s || result->accel_windows == 0U ||
        result->decel_windows == 0U) {
        return FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE;
    }
    const double sx = sqrt(g->xx);
    const double sy = sqrt(g->yy);
    const double r = g->xy / sx / sy;
    result->excitation = fmax(0.0, 1.0 - r * r);
    if (fabs(r) < 1.0) {
        result->gram_condition = (1.0 + fabs(r)) / (1.0 - fabs(r));
    }
    if (result->excitation < cfg->min_excitation) {
        return FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE;
    }
    const double a = g->xz / sx;
    const double b = g->yz / sy;
    const double inertia = (a - r * b) / result->excitation / sx;
    const double damping = (b - r * a) / result->excitation / sy;
    if (!isfinite(inertia) || !isfinite(damping)) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    if (inertia < cfg->min_inertia_kg_m2 || inertia > cfg->max_inertia_kg_m2 || damping < 0.0 ||
        damping > cfg->max_damping_nm_s) {
        return FOC_CALIBRATION_FAILURE_RANGE;
    }
    const float j_out = (float)inertia;
    const float b_out = (float)damping;
    const double j = j_out, d = b_out;
    const double xx = j * j * g->xx;
    const double xy = 2.0 * j * d * g->xy;
    const double yy = d * d * g->yy;
    const double xz = 2.0 * j * g->xz;
    const double yz = 2.0 * d * g->yz;
    const double residual2 = ((g->zz + xx + xy + yy) - xz) - yz;
    const double roundoff =
        (8.0 * count + 64.0) * DBL_EPSILON * (g->zz + xx + fabs(xy) + yy + fabs(xz) + fabs(yz));
    if (!isfinite(residual2) || !isfinite(roundoff) || residual2 < -roundoff) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    result->residual_rms_nm = sqrt((fmax(0.0, residual2) + roundoff) / count);
    if (result->residual_rms_nm > cfg->max_residual_rms_nm) {
        return FOC_CALIBRATION_FAILURE_RESIDUAL;
    }
    result->inertia_kg_m2 = j_out;
    result->damping_nm_s = b_out;
    return FOC_CALIBRATION_FAILURE_NONE;
}

/* Public Functions ------------------------------------------------------------- */

int foc_mechanical_id_init(
    struct foc_mechanical_id_context *context,
    const struct foc_mechanical_id_config *config,
    uint32_t capture_id
)
{
    if (context == NULL || config == NULL) {
        return FOC_MECHANICAL_ID_ERR_NULL;
    }
    if (capture_id == 0U || !foc_mechanical_id_config_valid(config)) {
        return FOC_MECHANICAL_ID_ERR_CONFIG;
    }
    *context = (struct foc_mechanical_id_context){
        .config = *config,
        .result = {
            .state = FOC_CALIBRATION_STATE_COLLECTING,
            .setup_id = config->setup_id,
            .torque_model_id = config->torque_model_id,
            .torque_source = config->torque_source,
            .load_source = config->load_source,
            .capture_id = capture_id,
            .torque_constant_nm_per_a = config->torque_constant_nm_per_a,
            .speed_resolution_rad_s = config->speed_resolution_rad_s,
        },
    };
    return FOC_MECHANICAL_ID_OK;
}

int foc_mechanical_id_reset(struct foc_mechanical_id_context *context, uint32_t capture_id)
{
    if (context == NULL) {
        return FOC_MECHANICAL_ID_ERR_NULL;
    }
    if (context->result.state == FOC_CALIBRATION_STATE_UNINITIALIZED) {
        return FOC_MECHANICAL_ID_ERR_STATE;
    }
    if (capture_id == context->result.capture_id) {
        return FOC_MECHANICAL_ID_ERR_CONFIG;
    }
    const struct foc_mechanical_id_config config = context->config;
    return foc_mechanical_id_init(context, &config, capture_id);
}

int foc_mechanical_id_push(
    struct foc_mechanical_id_context *context,
    const struct foc_mechanical_id_sample *sample
)
{
    if (context == NULL || sample == NULL) {
        return FOC_MECHANICAL_ID_ERR_NULL;
    }
    struct foc_mechanical_id_result *result = &context->result;
    if (result->state != FOC_CALIBRATION_STATE_COLLECTING) {
        return FOC_MECHANICAL_ID_ERR_STATE;
    }
    double torque;
    const enum foc_calibration_failure failure =
        foc_mechanical_id_endpoint(context, sample, &torque);
    if (failure != FOC_CALIBRATION_FAILURE_NONE) {
        return foc_mechanical_id_invalidate(
            context,
            failure,
            failure == FOC_CALIBRATION_FAILURE_PRECONDITION ? FOC_MECHANICAL_ID_ERR_PRECONDITION
                                                            : FOC_MECHANICAL_ID_ERR_SAMPLE
        );
    }
    if (result->samples == FOC_MECHANICAL_ID_MAX_SAMPLES) {
        return foc_mechanical_id_invalidate(
            context,
            FOC_CALIBRATION_FAILURE_SAMPLE_COUNT,
            FOC_MECHANICAL_ID_ERR_SAMPLE
        );
    }
    if (result->samples == 0U) {
        if (sample->interval_begin_us != sample->speed_us) {
            return foc_mechanical_id_invalidate(
                context,
                FOC_CALIBRATION_FAILURE_TIME_ORDER,
                FOC_MECHANICAL_ID_ERR_SAMPLE
            );
        }
        result->first_us = sample->speed_us;
        context->window_start_speed_rad_s = sample->speed_rad_s;
    } else {
        const uint32_t interval = sample->speed_us - result->last_us;
        const uint32_t order = sample->sequence - result->last_sequence;
        if (interval < context->config.min_interval_us ||
            interval > context->config.max_interval_us || order == 0U ||
            order >= FOC_MECHANICAL_ID_HALF_RANGE || sample->interval_begin_us != result->last_us) {
            return foc_mechanical_id_invalidate(
                context,
                FOC_CALIBRATION_FAILURE_TIME_ORDER,
                FOC_MECHANICAL_ID_ERR_SAMPLE
            );
        }
        const double half_dt = (double)interval * 0.5e-6;
        const double y = context->speed_integral_rad +
            half_dt * ((double)sample->speed_rad_s + context->previous_speed_rad_s);
        const double z =
            context->torque_integral_nm_s + half_dt * (torque + context->previous_net_torque_nm);
        const uint64_t span = context->window_us + interval;
        const uint32_t intervals = context->intervals + 1U;
        if (!isfinite(y) || !isfinite(z)) {
            return foc_mechanical_id_invalidate(
                context,
                FOC_CALIBRATION_FAILURE_NUMERIC,
                FOC_MECHANICAL_ID_ERR_SAMPLE
            );
        }
        if (intervals == context->config.intervals_per_window) {
            const double hz = 1.0e6 / (double)span;
            const double delta = (double)sample->speed_rad_s - context->window_start_speed_rad_s;
            if (!foc_mechanical_id_commit_window(context, delta, delta * hz, y * hz, z * hz)) {
                return foc_mechanical_id_invalidate(
                    context,
                    FOC_CALIBRATION_FAILURE_NUMERIC,
                    FOC_MECHANICAL_ID_ERR_SAMPLE
                );
            }
            context->speed_integral_rad = 0.0;
            context->torque_integral_nm_s = 0.0;
            context->window_us = 0U;
            context->intervals = 0U;
            context->window_start_speed_rad_s = sample->speed_rad_s;
        } else {
            context->speed_integral_rad = y;
            context->torque_integral_nm_s = z;
            context->window_us = span;
            context->intervals = intervals;
        }
    }
    result->last_us = sample->speed_us;
    result->last_sequence = sample->sequence;
    context->previous_speed_rad_s = sample->speed_rad_s;
    context->previous_net_torque_nm = torque;
    result->samples++;
    return FOC_MECHANICAL_ID_OK;
}

int foc_mechanical_id_finish(
    struct foc_mechanical_id_context *context,
    struct foc_mechanical_id_result *result
)
{
    if (result == NULL) {
        return FOC_MECHANICAL_ID_ERR_NULL;
    }
    *result = (struct foc_mechanical_id_result){0};
    if (context == NULL) {
        return FOC_MECHANICAL_ID_ERR_NULL;
    }
    if (context->result.state != FOC_CALIBRATION_STATE_COLLECTING) {
        *result = context->result;
        return context->result.usable ? FOC_MECHANICAL_ID_OK : FOC_MECHANICAL_ID_ERR_STATE;
    }
    const enum foc_calibration_failure failure =
        context->result.windows < context->config.min_windows || context->intervals != 0U
        ? FOC_CALIBRATION_FAILURE_SAMPLE_COUNT
        : foc_mechanical_id_fit(context);
    if (failure != FOC_CALIBRATION_FAILURE_NONE) {
        const int status =
            foc_mechanical_id_invalidate(context, failure, FOC_MECHANICAL_ID_ERR_QUALITY);
        *result = context->result;
        return status;
    }
    context->result.state = FOC_CALIBRATION_STATE_VALID;
    context->result.usable = true;
    *result = context->result;
    return FOC_MECHANICAL_ID_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
