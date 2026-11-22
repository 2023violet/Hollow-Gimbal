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
#include "foc_adc_test_clock.h"

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
    struct foc_adc_test_clock clock = {0};
    CHECK(foc_board_adc_init_timing(&adc, 0U, 0U, 0U));
    struct foc_board_adc_event event = foc_adc_test_event(&clock, 40U);
    CHECK(foc_board_adc_begin(&runtime, &adc, &event, false));
    event = foc_adc_test_event(&clock, 48U);
    CHECK(
        foc_board_adc_complete(&runtime, &adc, &output, ranks, 4U, &event, true) ==
        FOC_RUNTIME_ERR_ALIGNMENT
    );
    CHECK(runtime.adc.seen && runtime.adc.seq == 0U && runtime.adc.produced_us == 40U);
    event = foc_adc_test_event(&clock, 253U);
    CHECK(foc_board_adc_begin(&runtime, &adc, &event, false));
    event = foc_adc_test_event(&clock, 262U);
    CHECK(
        foc_board_adc_complete(&runtime, &adc, &output, ranks, 4U, &event, true) ==
        FOC_RUNTIME_ERR_ALIGNMENT
    );
    CHECK(runtime.interval_us == 213U && runtime.adc.seq == 1U);
    event = foc_adc_test_event(&clock, 263U);
    CHECK(
        foc_board_adc_complete(&runtime, &adc, &output, ranks, 4U, &event, true) ==
        FOC_RUNTIME_ERR_ORDER
    );
    CHECK(runtime.h0.due_count == 2U);
    event = foc_adc_test_event(&clock, 440U);
    CHECK(foc_board_adc_begin(&runtime, &adc, &event, false));
    event = foc_adc_test_event(&clock, 640U);
    CHECK(!foc_board_adc_begin(&runtime, &adc, &event, false));
    event = foc_adc_test_event(&clock, 648U);
    CHECK(
        foc_board_adc_complete(&runtime, &adc, &output, ranks, 4U, &event, true) ==
        FOC_RUNTIME_ERR_ORDER
    );
    event = foc_adc_test_event(&clock, 840U);
    CHECK(foc_board_adc_begin(&runtime, &adc, &event, false));
    event = foc_adc_test_event(&clock, 847U);
    CHECK(
        foc_board_adc_complete(&runtime, &adc, &output, ranks, 3U, &event, true) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(runtime.h0.due_count == 2U);
    event = foc_adc_test_event(&clock, 1040U);
    CHECK(foc_board_adc_begin(&runtime, &adc, &event, false));
    const uint16_t invalid[] = {1241U, 4096U, 2048U, 2048U};
    event = foc_adc_test_event(&clock, 1048U);
    CHECK(
        foc_board_adc_complete(&runtime, &adc, &output, invalid, 4U, &event, true) ==
        FOC_RUNTIME_ERR_TIMING
    );
    CHECK(runtime.h0.due_count == 3U && runtime.h0.missed == 3U);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK((runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_SAMPLE_TIMEOUT) != 0U);
    CHECK((runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U);
    CHECK(output.result == FOC_BOARD_INHIBIT_SOFTWARE && output.declined_updates == 0U);
    event = foc_adc_test_event(&clock, 1240U);
    CHECK(foc_board_adc_begin(&runtime, &adc, &event, false));
    event = foc_adc_test_event(&clock, 1321U);
    CHECK(
        foc_board_adc_complete(&runtime, &adc, &output, ranks, 4U, &event, true) ==
        FOC_RUNTIME_ERR_ORDER
    );
    return 0;
}

static int test_encoder(void)
{
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder = {0};
    struct foc_board_adc adc = {0};
    const uint32_t base = UINT32_MAX - 499U;
    struct foc_adc_test_clock clock = {.epoch = base};
    CHECK(foc_board_init(&runtime, &output, base) == FOC_RUNTIME_OK);
    CHECK(foc_board_adc_init_timing(&adc, base, base, 0U));
    CHECK(foc_board_encoder_begin(&encoder, base));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, base + 70U, FOC_BOARD_ENCODER_OK, 0U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(
        !encoder.feedback.valid && !encoder.feedback.speed_valid &&
        !runtime.sensor_slots[atomic_load(&runtime.sensor_slot)].sample.electrical_phase_valid
    );
    for (uint32_t offset = 80U; offset < 500U; offset += FOC_BOARD_H0_US) {
        struct foc_board_adc_event event = foc_adc_test_event(&clock, offset);
        CHECK(foc_board_adc_begin(&runtime, &adc, &event, false));
        event = foc_adc_test_event(&clock, offset + 8U);
        CHECK(
            foc_board_adc_complete(&runtime, &adc, &output, ranks, 4U, &event, true) ==
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
    CHECK(sample.electrical_phase_valid && encoder.feedback.mechanical_phase_count == 1U);
    CHECK(
        fabs(
            (double)sample.theta_elec_rad - 7.0 * 6.283185307179586476925286766559005768 / 65536.0
        ) < 1e-5
    );
    CHECK(fabsf(sample.mechanical_rad - 0.00009587f) < 1e-8f);
    CHECK(fabsf(sample.mechanical_rad_s - 0.191748f) < 1e-5f);
    CHECK(encoder.feedback.quality == FOC_ENCODER_QUALITY_VALID);
    CHECK(encoder.feedback.speed_valid && encoder.feedback.raw_velocity_rad_s > 0.0f);
    CHECK(sample.mechanical_rad_s == encoder.feedback.filtered_velocity_rad_s);
    struct foc_board_adc_event event = foc_adc_test_event(&clock, 640U);
    CHECK(foc_board_adc_begin(&runtime, &adc, &event, false));
    event = foc_adc_test_event(&clock, 648U);
    CHECK(
        foc_board_adc_complete(&runtime, &adc, &output, ranks, 4U, &event, true) == FOC_RUNTIME_IDLE
    );
    CHECK(runtime.angle_prediction_us == 140U && runtime.adc.produced_us == 140U);
    CHECK(foc_board_encoder_begin(&encoder, 500U));
    CHECK(!foc_board_encoder_begin(&encoder, 501U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 570U, FOC_BOARD_ENCODER_OK, 65534U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(encoder.result == FOC_BOARD_ENCODER_OVERLAP && encoder.losses == 1U);
    CHECK(!runtime.sensor_slots[atomic_load(&runtime.sensor_slot)].sample.electrical_phase_valid);
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

struct backend_probe {
    bool accept;
    unsigned int calls;
    enum foc_device_output_action last_action;
};

static bool backend_record(void *argument, const struct foc_device_pwm_request *request)
{
    struct backend_probe *probe = argument;
    probe->calls++;
    probe->last_action = request->action;
    return probe->accept;
}

static int test_output_backend(void)
{
    struct foc_board_output output = {0};
    struct backend_probe probe = {.accept = true};
    const struct foc_device_pwm_config pwm = {FOC_BOARD_COMPARE_MAX, FOC_DEVICE_PHASE_ORDER_ABC};
    const struct foc_output duties = {
        .output_enable = 1U,
        .svpwm = {.duty_a = 0.6f, .duty_b = 0.5f, .duty_c = 0.4f},
    };
    struct foc_device_pwm_request request;
    CHECK(foc_device_map_output(&duties, &pwm, &request) == 0);
    CHECK(!foc_board_output_attach(NULL, backend_record, &probe));
    CHECK(!foc_board_output_attach(&output, NULL, &probe));
    CHECK(foc_board_output_attach(&output, backend_record, &probe));
    CHECK(!foc_board_output_attach(&output, backend_record, &probe));

    /* A validated UPDATE reaches the backend and is applied. */
    CHECK(foc_board_output_submit(&output, &request));
    CHECK(output.result == FOC_BOARD_OUTPUT_APPLIED && output.applied_updates == 1U);
    CHECK(probe.calls == 1U && probe.last_action == FOC_DEVICE_OUTPUT_UPDATE);
    /* A backend refusal keeps the locked disposition. */
    probe.accept = false;
    CHECK(!foc_board_output_submit(&output, &request));
    CHECK(output.result == FOC_BOARD_UPDATE_NOT_APPLIED && output.declined_updates == 1U);
    CHECK(probe.calls == 2U);
    /* An invalid request never reaches the backend. */
    request.channel1_compare++;
    CHECK(!foc_board_output_submit(&output, &request) && output.result == FOC_BOARD_OUTPUT_INVALID);
    CHECK(probe.calls == 2U);
    /* ZERO_DUTY and ACTIVE_BRAKE stay locked even with a backend. */
    CHECK(foc_device_output_zero_duty(&pwm, &request) == 0);
    CHECK(
        !foc_board_output_submit(&output, &request) && output.result == FOC_BOARD_ZERO_DUTY_LOCKED
    );
    CHECK(probe.calls == 2U);
    /* INHIBIT is always accepted and also removes drive at the backend. */
    CHECK(foc_device_output_inhibit(&pwm, &request) == 0);
    CHECK(
        foc_board_output_submit(&output, &request) && output.result == FOC_BOARD_INHIBIT_SOFTWARE
    );
    CHECK(probe.calls == 3U && probe.last_action == FOC_DEVICE_OUTPUT_INHIBIT);

    /* Initialization rewrites the journal: an earlier attachment does not survive. */
    struct foc_runtime runtime;
    CHECK(foc_board_init(&runtime, &output, 0U) == FOC_RUNTIME_OK);
    CHECK(output.backend == NULL && output.applied_updates == 0U);
    return 0;
}

/* With a backend that applies UPDATE, the same sequence that test_rejected_transaction
 * drives into a declined UPDATE runs, and the snapshot carries the decoded current. */
static int test_applied_transaction(void)
{
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder = {0};
    struct foc_runtime_foreground_result foreground;
    struct backend_probe probe = {.accept = true};
    CHECK(foc_board_init(&runtime, &output, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_board_output_attach(&output, backend_record, &probe));
    const struct foc_pipeline_command command = {{0.1f, 0.5f}, 0U, 0U};
    CHECK(foc_runtime_publish_command(&runtime, &command, 0U) == FOC_RUNTIME_OK);
    /* Same H1 schedule as test_rejected_transaction: the first frame is too early. */
    CHECK(foc_board_encoder_begin(&encoder, 0U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 20U, FOC_BOARD_ENCODER_OK, 100U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    struct foc_runtime_snapshot view;
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
        if (cycle == 5U) {
            CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
            CHECK(!view.current_valid && view.measured_iq_a == 0.0f);
        }
        /* Phase A carries +0.2 A worth of counts: a nonzero decode. */
        const struct foc_runtime_fast_input input = {
            .adc = {1241U, 2172U, 1986U, 1986U, FOC_DEVICE_SAMPLE_ALL_VALID, cycle, time},
            .now_us = time + 8U,
            .action = cycle == 4U ? FOC_RUNTIME_ACTION_ARM
                : cycle == 5U     ? FOC_RUNTIME_ACTION_START
                                  : FOC_RUNTIME_ACTION_NONE,
        };
        const int status = foc_runtime_h0_step(&runtime, &input);
        if (cycle == 6U) {
            CHECK(status == FOC_RUNTIME_OK && runtime.pipeline_status == FOC_PIPELINE_OK);
        }
        (void)foc_runtime_foreground_poll(&runtime, time + 9U, 0U, 0U, &foreground);
    }
    CHECK(output.applied_updates == 1U && output.declined_updates == 0U);
    /* Disabled cycles submit INHIBIT first; the last request is the applied UPDATE. */
    CHECK(
        output.result == FOC_BOARD_OUTPUT_APPLIED && probe.last_action == FOC_DEVICE_OUTPUT_UPDATE
    );
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
    CHECK(view.current_valid && view.output_confirmed);
    CHECK(view.measured_id_a == runtime.pipeline.measured_dq_a.d);
    CHECK(view.measured_iq_a == runtime.pipeline.measured_dq_a.q);
    CHECK(fabsf(view.measured_id_a) + fabsf(view.measured_iq_a) > 0.05f);
    return 0;
}

/* One due H0 cycle with a valid zero-current frame, then supervision. */
static int tuning_cycle(struct foc_runtime *runtime, uint32_t cycle, enum foc_runtime_action action)
{
    struct foc_runtime_foreground_result foreground;
    const uint32_t time = cycle * FOC_BOARD_H0_US + 80U;
    const struct foc_runtime_fast_input input = {
        .adc = {1241U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, cycle, time},
        .now_us = time + 8U,
        .action = action,
    };
    const int status = foc_runtime_h0_step(runtime, &input);
    (void)foc_runtime_foreground_poll(runtime, time + 9U, 0U, 0U, &foreground);
    return status;
}

static int test_tuning_mailbox(void)
{
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder = {0};
    struct foc_runtime_tuning tuning;
    struct foc_runtime_tuning_result result;
    CHECK(foc_board_init(&runtime, &output, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_read_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    CHECK(tuning.current_kp_v_per_a.q == 1.35f && tuning.command_timeout_us == 2500U);
    CHECK(foc_runtime_read_tuning_result(&runtime, &result) == FOC_RUNTIME_OK);
    CHECK(result.result == FOC_RUNTIME_REQUEST_NONE && result.revision == 0U);
    (void)tuning_cycle(&runtime, 0U, FOC_RUNTIME_ACTION_NONE);

    /* The same bounds as initialization, checked before publication. */
    struct foc_runtime_tuning candidate = tuning;
    candidate.iq_limit_a = candidate.current_limit_a + 0.1f;
    CHECK(foc_runtime_publish_tuning(&runtime, &candidate, 1U) == FOC_RUNTIME_ERR_CONFIG);
    candidate = tuning;
    candidate.command_timeout_us = FOC_RUNTIME_LEASE_MIN_US - 1U;
    CHECK(foc_runtime_publish_tuning(&runtime, &candidate, 1U) == FOC_RUNTIME_ERR_CONFIG);
    candidate = tuning;
    candidate.current_kp_v_per_a.q = -1.0f;
    CHECK(foc_runtime_publish_tuning(&runtime, &candidate, 1U) == FOC_RUNTIME_ERR_CONFIG);
    candidate = tuning;
    candidate.velocity_kp_a_per_rad_s = NAN;
    CHECK(foc_runtime_publish_tuning(&runtime, &candidate, 1U) == FOC_RUNTIME_ERR_CONFIG);

    /* DISABLED with INHIBIT confirmed: H0 applies at its next cycle. */
    candidate = tuning;
    candidate.current_kp_v_per_a.q = 2.0f;
    candidate.velocity_kp_a_per_rad_s = 0.25f;
    candidate.command_timeout_us = 20000U;
    CHECK(foc_runtime_publish_tuning(&runtime, &candidate, 1U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_publish_tuning(&runtime, &candidate, 2U) == FOC_RUNTIME_ERR_BUSY);
    CHECK(foc_runtime_read_tuning_result(&runtime, &result) == FOC_RUNTIME_OK);
    CHECK(result.result == FOC_RUNTIME_REQUEST_PENDING && result.request_sequence == 1U);
    CHECK(runtime.pipeline.current.config.kp_v_per_a.q == 1.35f);
    (void)tuning_cycle(&runtime, 1U, FOC_RUNTIME_ACTION_NONE);
    CHECK(foc_runtime_read_tuning_result(&runtime, &result) == FOC_RUNTIME_OK);
    CHECK(result.result == FOC_RUNTIME_REQUEST_APPLIED && result.revision == 1U);
    CHECK(result.request_sequence == 1U && result.status == FOC_RUNTIME_OK);
    CHECK(runtime.pipeline.current.config.kp_v_per_a.q == 2.0f);
    CHECK(runtime.outer_velocity.config.kp_a_per_rad_s == 0.25f);
    CHECK(runtime.config.motion.velocity.kp_a_per_rad_s == 0.25f);
    CHECK(runtime.pipeline.safety.config.command_timeout_us == 20000U);
    CHECK(foc_runtime_read_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    CHECK(tuning.current_kp_v_per_a.q == 2.0f && tuning.command_timeout_us == 20000U);

    /* A pending STOP cancels the candidate; nothing changes. */
    candidate = tuning;
    candidate.current_kp_v_per_a.q = 3.0f;
    CHECK(foc_runtime_publish_tuning(&runtime, &candidate, 2U) == FOC_RUNTIME_OK);
    foc_runtime_request_inhibit(&runtime, 0U);
    (void)tuning_cycle(&runtime, 2U, FOC_RUNTIME_ACTION_NONE);
    CHECK(foc_runtime_read_tuning_result(&runtime, &result) == FOC_RUNTIME_OK);
    CHECK(result.result == FOC_RUNTIME_REQUEST_CANCELLED && result.revision == 1U);
    CHECK(runtime.pipeline.current.config.kp_v_per_a.q == 2.0f);

    /* Outside DISABLED (READY after ARM) H0 rejects it. */
    CHECK(foc_board_encoder_begin(&encoder, 0U));
    (void)foc_board_encoder_complete(&runtime, &encoder, 20U, FOC_BOARD_ENCODER_OK, 100U);
    (void)tuning_cycle(&runtime, 3U, FOC_RUNTIME_ACTION_NONE);
    CHECK(foc_board_encoder_begin(&encoder, 500U));
    CHECK(
        foc_board_encoder_complete(&runtime, &encoder, 570U, FOC_BOARD_ENCODER_OK, 100U) ==
        FOC_RUNTIME_OK
    );
    const struct foc_pipeline_command command = {{0.0f, 0.1f}, 1U, 800U};
    CHECK(foc_runtime_publish_command(&runtime, &command, 800U) == FOC_RUNTIME_OK);
    (void)tuning_cycle(&runtime, 4U, FOC_RUNTIME_ACTION_ARM);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_READY);
    CHECK(foc_runtime_publish_tuning(&runtime, &candidate, 3U) == FOC_RUNTIME_OK);
    (void)tuning_cycle(&runtime, 5U, FOC_RUNTIME_ACTION_NONE);
    CHECK(foc_runtime_read_tuning_result(&runtime, &result) == FOC_RUNTIME_OK);
    CHECK(result.result == FOC_RUNTIME_REQUEST_REJECTED && result.revision == 1U);
    CHECK(result.status == FOC_RUNTIME_ERR_CONTROL && result.request_sequence == 3U);
    CHECK(runtime.pipeline.current.config.kp_v_per_a.q == 2.0f);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_time() || test_adc() || test_encoder() || test_output() ||
        test_rejected_transaction() || test_output_backend() || test_applied_transaction() ||
        test_tuning_mailbox();
}

/* ---------------------------------------- end of file ----------------------------------------- */
