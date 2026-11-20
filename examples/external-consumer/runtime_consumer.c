/**
 * @file    runtime_consumer.c
 * @brief   Public Runtime and Sensor integration using the closed G3 Host Mock.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Synthetic observations exercise one serial H1, H0 and Foreground schedule.
 * Runtime owns Safety and output admission. Only the software Mock applies
 * updates to its compare state.
 */

#include "foc_encoder.h"
#include "foc_runtime.h"

#include <stdio.h>

#define CHECK(condition)                                          \
    do {                                                          \
        if (!(condition)) {                                       \
            fprintf(                                              \
                stderr,                                           \
                "Runtime consumer check failed at line %d: %s\n", \
                __LINE__,                                         \
                #condition                                        \
            );                                                    \
            return 1;                                             \
        }                                                         \
    } while (0)

enum {
    PERIOD_US = 100U,
    ENCODER_CPR = 4096U,
    STATIONARY_CODE = 16U
};

static const struct foc_encoder_config encoder_config = {
    .counts_per_revolution = ENCODER_CPR,
    .direction = FOC_ENCODER_DIRECTION_NORMAL,
    .max_speed_rad_s = 120.0f,
    .min_dt_us = 50U,
    .max_dt_us = 500U,
    .speed_lpf_cutoff_hz = 100.0f,
    .tracker_alpha = 0.5f,
    .tracker_beta = 0.1f,
};

static const struct foc_pipeline_config pipeline_config = {
    .current = {{1.0f, 2.4f, 0.05f, 0.95f, 0.0003f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f},
    .adc = {0.01f, 0.002f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {2500U, 500U, 350U, 350U, 3U},
};

static const struct foc_runtime_config runtime_config = {
    .h0 = {PERIOD_US, 20U, 10U},
    .h1 = {PERIOD_US, 20U, 10U},
    .supervision = {PERIOD_US, 20U, 10U},
    .telemetry = {10000U, 20U, 10U},
    .foreground_budget_us = 15U,
    .angle_timeout_us = 600U,
    .motion = {
        .enabled = true,
        .rotor = {.pole_pairs = 4U, .electrical_offset_rad = 0.1f},
        .prediction = {400U, 2000.0f, 0.0003f},
        .velocity = {0.18f, 1.4f, 0.002f},
        .position = {4.0f, 12.0f},
        .iq_limit_a = 2.5f,
    },
};

static int tick(
    struct foc_runtime *runtime,
    struct foc_encoder_context *encoder,
    uint32_t capture_us,
    int expected
)
{
    struct foc_encoder_feedback feedback;
    CHECK(
        foc_encoder_update(
            encoder,
            capture_us,
            STATIONARY_CODE,
            FOC_ENCODER_TRANSPORT_OK,
            &feedback
        ) == FOC_ENCODER_STATUS_OK
    );
    CHECK(feedback.valid && feedback.speed_valid);
    struct foc_runtime_sensor sensor = {
        .captured_us = feedback.captured_us,
        .seq = capture_us / PERIOD_US,
        .valid = true,
        .mechanical_rad = feedback.mechanical_rad,
        .mechanical_rad_s = feedback.filtered_velocity_rad_s,
        .electrical_phase_valid = true,
    };
    CHECK(
        foc_rotor_phase_from_counts(
            &runtime_config.motion.rotor,
            feedback.mechanical_phase_count,
            ENCODER_CPR,
            &sensor.theta_elec_rad
        ) == FOC_STATUS_OK
    );
    CHECK(foc_runtime_h1_step(runtime, &sensor, capture_us, 0U) == FOC_RUNTIME_OK);

    const struct foc_runtime_fast_input input = {
        .adc = {1200U, 2053U, 2038U, 2053U, FOC_DEVICE_SAMPLE_ALL_VALID, sensor.seq, capture_us},
        .now_us = capture_us + 1U,
    };
    CHECK(foc_runtime_h0_step(runtime, &input) == expected);
    struct foc_runtime_foreground_result foreground;
    CHECK(
        foc_runtime_foreground_poll(runtime, capture_us + 2U, 0U, 0U, &foreground) == FOC_RUNTIME_OK
    );
    CHECK(foreground.supervised);
    return 0;
}

static int lifecycle(
    struct foc_runtime *runtime,
    struct foc_encoder_context *encoder,
    uint32_t time_us,
    enum foc_runtime_action action,
    enum foc_runtime_mode mode
)
{
    struct foc_runtime_snapshot snapshot;
    CHECK(foc_runtime_read_snapshot(runtime, &snapshot) == FOC_RUNTIME_OK);
    const struct foc_runtime_lifecycle_request request = {
        .action = action,
        .requested_mode = mode,
        .request_sequence = time_us / PERIOD_US,
        .source_session = snapshot.motion_session,
        .produced_us = time_us,
    };
    CHECK(foc_runtime_publish_lifecycle_request(runtime, &request, time_us) == FOC_RUNTIME_OK);
    CHECK(tick(runtime, encoder, time_us, FOC_RUNTIME_IDLE) == 0);
    struct foc_runtime_lifecycle_result result;
    CHECK(foc_runtime_read_lifecycle_result(runtime, &result) == FOC_RUNTIME_OK);
    CHECK(
        result.result == FOC_RUNTIME_REQUEST_APPLIED && result.action == action &&
        result.request_sequence == request.request_sequence
    );
    return 0;
}

int main(void)
{
    struct foc_encoder_context encoder = {0};
    struct foc_runtime runtime = {0};
    struct foc_encoder_feedback first;
    struct foc_runtime_snapshot snapshot;

    CHECK(foc_encoder_init(&encoder, &encoder_config) == FOC_ENCODER_STATUS_OK);
    CHECK(
        foc_encoder_update(
            &encoder,
            PERIOD_US,
            STATIONARY_CODE,
            FOC_ENCODER_TRANSPORT_OK,
            &first
        ) == FOC_ENCODER_STATUS_OK
    );
    CHECK(!first.valid && !first.speed_valid);
    CHECK(
        foc_runtime_init(&runtime, &pipeline_config, &runtime_config, 2U * PERIOD_US) ==
        FOC_RUNTIME_OK
    );
    const struct foc_pipeline_command current = {{0.0f, 0.5f}, 1U, 2U * PERIOD_US};
    CHECK(foc_runtime_publish_command(&runtime, &current, current.produced_us) == FOC_RUNTIME_OK);
    CHECK(tick(&runtime, &encoder, 2U * PERIOD_US, FOC_RUNTIME_IDLE) == 0);
    CHECK(
        lifecycle(
            &runtime,
            &encoder,
            3U * PERIOD_US,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_CURRENT
        ) == 0
    );
    CHECK(
        lifecycle(
            &runtime,
            &encoder,
            4U * PERIOD_US,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_CURRENT
        ) == 0
    );
    CHECK(tick(&runtime, &encoder, 5U * PERIOD_US, FOC_RUNTIME_OK) == 0);
    CHECK(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    CHECK(
        snapshot.state == FOC_SAFE_STATE_RUNNING && snapshot.output_confirmed &&
        snapshot.action == FOC_DEVICE_OUTPUT_UPDATE && snapshot.updates == 1U &&
        snapshot.compare[1] > snapshot.compare[0]
    );

    foc_runtime_revoke_command(&runtime);
    CHECK(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    CHECK(snapshot.inhibit_pending && !snapshot.output_confirmed);
    CHECK(tick(&runtime, &encoder, 6U * PERIOD_US, FOC_RUNTIME_ERR_STOP) == 0);
    CHECK(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    CHECK(
        snapshot.state == FOC_SAFE_STATE_DISABLED && snapshot.output_confirmed &&
        snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.updates == 1U
    );

    CHECK(
        lifecycle(
            &runtime,
            &encoder,
            7U * PERIOD_US,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_VELOCITY
        ) == 0
    );
    struct foc_runtime_command velocity = {
        .mode = FOC_RUNTIME_MODE_VELOCITY,
        .seq = 2U,
        .produced_us = 8U * PERIOD_US,
        .target.motion_ref = 4.0f,
    };
    CHECK(
        foc_runtime_publish_motion_command(&runtime, &velocity, velocity.produced_us) ==
        FOC_RUNTIME_OK
    );
    CHECK(
        lifecycle(
            &runtime,
            &encoder,
            8U * PERIOD_US,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_VELOCITY
        ) == 0
    );
    CHECK(
        lifecycle(
            &runtime,
            &encoder,
            9U * PERIOD_US,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_VELOCITY
        ) == 0
    );
    CHECK(tick(&runtime, &encoder, 10U * PERIOD_US, FOC_RUNTIME_OK) == 0);
    CHECK(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    CHECK(
        snapshot.mode == FOC_RUNTIME_MODE_VELOCITY && snapshot.accepted_outer_seen &&
        snapshot.output_confirmed && snapshot.action == FOC_DEVICE_OUTPUT_UPDATE &&
        snapshot.updates == 2U && snapshot.compare[1] > snapshot.compare[0]
    );

    velocity.seq++;
    velocity.produced_us = 11U * PERIOD_US;
    velocity.target.motion_ref = -4.0f;
    CHECK(
        foc_runtime_publish_motion_command(&runtime, &velocity, velocity.produced_us) ==
        FOC_RUNTIME_OK
    );
    CHECK(tick(&runtime, &encoder, 11U * PERIOD_US, FOC_RUNTIME_OK) == 0);
    CHECK(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    CHECK(
        snapshot.accepted_outer_seq == 11U && snapshot.updates == 3U && snapshot.output_confirmed &&
        snapshot.compare[1] < snapshot.compare[0]
    );

    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_WDT);
    CHECK(tick(&runtime, &encoder, 12U * PERIOD_US, FOC_RUNTIME_ERR_STOP) == 0);
    CHECK(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    CHECK(
        snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED && snapshot.output_confirmed &&
        snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.updates == 3U &&
        (snapshot.faults & FOC_SAFE_FAULT_WDT) != 0U
    );

    puts("external Runtime: encoder, Current, Velocity, STOP, and latched FAULT passed");
    return 0;
}
