/**
 * @file    test_foc_board.c
 * @brief   Board1 fake-clock, peripheral-event and output transaction tests.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Exercise the same event boundary called by the MCU handlers with synthetic
 * register values and explicit time. Check independent physical expectations.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"

#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const uint16_t ranks[] = {1241U, 2100U, 2074U, 1970U};

/* Private Helpers -------------------------------------------------------------- */

static int test_time(void)
{
    float dt = -1.0f;
    CHECK(foc_board_elapsed_s(0U, 0U, 0U, &dt) && dt == 0.0f);
    CHECK(foc_board_elapsed_s(175U, 25U, 150U, &dt) && fabsf(dt - 0.000150f) < 1e-9f);
    CHECK(foc_board_elapsed_s(0U, UINT32_MAX - 199U, 200U, &dt) && fabsf(dt - 0.000200f) < 1e-9f);
    CHECK(!foc_board_elapsed_s(0U, UINT32_MAX - 199U, 199U, &dt));
    CHECK(!foc_board_elapsed_s(10U, 11U, UINT32_MAX, &dt));
    CHECK(!foc_board_elapsed_s(0x80000000U, 0U, UINT32_MAX, &dt));
    return 0;
}

static int test_adc(void)
{
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_adc adc = {0};
    CHECK(foc_board_init(&runtime, &output, 0U) == FOC_RUNTIME_OK);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    CHECK(output.result == FOC_BOARD_INHIBIT_SOFTWARE);

    const struct foc_device_adc_raw raw = {
        ranks[0],
        ranks[1],
        ranks[2],
        ranks[3],
        FOC_DEVICE_SAMPLE_ALL_VALID,
        0U,
        0U,
    };
    struct foc_device_measurement measurement;
    CHECK(foc_device_decode_sample(&raw, &runtime.pipeline.adc_config, &measurement) == 0);
    CHECK(fabsf(measurement.core.vbus_v - 11.0018f) < 0.002f);
    CHECK(fabsf(measurement.phase_current_a.a - 0.08381f) < 0.0001f);
    CHECK(fabsf(measurement.phase_current_a.b - 0.04190f) < 0.0001f);
    CHECK(fabsf(measurement.phase_current_a.c + 0.12571f) < 0.0001f);
    CHECK(foc_board_adc_begin(&runtime, &adc, 0U));
    CHECK(foc_board_adc_complete(&runtime, &adc, ranks, 4U, 8U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(runtime.adc.seen && runtime.adc.seq == 0U && runtime.adc.produced_us == 0U);
    CHECK(foc_board_adc_begin(&runtime, &adc, 213U));
    CHECK(foc_board_adc_complete(&runtime, &adc, ranks, 4U, 222U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(runtime.interval_us == 213U && runtime.adc.seq == 1U);
    CHECK(foc_board_adc_complete(&runtime, &adc, ranks, 4U, 223U) == FOC_RUNTIME_ERR_ORDER);
    CHECK(runtime.h0.due_count == 2U);
    CHECK(foc_board_adc_begin(&runtime, &adc, 400U));
    CHECK(!foc_board_adc_begin(&runtime, &adc, 600U));
    CHECK(foc_board_adc_complete(&runtime, &adc, ranks, 4U, 608U) == FOC_RUNTIME_ERR_ORDER);
    CHECK(foc_board_adc_begin(&runtime, &adc, 800U));
    CHECK(foc_board_adc_complete(&runtime, &adc, ranks, 3U, 807U) == FOC_RUNTIME_ERR_INPUT);
    CHECK(runtime.h0.due_count == 2U);
    CHECK(foc_board_adc_begin(&runtime, &adc, 1000U));
    const uint16_t invalid[] = {1241U, 4096U, 2048U, 2048U};
    CHECK(foc_board_adc_complete(&runtime, &adc, invalid, 4U, 1008U) == FOC_RUNTIME_ERR_TIMING);
    CHECK(runtime.h0.due_count == 3U && runtime.h0.missed == 3U);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK((runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_SAMPLE_TIMEOUT) != 0U);
    CHECK((runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U);
    CHECK(output.result == FOC_BOARD_INHIBIT_SOFTWARE && output.declined_updates == 0U);
    CHECK(foc_board_adc_begin(&runtime, &adc, 1200U));
    CHECK(foc_board_adc_complete(&runtime, &adc, ranks, 4U, 1281U) == FOC_RUNTIME_ERR_ORDER);
    return 0;
}

static int test_encoder(void)
{
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder = {0};
    struct foc_board_adc adc = {0};
    const uint32_t base = UINT32_MAX - 499U;
    CHECK(foc_board_init(&runtime, &output, base) == FOC_RUNTIME_OK);
    CHECK(foc_board_encoder_begin(&encoder, base));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, base + 70U, FOC_BOARD_ENCODER_OK, 0U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    for (uint32_t offset = 80U; offset < 500U; offset += FOC_BOARD_H0_US) {
        CHECK(foc_board_adc_begin(&runtime, &adc, base + offset));
        CHECK(
            foc_board_adc_complete(&runtime, &adc, ranks, 4U, base + offset + 8U) ==
            FOC_RUNTIME_ERR_ALIGNMENT
        );
    }
    CHECK(foc_board_encoder_begin(&encoder, 0U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 70U, FOC_BOARD_ENCODER_OK, 65535U) ==
        FOC_RUNTIME_OK
    );
    const struct foc_runtime_sensor sample =
        runtime.sensor_slots[atomic_load(&runtime.sensor_slot)].sample;
    CHECK(sample.valid && sample.captured_us == 0U && sample.seq == 1U);
    CHECK(fabsf(sample.mechanical_rad - 0.00009587f) < 1e-8f);
    CHECK(fabsf(sample.mechanical_rad_s - 0.191748f) < 1e-5f);
    CHECK(encoder.feedback.quality == FOC_ENCODER_QUALITY_VALID);
    CHECK(encoder.feedback.speed_valid && encoder.feedback.raw_velocity_rad_s > 0.0f);
    CHECK(sample.mechanical_rad_s == encoder.feedback.filtered_velocity_rad_s);
    CHECK(foc_board_adc_begin(&runtime, &adc, 100U));
    CHECK(foc_board_adc_complete(&runtime, &adc, ranks, 4U, 108U) == FOC_RUNTIME_IDLE);
    CHECK(runtime.angle_prediction_us == 100U && runtime.adc.produced_us == 100U);
    CHECK(foc_board_encoder_begin(&encoder, 500U));
    CHECK(!foc_board_encoder_begin(&encoder, 501U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 570U, FOC_BOARD_ENCODER_OK, 65534U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(encoder.result == FOC_BOARD_ENCODER_OVERLAP && encoder.losses == 1U);
    CHECK(foc_board_encoder_begin(&encoder, 1000U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 1070U, FOC_BOARD_ENCODER_FRAME, 0U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(encoder.result == FOC_BOARD_ENCODER_FRAME && encoder.losses == 2U);
    CHECK(foc_board_encoder_begin(&encoder, 1500U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 1601U, FOC_BOARD_ENCODER_OK, 65533U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(encoder.result == FOC_BOARD_ENCODER_TIMEOUT && encoder.losses == 3U);
    CHECK(foc_board_encoder_begin(&encoder, 2000U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 2070U, FOC_BOARD_ENCODER_OK, 32767U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(encoder.result == FOC_BOARD_ENCODER_DISCONTINUITY && encoder.losses == 4U);
    CHECK(foc_board_encoder_begin(&encoder, 3000U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 3070U, FOC_BOARD_ENCODER_OK, 65533U) ==
        FOC_RUNTIME_ERR_TIMING
    );
    CHECK(
        encoder.result == FOC_BOARD_ENCODER_DISCONTINUITY && encoder.losses == 5U &&
        encoder.continuity_lost
    );
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 3071U, FOC_BOARD_ENCODER_OK, 65533U) ==
        FOC_RUNTIME_ERR_ORDER
    );
    /* An old timestamp can resemble a fresh one after a full clock wrap. */
    CHECK(foc_board_encoder_begin(&encoder, 500U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 570U, FOC_BOARD_ENCODER_OK, 65534U) ==
        FOC_RUNTIME_ERR_ORDER
    );
    CHECK(encoder.result == FOC_BOARD_ENCODER_DISCONTINUITY && encoder.continuity_lost);
    return 0;
}

static int test_output(void)
{
    struct foc_board_output output = {0};
    const struct foc_device_pwm_config pwm = {FOC_BOARD_COMPARE_MAX, FOC_DEVICE_PHASE_ORDER_ABC};
    const struct foc_output duties = {
        .output_enable = 1U,
        .svpwm = {.duty_a = 1.0f, .duty_b = 0.5f, .duty_c = 0.0f},
    };
    struct foc_device_pwm_request request;
    CHECK(foc_device_map_output(&duties, &pwm, &request) == 0);
    CHECK(
        request.channel1_compare == 0U && request.channel2_compare == 14999U &&
        request.channel3_compare == 29999U
    );
    CHECK(!foc_board_output_submit(&output, &request));
    CHECK(output.result == FOC_BOARD_UPDATE_NOT_APPLIED && output.declined_updates == 1U);
    request.channel3_compare++;
    CHECK(!foc_board_output_submit(&output, &request) && output.result == FOC_BOARD_OUTPUT_INVALID);
    request.channel3_compare--;
    request.duty_b = NAN;
    CHECK(!foc_board_output_submit(&output, &request) && output.result == FOC_BOARD_OUTPUT_INVALID);
    CHECK(foc_device_output_zero_duty(&pwm, &request) == 0);
    CHECK(
        !foc_board_output_submit(&output, &request) && output.result == FOC_BOARD_ZERO_DUTY_LOCKED
    );
    CHECK(foc_device_output_active_brake(&pwm, &request) == 0);
    CHECK(
        !foc_board_output_submit(&output, &request) &&
        output.result == FOC_BOARD_ACTIVE_BRAKE_LOCKED
    );
    CHECK(foc_device_output_inhibit(&pwm, &request) == 0);
    CHECK(
        foc_board_output_submit(&output, &request) && output.result == FOC_BOARD_INHIBIT_SOFTWARE
    );
    return 0;
}

static int test_rejected_transaction(void)
{
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder = {0};
    struct foc_runtime_foreground_result foreground;
    CHECK(foc_board_init(&runtime, &output, 0U) == FOC_RUNTIME_OK);
    const struct foc_pipeline_command command = {{0.1f, 0.5f}, 0U, 0U};
    CHECK(foc_runtime_publish_command(&runtime, &command, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_board_encoder_begin(&encoder, 0U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 20U, FOC_BOARD_ENCODER_OK, 100U) ==
        FOC_RUNTIME_ERR_INPUT
    );

    for (uint32_t cycle = 0U; cycle < 7U; ++cycle) {
        const uint32_t time = cycle * FOC_BOARD_H0_US + 80U;
        if (cycle == 3U || cycle == 5U) {
            const uint32_t captured = cycle == 3U ? 500U : 1000U;
            CHECK(foc_board_encoder_begin(&encoder, captured));
            CHECK(
                foc_board_encoder_complete(
                    &runtime,
                    &encoder,
                    captured + 70U,
                    FOC_BOARD_ENCODER_OK,
                    100U
                ) == FOC_RUNTIME_OK
            );
        }
        const struct foc_runtime_fast_input input = {
            .adc = {1241U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, cycle, time},
            .now_us = time + 8U,
            .action = cycle == 4U ? FOC_RUNTIME_ACTION_ARM
                : cycle == 5U     ? FOC_RUNTIME_ACTION_START
                                  : FOC_RUNTIME_ACTION_NONE,
        };
        const int status = foc_runtime_h0_step(&runtime, &input);
        if (cycle == 4U || cycle == 5U) {
            CHECK(status == FOC_RUNTIME_IDLE);
        }
        if (cycle == 6U) {
            CHECK(
                status == FOC_RUNTIME_ERR_CONTROL &&
                runtime.pipeline_status == FOC_PIPELINE_ERR_UPDATE
            );
        }
        (void)foc_runtime_foreground_poll(&runtime, time + 9U, 0U, 0U, &foreground);
    }
    CHECK(output.declined_updates == 1U && output.result == FOC_BOARD_INHIBIT_SOFTWARE);
    CHECK(runtime.pipeline.mock.last_rejected_update.action == FOC_DEVICE_OUTPUT_UPDATE);
    CHECK(
        runtime.pipeline.current.integral_v.d == 0.0f &&
        runtime.pipeline.current.integral_v.q == 0.0f
    );
    CHECK(
        runtime.pipeline.mock.updates == 0U &&
        runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_time() || test_adc() || test_encoder() || test_output() ||
        test_rejected_transaction();
}

/* ---------------------------------------- end of file ----------------------------------------- */
