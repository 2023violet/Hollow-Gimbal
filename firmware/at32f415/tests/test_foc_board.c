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
#include "foc_board_thermal.h"
#include "foc_serial.h"

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

/* Board-like schedule: an H1 encoder frame every 500 us, then H0 at slot + 88 us. */
struct drive_bench {
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder;
    struct backend_probe probe;
    uint32_t next_h1_us;
};

static int drive_cycle(
    struct drive_bench *bench,
    uint32_t cycle,
    enum foc_runtime_action action,
    uint16_t phase_a,
    uint16_t phase_b
)
{
    struct foc_runtime_foreground_result foreground;
    const uint32_t time = cycle * FOC_BOARD_H0_US + 80U;
    while (bench->next_h1_us + 70U <= time) {
        if (foc_board_encoder_begin(&bench->encoder, bench->next_h1_us)) {
            (void)foc_board_encoder_complete(
                &bench->runtime,
                &bench->encoder,
                bench->next_h1_us + 70U,
                FOC_BOARD_ENCODER_OK,
                100U
            );
        }
        bench->next_h1_us += FOC_BOARD_H1_US;
    }
    const uint16_t phase_c = (uint16_t)(3U * 2048U - phase_a - phase_b);
    const struct foc_runtime_fast_input input = {
        .adc = {1241U, phase_a, phase_b, phase_c, FOC_DEVICE_SAMPLE_ALL_VALID, cycle, time},
        .now_us = time + 8U,
        .action = action,
        .requested_mode = FOC_RUNTIME_MODE_CALIBRATION,
    };
    const int status = foc_runtime_h0_step(&bench->runtime, &input);
    (void)foc_runtime_foreground_poll(&bench->runtime, time + 9U, 0U, 0U, &foreground);
    return status;
}

static int publish_drive(struct drive_bench *bench, uint32_t seq, uint32_t cycle)
{
    const struct foc_runtime_command drive = {
        .mode = FOC_RUNTIME_MODE_CALIBRATION,
        .seq = seq,
        .produced_us = cycle * FOC_BOARD_H0_US,
        .target.calibration = {.voltage_v = {0.5f, 0.0f}, .theta_elec_rad = 0.3f},
    };
    return foc_runtime_publish_calibration_command(&bench->runtime, &drive, drive.produced_us);
}

static int test_calibration_drive(void)
{
    static struct drive_bench bench;
    bench = (struct drive_bench){.probe = {.accept = true}};
    struct foc_runtime *runtime = &bench.runtime;
    CHECK(foc_board_init(runtime, &bench.output, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_board_output_attach(&bench.output, backend_record, &bench.probe));
    (void)drive_cycle(&bench, 0U, FOC_RUNTIME_ACTION_NONE, 2048U, 2048U);
    (void)drive_cycle(&bench, 1U, FOC_RUNTIME_ACTION_SET_MODE, 2048U, 2048U);
    CHECK(runtime->mode == FOC_RUNTIME_MODE_CALIBRATION);

    /* Without a drive command (e.g. after STOP) the cycle observes like CURRENT does:
     * no invalid-sample latch from a missing commanded angle. */
    for (uint32_t cycle = 2U; cycle < 12U; ++cycle) {
        (void)drive_cycle(&bench, cycle, FOC_RUNTIME_ACTION_NONE, 2048U, 2048U);
    }
    CHECK(runtime->pipeline.safety.latched_faults == 0U);
    CHECK(runtime->pipeline.safety.state == FOC_SAFE_STATE_DISABLED);

    /* Only finite CALIBRATION commands are published. */
    struct foc_runtime_command bad = {
        .mode = FOC_RUNTIME_MODE_CALIBRATION,
        .seq = 1U,
        .produced_us = 12U * FOC_BOARD_H0_US,
        .target.calibration = {.voltage_v = {0.5f, 0.0f}, .theta_elec_rad = NAN},
    };
    CHECK(
        foc_runtime_publish_calibration_command(runtime, &bad, bad.produced_us) ==
        FOC_RUNTIME_ERR_INPUT
    );
    bad.target.calibration.theta_elec_rad = 0.3f;
    bad.mode = FOC_RUNTIME_MODE_CURRENT;
    CHECK(
        foc_runtime_publish_calibration_command(runtime, &bad, bad.produced_us) ==
        FOC_RUNTIME_ERR_INPUT
    );

    CHECK(publish_drive(&bench, 1U, 12U) == FOC_RUNTIME_OK);
    (void)drive_cycle(&bench, 12U, FOC_RUNTIME_ACTION_ARM, 2048U, 2048U);
    CHECK(runtime->pipeline.safety.state == FOC_SAFE_STATE_READY);
    CHECK(publish_drive(&bench, 2U, 13U) == FOC_RUNTIME_OK);
    (void)drive_cycle(&bench, 13U, FOC_RUNTIME_ACTION_START, 2048U, 2048U);
    CHECK(runtime->pipeline.safety.state == FOC_SAFE_STATE_RUNNING);

    /* The commanded voltage reaches the output at the commanded angle; the PI is unused. */
    CHECK(publish_drive(&bench, 3U, 14U) == FOC_RUNTIME_OK);
    const uint32_t applied = bench.output.applied_updates;
    CHECK(drive_cycle(&bench, 14U, FOC_RUNTIME_ACTION_NONE, 2048U, 2048U) == FOC_RUNTIME_OK);
    CHECK(bench.output.applied_updates == applied + 1U);
    CHECK(bench.probe.last_action == FOC_DEVICE_OUTPUT_UPDATE);
    CHECK(runtime->pipeline.target_dq_a.d == 0.5f && runtime->pipeline.target_dq_a.q == 0.0f);
    /* The applied voltage of the accepted cycle is kept for the electrical stages. */
    CHECK(fabsf(runtime->pipeline.voltage_dq_v.d - 0.5f) < 1.0e-3f);
    CHECK(fabsf(runtime->pipeline.voltage_dq_v.q) < 1.0e-3f);
    CHECK(
        runtime->pipeline.current.integral_v.d == 0.0f &&
        runtime->pipeline.current.integral_v.q == 0.0f
    );
    const struct foc_device_pwm_request *request = &bench.output.request;
    CHECK(request->duty_a > request->duty_b && request->duty_b > request->duty_c);

    /* A phase current above the current limit stops the drive. */
    CHECK(publish_drive(&bench, 4U, 15U) == FOC_RUNTIME_OK);
    (void)drive_cycle(&bench, 15U, FOC_RUNTIME_ACTION_NONE, 2048U + 1700U, 2048U - 850U);
    CHECK(runtime->pipeline.safety.state != FOC_SAFE_STATE_RUNNING);
    CHECK(runtime->pipeline_status == FOC_PIPELINE_ERR_CORE);
    CHECK(bench.probe.last_action == FOC_DEVICE_OUTPUT_INHIBIT);
    CHECK(runtime->pipeline.voltage_dq_v.d == 0.0f && runtime->pipeline.voltage_dq_v.q == 0.0f);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

/* NTC readings over full scale per channel; NaN reports no reading. */
static bool fake_ntc(void *argument, enum foc_board_ntc channel, float *ratio)
{
    const float *ratios = argument;
    *ratio = ratios[channel];
    return !isnan(ratios[channel]);
}

/* Divider ratio of the default 10 kΩ β 3950 NTC to ground with a 10 kΩ pull-up. */
static float ntc_ratio(float celsius)
{
    const float r = 10000.0f * expf(3950.0f * (1.0f / (celsius + 273.15f) - 1.0f / 298.15f));
    return r / (r + 10000.0f);
}

static int test_thermal_poll(void)
{
    static struct foc_runtime runtime;
    static struct foc_board_output output;
    static struct foc_serial serial;
    struct foc_board_thermal thermal;
    float ratios[FOC_BOARD_NTC_COUNT] = {0.5f, 0.5f};
    const uint32_t both =
        FOC_SAFE_FAULT_OVER_TEMPERATURE_FET | FOC_SAFE_FAULT_OVER_TEMPERATURE_MOTOR;
    CHECK(foc_board_init(&runtime, &output, 0U) == FOC_RUNTIME_OK);
    foc_serial_init(&serial, &runtime);
    foc_board_thermal_init(&thermal, fake_ntc, ratios);

    /* Disabled channels read NaN and neither derate nor trip. */
    uint32_t t = 1000U;
    foc_board_thermal_poll(&thermal, &serial, t);
    CHECK(isnan(serial.temperature.fet_c) && isnan(serial.temperature.motor_c));
    CHECK(foc_board_thermal_derate(&thermal) == 0.0f && foc_board_thermal_faults(&thermal) == 0U);
    serial.thermal.fet.enabled = 1U;
    serial.thermal.motor.enabled = 1U;
    foc_board_thermal_poll(&thermal, &serial, t + FOC_BOARD_THERMAL_PERIOD_US - 1U);
    CHECK(isnan(serial.temperature.fet_c));
    t += FOC_BOARD_THERMAL_PERIOD_US;
    foc_board_thermal_poll(&thermal, &serial, t);
    CHECK(fabsf(serial.temperature.fet_c - 25.0f) < 0.05f);
    CHECK(fabsf(serial.temperature.motor_c - 25.0f) < 0.05f);

    /* The motor at 85 °C derates halfway across 75..95 °C. */
    ratios[FOC_BOARD_NTC_MOTOR] = ntc_ratio(85.0f);
    for (unsigned int poll = 0U; poll < 300U; ++poll) {
        t += FOC_BOARD_THERMAL_PERIOD_US;
        foc_board_thermal_poll(&thermal, &serial, t);
    }
    CHECK(fabsf(serial.temperature.motor_c - 85.0f) < 0.1f);
    CHECK(fabsf(foc_board_thermal_derate(&thermal) - 0.5f) < 0.01f);
    CHECK(foc_board_thermal_faults(&thermal) == 0U);

    /* A lost FET reading derates fully and trips after FOC_THERMAL_INVALID_SAMPLES polls. */
    ratios[FOC_BOARD_NTC_FET] = NAN;
    for (uint32_t poll = 1U; poll <= FOC_THERMAL_INVALID_SAMPLES; ++poll) {
        t += FOC_BOARD_THERMAL_PERIOD_US;
        foc_board_thermal_poll(&thermal, &serial, t);
        CHECK(foc_board_thermal_derate(&thermal) == 1.0f);
        CHECK(
            foc_board_thermal_faults(&thermal) ==
            (poll == FOC_THERMAL_INVALID_SAMPLES ? FOC_SAFE_FAULT_OVER_TEMPERATURE_FET : 0U)
        );
    }
    CHECK(isnan(serial.temperature.fet_c));

    /* Board1 has no reader: an enabled channel trips both ways. */
    foc_board_thermal_init(&thermal, NULL, NULL);
    for (uint32_t poll = 0U; poll < FOC_THERMAL_INVALID_SAMPLES; ++poll) {
        t += FOC_BOARD_THERMAL_PERIOD_US;
        foc_board_thermal_poll(&thermal, &serial, t);
    }
    CHECK(foc_board_thermal_faults(&thermal) == both);
    return 0;
}

int main(void)
{
    return test_time() || test_adc() || test_encoder() || test_output() ||
        test_rejected_transaction() || test_output_backend() || test_applied_transaction() ||
        test_tuning_mailbox() || test_calibration_drive() || test_thermal_poll();
}

/* ---------------------------------------- end of file ----------------------------------------- */
