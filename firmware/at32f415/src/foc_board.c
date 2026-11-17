/**
 * @file    foc_board.c
 * @brief   Reference Board1 configuration and portable peripheral boundary.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Keep nominal scaling, timing and phase conventions local to Board1.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"

#include <stddef.h>

/* Private Macros --------------------------------------------------------------- */

#define FOC_BOARD_ADC_MAX          4095U
#define FOC_BOARD_ADC_FRAME_US     80U
#define FOC_BOARD_ENCODER_FRAME_US 100U
#define FOC_BOARD_ENCODER_GAP_US   2500U

/* Private Constants ------------------------------------------------------------ */

/*
 * Board1 nominal control configuration. Physical conversion factors and loop
 * gains are kept here as the current firmware defaults; they are not a claim
 * that the connected motor has been identified. Units are documented by the
 * owning public structures in core/, g3/, and runtime/.
 */
static const struct foc_pipeline_config pipeline_config = {
    .current =
        {
            .pwm =
                {
                    .vbus_min_v = 1.0f,
                    .voltage_limit_v = 2.4f,
                    .duty_min = 0.0f,
                    .duty_max = 1.0f,
                    .max_dt_s = 0.0006f,
                },
            .kp_v_per_a = {.d = 1.08f, .q = 1.35f},
            .ki_v_per_a_s = {.d = 540.0f, .q = 540.0f},
            .current_limit_a = 2.5f,
        },
    .adc =
        {
            /* 3.3 V ADC full scale and an 11:1 VBUS divider, in V/count. */
            .vbus_v_per_count = 3.3f * 11.0f / (float)FOC_BOARD_ADC_MAX,
            /* 0.5 V/A current amplifier scale, in A/count. */
            .current_a_per_count = 3.3f / ((float)FOC_BOARD_ADC_MAX * 0.5f),
            .vbus_min_v = 1.0f,
            .phase_a_offset_count = 2048.0f,
            .phase_b_offset_count = 2048.0f,
            .phase_c_offset_count = 2048.0f,
            .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
        },
    .pwm =
        {
            .period_counts = FOC_BOARD_COMPARE_MAX,
            .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
        },
    /* The first usable velocity follows the second encoder frame. */
    .safety =
        {
            .command_timeout_us = 2500U,
            .sample_timeout_us = 500U,
            .fast_heartbeat_timeout_us = 700U,
            .foreground_heartbeat_timeout_us = 1000U,
            .invalid_sample_latch_after = 5U,
        },
    .output_submit = foc_board_output_submit,
};

/* H0/H1 and foreground values are nominal Board1 scheduling budgets in us. */
static const struct foc_runtime_config runtime_config = {
    .h0 = {.period_us = FOC_BOARD_H0_US, .max_delay_us = 150U, .budget_us = 100U},
    .h1 = {.period_us = FOC_BOARD_H1_US, .max_delay_us = 150U, .budget_us = 150U},
    .supervision = {.period_us = 200U, .max_delay_us = 150U, .budget_us = 100U},
    .telemetry = {.period_us = 10000U, .max_delay_us = 500U, .budget_us = 100U},
    .foreground_budget_us = 150U,
    .angle_timeout_us = 650U,
    .motion = {
        .enabled = true,
        .rotor = {.pole_pairs = 7U, .electrical_offset_rad = 0.0f},
        .prediction =
            {
                .max_prediction_us = 650U,
                .max_accel_rad_s2 = 2000.0f,
                .max_error_rad = 0.00065f,
            },
        .velocity = {.kp_a_per_rad_s = 0.18f, .ki_a_per_rad = 1.4f, .max_dt_s = 0.002f},
        .position = {.kp_per_s = 4.0f, .velocity_limit_rad_s = 12.0f},
        .iq_limit_a = 2.5f,
        .trajectory = {.enabled = false},
    },
};

static const struct foc_encoder_config encoder_config = {
    .counts_per_revolution = 65536U,
    .direction = FOC_ENCODER_DIRECTION_REVERSED,
    .max_speed_rad_s = 1000.0f,
    .min_dt_us = 1U,
    .max_dt_us = FOC_BOARD_ENCODER_GAP_US,
    .speed_lpf_cutoff_hz = 80.0f,
    .tracker_alpha = 0.85f,
    .tracker_beta = 0.2f,
};

/* Private Helpers -------------------------------------------------------------- */

static int foc_board_encoder_init_sensor(struct foc_board_encoder *encoder)
{
    if (encoder->sensor.initialized) {
        return FOC_ENCODER_STATUS_OK;
    }
    const struct foc_encoder_config *config =
        encoder->config_valid ? &encoder->config : &encoder_config;
    return foc_encoder_init(&encoder->sensor, config);
}

static enum foc_encoder_transport foc_board_encoder_transport(enum foc_board_encoder_result result)
{
    if (result == FOC_BOARD_ENCODER_TIMEOUT) {
        return FOC_ENCODER_TRANSPORT_TIMEOUT;
    }
    if (result != FOC_BOARD_ENCODER_OK) {
        return FOC_ENCODER_TRANSPORT_FRAME_ERROR;
    }
    return FOC_ENCODER_TRANSPORT_OK;
}

static enum foc_board_encoder_result foc_board_encoder_quality(enum foc_encoder_quality quality)
{
    switch (quality) {
    case FOC_ENCODER_QUALITY_TIMEOUT:
        return FOC_BOARD_ENCODER_TIMEOUT;
    case FOC_ENCODER_QUALITY_TRANSPORT_ERROR:
    case FOC_ENCODER_QUALITY_INVALID_SAMPLE:
    case FOC_ENCODER_QUALITY_NUMERIC_ERROR:
        return FOC_BOARD_ENCODER_FRAME;
    case FOC_ENCODER_QUALITY_DISCONTINUITY:
    case FOC_ENCODER_QUALITY_SPEED_OUT_OF_RANGE:
    case FOC_ENCODER_QUALITY_DUPLICATE:
    case FOC_ENCODER_QUALITY_TIME_ORDER_ERROR:
    case FOC_ENCODER_QUALITY_INVALID_INTERVAL:
        return FOC_BOARD_ENCODER_DISCONTINUITY;
    default:
        return FOC_BOARD_ENCODER_OK;
    }
}

static bool foc_board_compares_zero(const struct foc_device_pwm_request *request)
{
    return request->duty_a == 0.0f && request->duty_b == 0.0f && request->duty_c == 0.0f &&
        request->channel1_compare == 0U && request->channel2_compare == 0U &&
        request->channel3_compare == 0U;
}

/* Public Functions ------------------------------------------------------------- */

int foc_board_init(struct foc_runtime *runtime, struct foc_board_output *output, uint32_t now_us)
{
    if (output == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    *output = (struct foc_board_output){0};
    struct foc_pipeline_config config = pipeline_config;
    config.output_arg = output;
    return foc_runtime_init(runtime, &config, &runtime_config, now_us);
}

int foc_board_init_with_calibration(
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    const struct foc_calibration_snapshot *snapshot,
    uint32_t now_us
)
{
    if (runtime == NULL || output == NULL || encoder == NULL ||
        !foc_calibration_snapshot_valid(snapshot) || encoder->sensor.initialized ||
        encoder->config_valid ||
        !foc_calibration_identity_matches(
            &snapshot->source_identity,
            &(struct foc_calibration_identity){
                .adc_full_scale_counts = FOC_BOARD_ADC_MAX,
                .current_a_per_count = pipeline_config.adc.current_a_per_count,
                .encoder_counts_per_revolution = encoder_config.counts_per_revolution,
                .encoder_direction = encoder_config.direction,
                .phase_order = pipeline_config.adc.phase_order,
                .pole_pairs = snapshot->pole_pairs,
            }
        )) {
        return FOC_RUNTIME_ERR_CONFIG;
    }

    struct foc_pipeline_config candidate_pipeline = pipeline_config;
    candidate_pipeline.adc.phase_a_offset_count = snapshot->phase_a_offset_count;
    candidate_pipeline.adc.phase_b_offset_count = snapshot->phase_b_offset_count;
    candidate_pipeline.adc.phase_c_offset_count = snapshot->phase_c_offset_count;
    struct foc_runtime_config candidate_runtime_config = runtime_config;
    candidate_runtime_config.motion.rotor.pole_pairs = snapshot->pole_pairs;
    candidate_runtime_config.motion.rotor.electrical_offset_rad = snapshot->electrical_offset_rad;

    struct foc_encoder_config candidate_encoder = encoder_config;
    candidate_encoder.counts_per_revolution =
        snapshot->source_identity.encoder_counts_per_revolution;
    candidate_encoder.direction = snapshot->runtime_encoder_direction == 1
        ? FOC_ENCODER_DIRECTION_NORMAL
        : FOC_ENCODER_DIRECTION_REVERSED;

    struct foc_board_output candidate_output = {0};
    candidate_pipeline.output_arg = &candidate_output;
    const int status =
        foc_runtime_init(runtime, &candidate_pipeline, &candidate_runtime_config, now_us);
    if (status != FOC_RUNTIME_OK) {
        return status;
    }

    runtime->pipeline.output_arg = output;
    *output = candidate_output;
    *encoder = (struct foc_board_encoder){.config = candidate_encoder, .config_valid = true};
    return FOC_RUNTIME_OK;
}

bool foc_board_elapsed_s(uint32_t now_us, uint32_t previous_us, uint32_t limit_us, float *seconds)
{
    if (seconds == NULL || !foc_safe_timestamp_fresh(now_us, previous_us, limit_us)) {
        return false;
    }
    *seconds = (float)(now_us - previous_us) * 1.0e-6f;
    return true;
}

bool foc_board_adc_begin(struct foc_runtime *runtime, struct foc_board_adc *adc, uint32_t now_us)
{
    if (adc->pending) {
        adc->expired = true;
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_SAMPLE_TIMEOUT);
        return false;
    }
    adc->pending = true;
    adc->captured_us = now_us;
    return true;
}

int foc_board_adc_complete(
    struct foc_runtime *runtime,
    struct foc_board_adc *adc,
    const uint16_t *ranks,
    unsigned int count,
    uint32_t now_us
)
{
    if (!adc->pending) {
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_SAMPLE_INVALID);
        return FOC_RUNTIME_ERR_ORDER;
    }
    adc->pending = false;
    const uint32_t seq = adc->seq++;
    if (adc->expired ||
        !foc_safe_timestamp_fresh(now_us, adc->captured_us, FOC_BOARD_ADC_FRAME_US)) {
        adc->expired = false;
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_SAMPLE_TIMEOUT);
        return FOC_RUNTIME_ERR_ORDER;
    }
    if (ranks == NULL || count != 4U) {
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_SAMPLE_INVALID);
        return FOC_RUNTIME_ERR_INPUT;
    }
    const bool valid = ranks[0] <= FOC_BOARD_ADC_MAX && ranks[1] <= FOC_BOARD_ADC_MAX &&
        ranks[2] <= FOC_BOARD_ADC_MAX && ranks[3] <= FOC_BOARD_ADC_MAX;
    const struct foc_runtime_fast_input input = {
        .adc =
            {ranks[0],
             ranks[1],
             ranks[2],
             ranks[3],
             valid ? FOC_DEVICE_SAMPLE_ALL_VALID : 0U,
             seq,
             adc->captured_us},
        .now_us = now_us,
    };
    if (!valid) {
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_SAMPLE_INVALID);
    }
    return foc_runtime_h0_step(runtime, &input);
}

bool foc_board_encoder_begin(struct foc_board_encoder *encoder, uint32_t now_us)
{
    if (encoder == NULL || foc_board_encoder_init_sensor(encoder) != FOC_ENCODER_STATUS_OK) {
        return false;
    }
    if (encoder->pending) {
        encoder->result = FOC_BOARD_ENCODER_OVERLAP;
        return false;
    }
    encoder->captured_us = now_us;
    encoder->pending = true;
    encoder->result = FOC_BOARD_ENCODER_OK;
    return true;
}

int foc_board_encoder_complete(
    struct foc_runtime *runtime,
    struct foc_board_encoder *encoder,
    uint32_t now_us,
    enum foc_board_encoder_result transport,
    uint16_t code
)
{
    if (!encoder->pending) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    encoder->pending = false;
    struct foc_runtime_sensor sample = {.captured_us = encoder->captured_us, .seq = encoder->seq++};
    if (encoder->result != FOC_BOARD_ENCODER_OVERLAP) {
        encoder->result = transport;
    }
    if (encoder->result != FOC_BOARD_ENCODER_OVERLAP) {
        if (encoder->result == FOC_BOARD_ENCODER_OK &&
            !foc_safe_timestamp_fresh(now_us, sample.captured_us, FOC_BOARD_ENCODER_FRAME_US)) {
            encoder->result = FOC_BOARD_ENCODER_TIMEOUT;
        }
        const enum foc_encoder_transport frame_transport =
            foc_board_encoder_transport(encoder->result);
        const int sensor_status = foc_encoder_update(
            &encoder->sensor,
            sample.captured_us,
            code,
            frame_transport,
            &encoder->feedback
        );
        sample.valid = encoder->feedback.valid;
        sample.mechanical_rad = encoder->feedback.mechanical_rad;
        sample.mechanical_rad_s = encoder->feedback.filtered_velocity_rad_s;
        sample.electrical_phase_valid = sample.valid &&
            encoder->feedback.captured_us == sample.captured_us &&
            foc_rotor_phase_from_counts(
                &runtime->config.motion.rotor,
                encoder->feedback.mechanical_phase_count,
                encoder->sensor.config.counts_per_revolution,
                &sample.theta_elec_rad
            ) == FOC_STATUS_OK;
        if (sensor_status != FOC_ENCODER_STATUS_OK && encoder->result == FOC_BOARD_ENCODER_OK) {
            encoder->result = foc_board_encoder_quality(encoder->feedback.quality);
        }
        encoder->continuity_lost = encoder->feedback.continuity_lost;
    }
    if (encoder->result == FOC_BOARD_ENCODER_OK) {
        encoder->losses = 0U;
    } else if (encoder->losses < UINT32_MAX) {
        encoder->losses++;
    }
    if (encoder->result != FOC_BOARD_ENCODER_OK) {
        /* H0 may preempt H1 before the invalid snapshot is published. */
        foc_runtime_request_inhibit(runtime, 0U);
    }
    const int status = foc_runtime_h1_step(runtime, &sample, now_us, 0U);
    if (sample.valid && status != FOC_RUNTIME_OK) {
        foc_runtime_request_inhibit(runtime, 0U);
    }
    return status;
}

bool foc_board_output_submit(void *argument, const struct foc_device_pwm_request *request)
{
    struct foc_board_output *output = argument;
    if (output == NULL) {
        return false;
    }
    output->result = FOC_BOARD_OUTPUT_INVALID;
    if (request == NULL) {
        return false;
    }
    output->request = *request;
    if (request->action == FOC_DEVICE_OUTPUT_UPDATE) {
        const struct foc_output duties = {
            .output_enable = 1U,
            .svpwm =
                {.duty_a = request->duty_a, .duty_b = request->duty_b, .duty_c = request->duty_c},
        };
        struct foc_device_pwm_request mapped;
        if (request->output_enable_request == 1U &&
            foc_device_map_output(&duties, &pipeline_config.pwm, &mapped) == FOC_DEVICE_STATUS_OK &&
            mapped.channel1_compare == request->channel1_compare &&
            mapped.channel2_compare == request->channel2_compare &&
            mapped.channel3_compare == request->channel3_compare) {
            output->result = FOC_BOARD_UPDATE_NOT_APPLIED;
            output->declined_updates++;
        }
    } else if (foc_board_compares_zero(request)) {
        if (request->action == FOC_DEVICE_OUTPUT_INHIBIT && request->output_enable_request == 0U) {
            output->result = FOC_BOARD_INHIBIT_SOFTWARE;
            return true;
        }
        if (request->output_enable_request == 1U) {
            if (request->action == FOC_DEVICE_OUTPUT_ZERO_DUTY) {
                output->result = FOC_BOARD_ZERO_DUTY_LOCKED;
            } else if (request->action == FOC_DEVICE_OUTPUT_ACTIVE_BRAKE) {
                output->result = FOC_BOARD_ACTIVE_BRAKE_LOCKED;
            }
        }
    }
    return false;
}

/* ---------------------------------------- end of file ----------------------------------------- */
