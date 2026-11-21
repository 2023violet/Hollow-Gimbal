/**
 * @file    foc_virtual_bootstrap_capture.c
 * @brief   Bounded Host-only, open-circuit reference capture.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Implementation role
 * -------------------
 * Collect synchronous measurements while a torque-limited external shaft
 * drive moves an open-winding motor. The Runtime stays inhibited throughout.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_bootstrap_capture.h"
#include "foc_virtual_bootstrap_instrument.h"
#include "foc_virtual_calibration_trial.h"

#include <math.h>
#include <stddef.h>

/* Private Constants ------------------------------------------------------------ */

#define FOC_BOOT_STEP_US     250U
#define FOC_BOOT_SAMPLE_US   4000U
#define FOC_BOOT_SETTLING_US 280000U
#define FOC_BOOT_TOTAL_US                                                                        \
    (FOC_BOOTSTRAP_SWEEPS *                                                                      \
         (FOC_BOOT_SETTLING_US + FOC_VIRTUAL_BOOTSTRAP_SAMPLES_PER_SWEEP * FOC_BOOT_SAMPLE_US) + \
     FOC_BOOT_SETTLING_US)

static const double FOC_BOOT_SPEED_RAD_S = 8.0;
static const double FOC_BOOT_FOLLOWING_ERROR_RAD_S = 0.8;
static const double FOC_BOOT_MAX_SPEED_RAD_S = 10.0;
static const double FOC_BOOT_TORQUE_LIMIT_NM = 0.3;
static const double FOC_BOOT_MIN_SIGNAL_V = 0.01;
static const double FOC_BOOT_ZERO_CURRENT_LIMIT_A = 0.05;
static const double FOC_BOOT_SQRT3 = 1.73205080756887729353;

/* Private Helpers -------------------------------------------------------------- */

static int check_range(
    const struct foc_virtual_motor *trial,
    const struct foc_virtual_current_profile *profile,
    struct foc_virtual_bootstrap_capture_result *result,
    uint32_t sequence,
    double signal_v
)
{
    struct foc_device_adc_raw raw;
    struct foc_device_measurement adc;
    if (foc_virtual_current_acquire_adc(profile, trial, sequence, result->completed_us, &raw) !=
            0 ||
        foc_device_decode_sample(&raw, &profile->adc, &adc) != FOC_DEVICE_STATUS_OK) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ADC;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_MEASUREMENT;
    }
    const double current_limit_a =
        fmin(profile->current.current_limit_a, FOC_BOOT_ZERO_CURRENT_LIMIT_A);
    if (signal_v * FOC_BOOT_SQRT3 >= adc.core.vbus_v ||
        fabs(adc.phase_current_a.a) > current_limit_a ||
        fabs(adc.phase_current_a.b) > current_limit_a ||
        fabs(adc.phase_current_a.c) > current_limit_a) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LIMIT;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_LIMIT;
    }
    return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OK;
}

static int advance(
    struct foc_virtual_motor *trial,
    const struct foc_virtual_current_profile *profile,
    const struct foc_virtual_bootstrap_capture_request *request,
    struct foc_virtual_bootstrap_capture_result *result,
    double speed_rad_s,
    uint32_t duration_us
)
{
    for (uint32_t elapsed = 0U; elapsed < duration_us; elapsed += FOC_BOOT_STEP_US) {
        const uint32_t next_us = result->completed_us + FOC_BOOT_STEP_US;
        const int permitted = request->permit(request->argument, next_us);
        if (permitted != 0) {
            result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_GATE;
            return permitted;
        }
        struct foc_virtual_bootstrap_shaft shaft;
        double alpha_v;
        double beta_v;
        if (foc_virtual_motor_step_open_circuit(
                trial,
                speed_rad_s,
                FOC_BOOT_TORQUE_LIMIT_NM,
                (double)FOC_BOOT_STEP_US * 1.0e-6
            ) != 0 ||
            foc_virtual_motor_measure_shaft(trial, &shaft) != 0 ||
            foc_virtual_motor_measure_open_circuit(trial, &alpha_v, &beta_v) != 0) {
            result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OPEN_CIRCUIT;
            return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_MEASUREMENT;
        }
        result->completed_us = next_us;
        if (fabs(shaft.speed_rad_s) > FOC_BOOT_MAX_SPEED_RAD_S) {
            result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LIMIT;
            return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_LIMIT;
        }
        const int status =
            check_range(trial, profile, result, result->count + 1U, hypot(alpha_v, beta_v));
        if (status != 0) {
            return status;
        }
    }
    return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OK;
}

static int acquire(
    const struct foc_virtual_motor *trial,
    const struct foc_virtual_current_profile *profile,
    const struct foc_virtual_bootstrap_capture_request *request,
    struct foc_virtual_bootstrap_capture_result *result,
    uint32_t sweep,
    double target_rad_s
)
{
    struct foc_virtual_bootstrap_shaft shaft;
    double alpha_v;
    double beta_v;
    if (foc_virtual_motor_measure_shaft(trial, &shaft) != 0 ||
        foc_virtual_motor_measure_open_circuit(trial, &alpha_v, &beta_v) != 0) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OPEN_CIRCUIT;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_MEASUREMENT;
    }
    if (fabs(shaft.speed_rad_s - target_rad_s) > FOC_BOOT_FOLLOWING_ERROR_RAD_S) {
        result->reason = fabs(shaft.speed_rad_s) <= FOC_BOOT_FOLLOWING_ERROR_RAD_S
            ? FOC_VIRTUAL_BOOTSTRAP_CAPTURE_STALLED
            : FOC_VIRTUAL_BOOTSTRAP_CAPTURE_FOLLOWING;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_EXCITATION;
    }

    uint16_t code;
    if (request->encoder_unavailable ||
        foc_virtual_current_acquire_encoder(profile, trial, &code) != 0) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ENCODER;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_MEASUREMENT;
    }
    const uint32_t sequence = result->count + 1U;
    const double signal_v = hypot(alpha_v, beta_v);
    if (signal_v < FOC_BOOT_MIN_SIGNAL_V) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LOW_SIGNAL;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_EXCITATION;
    }
    if (request->trial != NULL) {
        const uint32_t seed = request->trial->seed;
        const uint32_t tick = result->completed_us;
        alpha_v += 0.00025 * foc_virtual_cal_trial_dither(seed, tick, 4U);
        beta_v += 0.00025 * foc_virtual_cal_trial_dither(seed, tick, 5U);
    }

    result->samples[result->count++] = (struct foc_bootstrap_sample){
        .identity = request->identity,
        .capture = request->capture,
        .captured_us = result->completed_us,
        .sequence = sequence,
        .sweep = sweep,
        .raw_code = code,
        .shaft_rad_s = (float)shaft.speed_rad_s,
        .emf_alpha_v = (float)alpha_v,
        .emf_beta_v = (float)beta_v,
        .valid = true,
        .output_inhibited = true,
    };
    return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OK;
}

/* Public Functions ------------------------------------------------------------- */

int foc_virtual_bootstrap_capture(
    const struct foc_virtual_motor *motor,
    const struct foc_virtual_current_profile *profile,
    const struct foc_virtual_bootstrap_capture_request *request,
    struct foc_virtual_bootstrap_capture_result *result
)
{
    if (result == NULL) {
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_SETUP;
    }
    *result = (struct foc_virtual_bootstrap_capture_result){0};
    if (motor == NULL || profile == NULL || request == NULL || request->permit == NULL ||
        request->capture == 0U || request->started_us > UINT32_MAX - FOC_BOOT_TOTAL_US ||
        !isfinite(profile->current.current_limit_a) || profile->current.current_limit_a <= 0.0f) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_SETUP;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_SETUP;
    }
    result->completed_us = request->started_us;
    const struct foc_calibration_identity *identity = &request->identity.acquisition;
    if (identity->adc_full_scale_counts != 4095U ||
        identity->phase_order != (uint32_t)profile->adc.phase_order ||
        identity->phase_order != (uint32_t)profile->pwm.phase_order ||
        identity->encoder_counts_per_revolution != profile->encoder.counts_per_revolution ||
        identity->current_a_per_count != profile->adc.current_a_per_count) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_IDENTITY;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_IDENTITY;
    }
    int status = request->permit(request->argument, result->completed_us);
    if (status != FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OK) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_GATE;
        return status;
    }
    struct foc_virtual_motor trial = *motor;
    struct foc_virtual_bootstrap_shaft shaft;
    if (foc_virtual_motor_measure_shaft(&trial, &shaft) != 0 ||
        fabs(shaft.speed_rad_s) > FOC_BOOT_FOLLOWING_ERROR_RAD_S) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_SETUP;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_SETUP;
    }
    uint16_t raw_initial;
    if (foc_virtual_current_acquire_encoder(profile, &trial, &raw_initial) != 0) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ENCODER;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_MEASUREMENT;
    }
    double alpha_initial;
    double beta_initial;
    if (foc_virtual_motor_measure_open_circuit(&trial, &alpha_initial, &beta_initial) != 0) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OPEN_CIRCUIT;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_MEASUREMENT;
    }
    status = check_range(&trial, profile, result, 0U, hypot(alpha_initial, beta_initial));
    if (status != 0) {
        return status;
    }

    const double first_speed = request->trial != NULL && request->trial->reverse_first
        ? -FOC_BOOT_SPEED_RAD_S
        : FOC_BOOT_SPEED_RAD_S;
    for (uint32_t sweep = 0U; sweep < FOC_BOOTSTRAP_SWEEPS; ++sweep) {
        const double target = sweep % 2U == 0U ? first_speed : -first_speed;
        status = advance(&trial, profile, request, result, target, FOC_BOOT_SETTLING_US);
        for (uint32_t i = 0U; i < FOC_VIRTUAL_BOOTSTRAP_SAMPLES_PER_SWEEP && status == 0; ++i) {
            status = advance(&trial, profile, request, result, target, FOC_BOOT_SAMPLE_US);
            if (status == 0) {
                status = acquire(&trial, profile, request, result, sweep, target);
            }
        }
        if (status != 0) {
            return status;
        }
    }
    status = advance(&trial, profile, request, result, 0.0, FOC_BOOT_SETTLING_US);
    if (status != 0) {
        return status;
    }
    if (foc_virtual_motor_measure_shaft(&trial, &shaft) != 0 ||
        fabs(shaft.speed_rad_s) > FOC_BOOT_FOLLOWING_ERROR_RAD_S) {
        result->reason = FOC_VIRTUAL_BOOTSTRAP_CAPTURE_FOLLOWING;
        return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_EXCITATION;
    }
    return FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OK;
}

const char *foc_virtual_bootstrap_capture_reason_name(
    enum foc_virtual_bootstrap_capture_reason reason
)
{
    switch (reason) {
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_NONE:
        return "none";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_SETUP:
        return "invalid_setup";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_IDENTITY:
        return "instrument_identity";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_GATE:
        return "capture_revoked";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OPEN_CIRCUIT:
        return "open_winding_required";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_STALLED:
        return "shaft_stalled";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_FOLLOWING:
        return "shaft_following";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ENCODER:
        return "encoder_transport";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ADC:
        return "adc_measurement";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LIMIT:
        return "measurement_limit";
    case FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LOW_SIGNAL:
        return "weak_emf";
    default:
        return "unknown";
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
