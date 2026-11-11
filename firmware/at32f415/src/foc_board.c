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

#define FOC_BOARD_ADC_MAX           4095U
#define FOC_BOARD_ADC_FRAME_US      80U
#define FOC_BOARD_ENCODER_FRAME_US  100U
#define FOC_BOARD_ENCODER_GAP_US    2500U
#define FOC_BOARD_ENCODER_MAX_RAD_S 1000.0f
#define FOC_BOARD_RAD_PER_COUNT     (6.283185307179586f / 65536.0f)

/* Private Constants ------------------------------------------------------------ */

static const struct foc_pipeline_config pipeline_config = {
    .current = {{1.0f, 2.4f, 0.0f, 1.0f, 0.0006f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f},
    .adc =
        {3.3f * 11.0f / 4095.0f,
         3.3f / (4095.0f * 0.5f),
         1.0f,
         2048.0f,
         2048.0f,
         2048.0f,
         FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {FOC_BOARD_COMPARE_MAX, FOC_DEVICE_PHASE_ORDER_ABC},
    /* The first usable velocity follows the second encoder frame. */
    .safety = {2500U, 500U, 700U, 1000U, 5U},
    .output_submit = foc_board_output_submit,
};

static const struct foc_runtime_config runtime_config = {
    .h0 = {FOC_BOARD_H0_US, 150U, 100U},
    .h1 = {FOC_BOARD_H1_US, 150U, 150U},
    .supervision = {200U, 150U, 100U},
    .telemetry = {10000U, 500U, 100U},
    .foreground_budget_us = 150U,
    .angle_timeout_us = 650U,
    .motion =
        {true, {7U, 0.0f}, {650U, 2000.0f, 0.00065f}, {0.18f, 1.4f, 0.002f}, {4.0f, 12.0f}, 2.5f},
};

/* Private Helpers -------------------------------------------------------------- */

static bool foc_board_encoder_position(
    struct foc_board_encoder *encoder,
    uint16_t code,
    struct foc_runtime_sensor *sample
)
{
    /* The Legacy CCW mount reverses the raw sensor direction. */
    const uint16_t directed = (uint16_t)(0U - code);
    if (!encoder->previous_seen) {
        encoder->position_rad = (float)directed * FOC_BOARD_RAD_PER_COUNT;
        encoder->previous_seen = true;
        encoder->previous_us = sample->captured_us;
        encoder->previous_code = code;
        /* A single angle cannot establish velocity. */
        return false;
    }

    float dt_s;
    if (encoder->continuity_lost ||
        !foc_board_elapsed_s(
            sample->captured_us,
            encoder->previous_us,
            FOC_BOARD_ENCODER_GAP_US,
            &dt_s
        ) ||
        dt_s == 0.0f) {
        encoder->result = FOC_BOARD_ENCODER_DISCONTINUITY;
        return false;
    }
    const uint16_t wrapped = (uint16_t)(encoder->previous_code - code);
    const int32_t delta = wrapped < 32768U ? (int32_t)wrapped : (int32_t)wrapped - 65536;
    const float step_rad = (float)delta * FOC_BOARD_RAD_PER_COUNT;
    const float velocity_rad_s = step_rad / dt_s;
    if (velocity_rad_s < -FOC_BOARD_ENCODER_MAX_RAD_S ||
        velocity_rad_s > FOC_BOARD_ENCODER_MAX_RAD_S) {
        encoder->result = FOC_BOARD_ENCODER_DISCONTINUITY;
        return false;
    }

    encoder->position_rad += step_rad;
    encoder->previous_us = sample->captured_us;
    encoder->previous_code = code;
    sample->mechanical_rad = encoder->position_rad;
    sample->mechanical_rad_s = velocity_rad_s;
    return true;
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
    if (encoder->previous_seen &&
        !foc_safe_timestamp_fresh(
            sample.captured_us,
            encoder->previous_us,
            FOC_BOARD_ENCODER_GAP_US
        )) {
        encoder->continuity_lost = true;
    }
    if (encoder->result != FOC_BOARD_ENCODER_OVERLAP) {
        encoder->result = transport;
    }
    if (encoder->result == FOC_BOARD_ENCODER_OK) {
        if (!foc_safe_timestamp_fresh(now_us, sample.captured_us, FOC_BOARD_ENCODER_FRAME_US)) {
            encoder->result = FOC_BOARD_ENCODER_TIMEOUT;
        } else {
            sample.valid = foc_board_encoder_position(encoder, code, &sample);
        }
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
