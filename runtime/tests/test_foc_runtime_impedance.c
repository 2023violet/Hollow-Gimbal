/**
 * @file    test_foc_runtime_impedance.c
 * @brief   Runtime impedance mailbox, identity, and output transaction tests.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Exercise the public Foreground, H1, H0 and lifecycle paths with fixed
 * source identities and a deterministic mock output.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

#include <float.h>
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

static const struct foc_pipeline_config pipeline_config = {
    .current = {{1.0f, 2.4f, 0.0f, 1.0f, 0.0003f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f},
    .adc = {0.01f, 0.002f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {1500U, 500U, 350U, 350U, 3U},
};

static const struct foc_runtime_config config = {
    .h0 = {100U, 20U, 10U},
    .h1 = {100U, 20U, 10U},
    .supervision = {100U, 20U, 10U},
    .telemetry = {10000U, 20U, 10U},
    .foreground_budget_us = 15U,
    .angle_timeout_us = 200U,
    .motion = {
        .enabled = true,
        .rotor = {4U, 0.37f},
        .prediction = {150U, 2000.0f, 0.001f},
        .velocity = {0.18f, 1.4f, 0.002f},
        .position = {4.0f, 12.0f},
        .iq_limit_a = 1.25f,
        .impedance = {
            .enabled = true,
            .mechanical_frame_id = 7U,
            .torque_constant_nm_per_a = 0.25f,
            .torque_limit_nm = 0.5f,
            .max_stiffness_nm_per_rad = 2.0f,
            .max_damping_nm_s_per_rad = 0.5f,
            .max_feedforward_nm = 0.4f,
        },
    },
};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_runtime_fast_input adc(uint32_t t)
{
    return (struct foc_runtime_fast_input){
        .adc = {1200U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, t / 100U, t},
        .now_us = t,
    };
}

static int sense(struct foc_runtime *runtime, uint32_t t, float p, float v, uint32_t frame)
{
    const struct foc_runtime_sensor sample = {
        .theta_elec_rad = 0.37f,
        .captured_us = t,
        .seq = t / 100U,
        .valid = true,
        .mechanical_rad = p,
        .mechanical_rad_s = v,
        .electrical_phase_valid = true,
        .mechanical_frame_id = frame,
    };
    return foc_runtime_h1_step(runtime, &sample, t, 0U);
}

static int cycle(struct foc_runtime *runtime, uint32_t t, int expected)
{
    struct foc_runtime_fast_input input = adc(t);
    struct foc_runtime_foreground_result result;
    CHECK(foc_runtime_h0_step(runtime, &input) == expected);
    CHECK(foc_runtime_foreground_poll(runtime, t, 0U, 0U, &result) >= 0);
    return 0;
}

static int request(
    struct foc_runtime *runtime,
    uint32_t t,
    enum foc_runtime_action action,
    enum foc_runtime_mode mode
)
{
    struct foc_runtime_snapshot snapshot;
    CHECK(foc_runtime_read_snapshot(runtime, &snapshot) == FOC_RUNTIME_OK);
    const struct foc_runtime_lifecycle_request request = {
        .action = action,
        .requested_mode = mode,
        .request_sequence = t / 100U,
        .source_session = snapshot.motion_session,
        .produced_us = t - 1U,
    };
    return foc_runtime_publish_lifecycle_request(runtime, &request, t - 1U);
}

static const struct foc_runtime_sensor_slot *proposal(const struct foc_runtime *runtime)
{
    return &runtime->sensor_slots[atomic_load(&runtime->sensor_slot)];
}

static struct foc_runtime_command
command_at(const struct foc_runtime *runtime, uint32_t seq, uint32_t t)
{
    return (struct foc_runtime_command){
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = seq,
        .produced_us = t,
        .target.impedance = {
            .position_rad = 0.75f,
            .velocity_rad_s = 0.25f,
            .stiffness_nm_per_rad = 0.5f,
            .damping_nm_s_per_rad = 0.25f,
            .torque_feedforward_nm = 0.0625f,
            .mechanical_frame_id = 7U,
            .source_session = runtime->motion_session,
        },
    };
}

static int expect_lifecycle(
    struct foc_runtime *runtime,
    uint32_t t,
    enum foc_runtime_mode mode,
    enum foc_runtime_request_result expected
)
{
    struct foc_runtime_lifecycle_result result;
    CHECK(foc_runtime_read_lifecycle_result(runtime, &result) == FOC_RUNTIME_OK);
    CHECK(
        result.result == expected && result.h0_us == t && result.request_sequence == t / 100U &&
        result.applied_session == runtime->motion_session && result.requested_mode == mode
    );
    return 0;
}

static int select_impedance(
    struct foc_runtime *runtime,
    const struct foc_runtime_config *policy,
    const struct foc_pipeline_config *pipeline
)
{
    CHECK(foc_runtime_init(runtime, pipeline, policy, 0U) == FOC_RUNTIME_OK);
    CHECK(sense(runtime, 0U, 0.0f, 0.0f, 0U) == FOC_RUNTIME_OK);
    CHECK(cycle(runtime, 0U, FOC_RUNTIME_IDLE) == 0);
    CHECK(runtime->mode == FOC_RUNTIME_MODE_CURRENT && runtime->pipeline.mock.output_confirmed);
    const struct foc_runtime_command early = command_at(runtime, 1U, 99U);
    CHECK(foc_runtime_publish_impedance_command(runtime, &early, 99U) == FOC_RUNTIME_ERR_CONTROL);
    CHECK(
        request(runtime, 100U, FOC_RUNTIME_ACTION_SET_MODE, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(sense(runtime, 100U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(cycle(runtime, 100U, FOC_RUNTIME_IDLE) == 0);
    CHECK(
        expect_lifecycle(runtime, 100U, FOC_RUNTIME_MODE_IMPEDANCE, FOC_RUNTIME_REQUEST_APPLIED) ==
        0
    );
    CHECK(runtime->motion_session == 1U && runtime->mode == FOC_RUNTIME_MODE_IMPEDANCE);
    CHECK(runtime->pipeline.mock.updates == 0U && runtime->pipeline.mock.output_confirmed);
    CHECK(runtime->pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT);
    return 0;
}

static int arm_start(struct foc_runtime *runtime, uint32_t arm_us)
{
    CHECK(
        request(runtime, arm_us, FOC_RUNTIME_ACTION_ARM, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(sense(runtime, arm_us, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(proposal(runtime)->target_valid && proposal(runtime)->target.target_a.q == 0.0f);
    CHECK(cycle(runtime, arm_us, FOC_RUNTIME_IDLE) == 0);
    CHECK(
        expect_lifecycle(
            runtime,
            arm_us,
            FOC_RUNTIME_MODE_IMPEDANCE,
            FOC_RUNTIME_REQUEST_APPLIED
        ) == 0
    );
    CHECK(
        runtime->pipeline.safety.state == FOC_SAFE_STATE_READY &&
        runtime->pipeline.mock.updates == 0U
    );
    const uint32_t start_us = arm_us + 100U;
    CHECK(
        request(runtime, start_us, FOC_RUNTIME_ACTION_START, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(sense(runtime, start_us, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(cycle(runtime, start_us, FOC_RUNTIME_IDLE) == 0);
    CHECK(
        expect_lifecycle(
            runtime,
            start_us,
            FOC_RUNTIME_MODE_IMPEDANCE,
            FOC_RUNTIME_REQUEST_APPLIED
        ) == 0
    );
    CHECK(
        runtime->pipeline.safety.state == FOC_SAFE_STATE_RUNNING &&
        runtime->pipeline.mock.updates == 0U
    );
    return 0;
}

static int test_config_and_admission(void)
{
    struct foc_runtime runtime;
    for (unsigned i = 0U; i < 8U; ++i) {
        struct foc_runtime_config bad = config;
        struct foc_runtime_impedance_config *policy = &bad.motion.impedance;
        switch (i) {
        case 0:
            policy->torque_constant_nm_per_a = 0.0f;
            break;
        case 1:
            policy->torque_constant_nm_per_a = FLT_MAX;
            break;
        case 2:
            policy->mechanical_frame_id = 0U;
            break;
        case 3:
            policy->torque_limit_nm = -1.0f;
            break;
        case 4:
            policy->max_feedforward_nm = INFINITY;
            break;
        case 5:
            policy->max_stiffness_nm_per_rad = NAN;
            break;
        case 6:
            policy->max_damping_nm_s_per_rad = -0.1f;
            break;
        case 7:
            bad.motion.enabled = false;
            break;
        }
        CHECK(foc_runtime_init(&runtime, &pipeline_config, &bad, 0U) == FOC_RUNTIME_ERR_CONFIG);
    }

    CHECK(select_impedance(&runtime, &config, &pipeline_config) == 0);
    const struct foc_runtime_command accepted = command_at(&runtime, 10U, 101U);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &accepted, 101U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_publish_motion_command(&runtime, &accepted, 101U) == FOC_RUNTIME_ERR_INPUT);
    const unsigned active = atomic_load(&runtime.command_slot);
    const struct foc_pipeline_command wrong_current = {{0.0f, 0.0f}, 11U, 102U};
    const struct foc_runtime_command wrong_position =
        {FOC_RUNTIME_MODE_POSITION, 11U, 102U, {.motion_ref = 0.0f}};
    CHECK(foc_runtime_publish_command(&runtime, &wrong_current, 102U) == FOC_RUNTIME_ERR_CONTROL);
    CHECK(
        foc_runtime_publish_motion_command(&runtime, &wrong_position, 102U) ==
        FOC_RUNTIME_ERR_CONTROL
    );
    for (unsigned i = 0U; i < 12U; ++i) {
        struct foc_runtime_command bad = command_at(&runtime, 11U, 102U);
        int expected = FOC_RUNTIME_ERR_INPUT;
        switch (i) {
        case 0:
            bad.mode = FOC_RUNTIME_MODE_POSITION;
            break;
        case 1:
            bad.target.impedance.position_rad = nextafterf(1024.0f, INFINITY);
            break;
        case 2:
            bad.target.impedance.velocity_rad_s = NAN;
            break;
        case 3:
            bad.target.impedance.stiffness_nm_per_rad = 2.01f;
            break;
        case 4:
            bad.target.impedance.stiffness_nm_per_rad = -1.0f;
            break;
        case 5:
            bad.target.impedance.damping_nm_s_per_rad = 0.51f;
            break;
        case 6:
            bad.target.impedance.torque_feedforward_nm = -0.41f;
            break;
        case 7:
            bad.target.impedance.mechanical_frame_id = 8U;
            break;
        case 8:
            --bad.target.impedance.source_session;
            expected = FOC_RUNTIME_ERR_ORDER;
            break;
        case 9:
            bad.seq = accepted.seq;
            expected = FOC_RUNTIME_ERR_ORDER;
            break;
        case 10:
            bad.produced_us = 1902U;
            expected = FOC_RUNTIME_ERR_ORDER;
            break;
        case 11:
            bad.produced_us = 1901U;
            expected = FOC_RUNTIME_ERR_ORDER;
            break;
        }
        CHECK(
            foc_runtime_publish_impedance_command(&runtime, &bad, i == 10U ? 3403U : 102U) ==
            expected
        );
        CHECK(atomic_load(&runtime.command_slot) == active && runtime.published_command.seq == 10U);
        CHECK(runtime.command_slots[active].produced_us == 101U && !runtime.consumed_command.seen);
    }
    const struct foc_runtime_command follow = command_at(&runtime, 11U, 150U);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &follow, 150U) == FOC_RUNTIME_OK);
    CHECK(arm_start(&runtime, 200U) == 0);
    CHECK(sense(&runtime, 400U, 0.5f, 0.5f, 7U) == FOC_RUNTIME_OK);
    const struct foc_runtime_sensor_slot *slot = proposal(&runtime);
    /* (.5*.25 + .25*(-.25) + .0625) / .25 = +.5 A. */
    CHECK(slot->target_valid && slot->mode == FOC_RUNTIME_MODE_IMPEDANCE && slot->session == 1U);
    CHECK(slot->sample.seq == 4U && slot->sample.captured_us == 400U);
    CHECK(slot->target.seq == 11U && slot->target.produced_us == 150U);
    CHECK(slot->target.target_a.d == 0.0f && slot->target.target_a.q == 0.5f);
    CHECK(
        runtime.pipeline.mock.updates == 0U && !runtime.accepted_outer_seen &&
        !runtime.outer_trial_seen
    );
    CHECK(cycle(&runtime, 400U, FOC_RUNTIME_OK) == 0);
    CHECK(runtime.accepted_outer_seq == 4U && runtime.pipeline.mock.updates == 1U);
    struct foc_runtime_command changed = command_at(&runtime, 12U, 401U);
    changed.target.impedance.stiffness_nm_per_rad = 0.0f;
    changed.target.impedance.damping_nm_s_per_rad = 0.5f;
    CHECK(foc_runtime_publish_impedance_command(&runtime, &changed, 401U) == FOC_RUNTIME_OK);
    CHECK(sense(&runtime, 500U, 0.5f, 0.5f, 7U) == FOC_RUNTIME_OK);
    CHECK(
        proposal(&runtime)->target_valid && proposal(&runtime)->target.target_a.q == -0.25f &&
        proposal(&runtime)->session == 1U
    );
    CHECK(cycle(&runtime, 500U, FOC_RUNTIME_OK) == 0);
    struct foc_runtime_command negative = command_at(&runtime, 13U, 501U);
    negative.target.impedance.position_rad = -1024.0f;
    CHECK(foc_runtime_publish_impedance_command(&runtime, &negative, 501U) == FOC_RUNTIME_OK);
    CHECK(sense(&runtime, 600U, 1024.0f, 0.5f, 7U) == FOC_RUNTIME_OK);
    CHECK(proposal(&runtime)->target_valid && proposal(&runtime)->target.target_a.q == -1.25f);
    CHECK(cycle(&runtime, 600U, FOC_RUNTIME_OK) == 0);
    CHECK(sense(&runtime, 700U, nextafterf(1024.0f, INFINITY), 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(!proposal(&runtime)->target_valid);
    CHECK(cycle(&runtime, 700U, FOC_RUNTIME_ERR_CONTROL) == 0);
    CHECK(
        runtime.pipeline.mock.updates == 3U &&
        runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT
    );
    return 0;
}

static int test_rearm_and_frame(void)
{
    struct foc_runtime runtime;
    CHECK(select_impedance(&runtime, &config, &pipeline_config) == 0);
    struct foc_runtime_command cmd = command_at(&runtime, 20U, 100U);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &cmd, 101U) == FOC_RUNTIME_OK);
    CHECK(
        request(&runtime, 200U, FOC_RUNTIME_ACTION_ARM, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(
        sense(&runtime, 200U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK && proposal(&runtime)->target_valid
    );
    CHECK(cycle(&runtime, 200U, FOC_RUNTIME_ERR_CONTROL) == 0);
    CHECK(
        expect_lifecycle(
            &runtime,
            200U,
            FOC_RUNTIME_MODE_IMPEDANCE,
            FOC_RUNTIME_REQUEST_REJECTED
        ) == 0
    );
    CHECK(!runtime.consumed_command.seen);
    cmd = command_at(&runtime, 21U, 201U);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &cmd, 201U) == FOC_RUNTIME_OK);
    CHECK(arm_start(&runtime, 300U) == 0);
    CHECK(
        sense(&runtime, 500U, 0.0f, 0.0f, 8U) == FOC_RUNTIME_OK && !proposal(&runtime)->target_valid
    );
    CHECK(cycle(&runtime, 500U, FOC_RUNTIME_ERR_CONTROL) == 0);
    CHECK(runtime.pipeline.mock.updates == 0U && runtime.motion_session == 2U);
    CHECK(runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT);
    CHECK(
        sense(&runtime, 600U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK && !proposal(&runtime)->target_valid
    );
    CHECK(foc_runtime_publish_impedance_command(&runtime, &cmd, 600U) == FOC_RUNTIME_ERR_ORDER);
    return 0;
}

struct preemption {
    enum foc_runtime_hook_point point;
    int status;
    bool armed;
};

static void stop_in_hook(struct foc_runtime *runtime, enum foc_runtime_hook_point point, void *arg)
{
    struct preemption *event = arg;
    if (event->armed && point == event->point) {
        event->armed = false;
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_NONE);
        const struct foc_runtime_fast_input input = adc(400U);
        event->status = foc_runtime_h0_step(runtime, &input);
    }
}

static int test_stop_preemption(enum foc_runtime_hook_point point)
{
    struct preemption crossing = {.point = point};
    struct foc_runtime_config policy = config;
    policy.hook = stop_in_hook;
    policy.hook_arg = &crossing;
    struct foc_runtime runtime;
    CHECK(select_impedance(&runtime, &policy, &pipeline_config) == 0);
    struct foc_runtime_command cmd = command_at(&runtime, 1U, 101U);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &cmd, 101U) == FOC_RUNTIME_OK);
    CHECK(arm_start(&runtime, 200U) == 0);
    cmd = command_at(&runtime, 2U, 399U);
    crossing.armed = true;
    CHECK(foc_runtime_publish_impedance_command(&runtime, &cmd, 399U) == FOC_RUNTIME_OK);
    CHECK(sense(&runtime, 400U, 0.25f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(
        !crossing.armed && crossing.status == FOC_RUNTIME_ERR_STOP && runtime.motion_session == 2U
    );
    if (point == FOC_RUNTIME_HOOK_SENSOR_WRITING) {
        CHECK(proposal(&runtime)->target_valid && proposal(&runtime)->session == 1U);
    } else {
        CHECK(!proposal(&runtime)->target_valid && proposal(&runtime)->session == 2U);
    }
    struct foc_runtime_foreground_result foreground;
    CHECK(foc_runtime_foreground_poll(&runtime, 400U, 0U, 0U, &foreground) >= 0);
    CHECK(
        runtime.pipeline.mock.updates == 0U &&
        runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT
    );
    CHECK(
        request(&runtime, 500U, FOC_RUNTIME_ACTION_ARM, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(cycle(&runtime, 500U, FOC_RUNTIME_ERR_CONTROL) == 0);
    CHECK(
        runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED &&
        runtime.consumed_command.seq == 1U
    );
    CHECK(sense(&runtime, 500U, 0.25f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &cmd, 500U) == FOC_RUNTIME_ERR_ORDER);
    cmd = command_at(&runtime, 3U, 501U);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &cmd, 501U) == FOC_RUNTIME_OK);
    CHECK(arm_start(&runtime, 600U) == 0);
    CHECK(sense(&runtime, 800U, 0.25f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 800U, FOC_RUNTIME_OK) == 0);
    CHECK(runtime.pipeline.mock.updates == 1U && runtime.pipeline.command.seq == 3U);
    return 0;
}

static int test_disabled_set_mode_observes_adc(void)
{
    struct foc_runtime_config disabled = config;
    disabled.motion.impedance.enabled = false;
    struct foc_runtime runtime;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &disabled, 0U) == FOC_RUNTIME_OK);
    CHECK(sense(&runtime, 0U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 0U, FOC_RUNTIME_IDLE) == 0);
    CHECK(
        request(&runtime, 100U, FOC_RUNTIME_ACTION_SET_MODE, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(sense(&runtime, 100U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    struct foc_runtime_fast_input input = adc(100U);
    input.adc.valid_mask = 0U;
    CHECK(foc_runtime_h0_step(&runtime, &input) == FOC_RUNTIME_ERR_CONTROL);
    CHECK(runtime.adc.produced_us == 100U && runtime.pipeline_status == FOC_PIPELINE_ERR_SAMPLE);
    CHECK(runtime.mode == FOC_RUNTIME_MODE_CURRENT && runtime.pipeline.mock.updates == 0U);
    CHECK(
        expect_lifecycle(
            &runtime,
            100U,
            FOC_RUNTIME_MODE_IMPEDANCE,
            FOC_RUNTIME_REQUEST_REJECTED
        ) == 0
    );
    return 0;
}

static int test_damping_capture_and_session(void)
{
    struct foc_runtime_config policy = config;
    struct foc_runtime runtime;
    const uint32_t boot = UINT32_MAX - 450U;
    const float invalid[] = {-1.0f, 0.5f, 201.0f, NAN, INFINITY};
    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; ++i) {
        policy.motion.impedance.damping_filter_hz = invalid[i];
        CHECK(foc_runtime_init(&runtime, &pipeline_config, &policy, boot) == FOC_RUNTIME_ERR_CONFIG);
    }
    policy.motion.impedance.damping_filter_hz = 48.0f;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &policy, boot) == FOC_RUNTIME_OK);
    static const enum foc_runtime_action actions[] = {
        FOC_RUNTIME_ACTION_NONE,
        FOC_RUNTIME_ACTION_SET_MODE,
        FOC_RUNTIME_ACTION_ARM,
        FOC_RUNTIME_ACTION_START,
        FOC_RUNTIME_ACTION_NONE,
        FOC_RUNTIME_ACTION_NONE,
        FOC_RUNTIME_ACTION_NONE,
        FOC_RUNTIME_ACTION_NONE,
        FOC_RUNTIME_ACTION_ARM,
        FOC_RUNTIME_ACTION_START,
        FOC_RUNTIME_ACTION_NONE,
        FOC_RUNTIME_ACTION_NONE,
        FOC_RUNTIME_ACTION_SET_MODE,
    };
    for (uint32_t i = 0U; i < sizeof actions / sizeof actions[0]; ++i) {
        const uint32_t now = boot + i * 100U;
        if (i == 2U || i == 5U || i == 8U) {
            const struct foc_runtime_command command = command_at(&runtime, i, now - 90U);
            CHECK(foc_runtime_publish_impedance_command(&runtime, &command, now - 90U) ==
                  FOC_RUNTIME_OK);
        }
        if (actions[i] != FOC_RUNTIME_ACTION_NONE) {
            const struct foc_runtime_lifecycle_request lifecycle = {
                .action = actions[i],
                .requested_mode = i == 12U ? FOC_RUNTIME_MODE_POSITION : FOC_RUNTIME_MODE_IMPEDANCE,
                .request_sequence = i,
                .source_session = runtime.motion_session,
                .produced_us = now - 1U,
            };
            CHECK(foc_runtime_publish_lifecycle_request(&runtime, &lifecycle, now - 1U) ==
                  FOC_RUNTIME_OK);
        }
        if (i == 11U) {
            foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_NONE);
        }
        const struct foc_runtime_sensor sample = {
            .theta_elec_rad = 0.37f,
            .captured_us = i == 5U ? now - 10U : now,
            .seq = i + (i >= 6U ? 1U : 0U),
            .valid = true,
            .electrical_phase_valid = true,
            .mechanical_rad = 0.75f,
            .mechanical_rad_s =
                i == 5U ? 1.25f : i == 6U ? 0.75f : i == 7U ? NAN : i == 10U ? -0.25f : 0.25f,
            .mechanical_frame_id = 7U,
        };
        CHECK(foc_runtime_h1_step(&runtime, &sample, now, 0U) ==
              (i == 7U ? FOC_RUNTIME_ERR_INPUT : FOC_RUNTIME_OK));
        const bool updates = (i >= 4U && i <= 6U) || i == 10U;
        if (updates) {
            /* K error is zero. At gapped capture and rearm the new D sample seeds directly. */
            const float iq = i == 4U ? 0.25f : i == 5U ? 0.22357393f : i == 6U ? -0.25f : 0.75f;
            const struct foc_runtime_sensor_slot *slot = proposal(&runtime);
            CHECK(slot->target_valid && fabsf(slot->target.target_a.q - iq) < 1.0e-6f);
            CHECK(slot->sample.mechanical_rad_s == sample.mechanical_rad_s);
            CHECK(slot->electrical.velocity_rad_s == 4.0f * sample.mechanical_rad_s);
        } else {
            CHECK(!runtime.damping_filter_seen);
        }
        const uint32_t before = runtime.pipeline.mock.updates;
        struct foc_runtime_fast_input input = adc(now);
        input.adc.sample_seq = i;
        const int status = foc_runtime_h0_step(&runtime, &input);
        CHECK(runtime.pipeline.mock.updates == before + (updates ? 1U : 0U));
        if (i == 7U || i == 11U) {
            CHECK(status < 0 && runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT);
        } else {
            CHECK(status >= 0);
        }
        struct foc_runtime_foreground_result result;
        CHECK(foc_runtime_foreground_poll(&runtime, now, 0U, 0U, &result) >= 0);
    }
    CHECK(runtime.mode == FOC_RUNTIME_MODE_POSITION && runtime.pipeline.mock.updates == 4U);
    return 0;
}

static int test_source_order_across_modes(void)
{
    struct foc_runtime runtime;
    CHECK(select_impedance(&runtime, &config, &pipeline_config) == 0);
    const struct foc_runtime_command unconsumed = command_at(&runtime, 40U, 101U);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &unconsumed, 101U) == FOC_RUNTIME_OK);
    foc_runtime_revoke_command(&runtime);
    CHECK(sense(&runtime, 200U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 200U, FOC_RUNTIME_ERR_STOP) == 0);
    CHECK(
        request(&runtime, 300U, FOC_RUNTIME_ACTION_SET_MODE, FOC_RUNTIME_MODE_CURRENT) ==
        FOC_RUNTIME_OK
    );
    CHECK(sense(&runtime, 300U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 300U, FOC_RUNTIME_IDLE) == 0);
    CHECK(
        expect_lifecycle(&runtime, 300U, FOC_RUNTIME_MODE_CURRENT, FOC_RUNTIME_REQUEST_APPLIED) == 0
    );
    CHECK(!runtime.consumed_command.seen);
    const struct foc_pipeline_command old_current = {{0.0f, 0.1f}, 39U, 301U};
    CHECK(foc_runtime_publish_command(&runtime, &old_current, 301U) == FOC_RUNTIME_ERR_ORDER);
    CHECK(runtime.published_command.seq == 40U && runtime.published_command.produced_us == 101U);
    CHECK(sense(&runtime, 400U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 400U, FOC_RUNTIME_IDLE) == 0);
    for (unsigned i = 0U; i < 2U; ++i) {
        const enum foc_runtime_mode mode =
            i == 0U ? FOC_RUNTIME_MODE_VELOCITY : FOC_RUNTIME_MODE_POSITION;
        const uint32_t t = 500U + i * 200U;
        CHECK(request(&runtime, t, FOC_RUNTIME_ACTION_SET_MODE, mode) == FOC_RUNTIME_OK);
        CHECK(sense(&runtime, t, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
        CHECK(cycle(&runtime, t, FOC_RUNTIME_IDLE) == 0);
        CHECK(expect_lifecycle(&runtime, t, mode, FOC_RUNTIME_REQUEST_APPLIED) == 0);
        const struct foc_runtime_command legacy = {mode, 41U + i, t + 1U, {.motion_ref = 0.1f}};
        CHECK(foc_runtime_publish_motion_command(&runtime, &legacy, t + 1U) == FOC_RUNTIME_OK);
        CHECK(sense(&runtime, t + 100U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
        CHECK(cycle(&runtime, t + 100U, FOC_RUNTIME_IDLE) == 0);
    }
    CHECK(
        request(&runtime, 900U, FOC_RUNTIME_ACTION_SET_MODE, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(sense(&runtime, 900U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 900U, FOC_RUNTIME_IDLE) == 0);
    const struct foc_runtime_command replay = command_at(&runtime, 42U, 901U);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &replay, 901U) == FOC_RUNTIME_ERR_ORDER);
    const struct foc_runtime_command fresh = command_at(&runtime, 43U, 901U);
    CHECK(foc_runtime_publish_impedance_command(&runtime, &fresh, 901U) == FOC_RUNTIME_OK);
    CHECK(arm_start(&runtime, 1000U) == 0);
    return 0;
}

/* A profile-derived torque constant follows the live flux; tuning stays available. */
static int test_profile_torque_constant(void)
{
    static struct foc_runtime runtime;
    struct foc_runtime_config derived = config;
    derived.motion.impedance.torque_constant_from_profile = true;
    derived.motion.impedance.torque_constant_nm_per_a = 0.0f;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &derived, 0U) == FOC_RUNTIME_OK);
    CHECK(runtime.config.motion.impedance.torque_constant_nm_per_a == 0.0f);
    CHECK(sense(&runtime, 0U, 0.0f, 0.0f, 0U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 0U, FOC_RUNTIME_IDLE) == 0);
    /* No flux yet: IMPEDANCE is not available. */
    CHECK(
        request(&runtime, 100U, FOC_RUNTIME_ACTION_SET_MODE, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(sense(&runtime, 100U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    {
        struct foc_runtime_fast_input input = adc(100U);
        struct foc_runtime_foreground_result result;
        (void)foc_runtime_h0_step(&runtime, &input);
        (void)foc_runtime_foreground_poll(&runtime, 100U, 0U, 0U, &result);
    }
    CHECK(runtime.mode == FOC_RUNTIME_MODE_CURRENT);
    /* Tuning is accepted while impedance is enabled; the flux sets Kt = 1.5 x 4 x flux. */
    struct foc_runtime_tuning tuning;
    CHECK(foc_runtime_read_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    tuning.command_timeout_us = FOC_RUNTIME_LEASE_MIN_US; /* the fixture's 1.5 ms is below it */
    tuning.profile.flux_wb = 0.01f;
    CHECK(foc_runtime_apply_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    CHECK(fabsf(runtime.config.motion.impedance.torque_constant_nm_per_a - 0.06f) < 1.0e-7f);
    CHECK(
        request(&runtime, 200U, FOC_RUNTIME_ACTION_SET_MODE, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(sense(&runtime, 200U, 0.0f, 0.0f, 7U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 200U, FOC_RUNTIME_IDLE) == 0);
    CHECK(runtime.mode == FOC_RUNTIME_MODE_IMPEDANCE);
    tuning.profile.flux_wb = 0.02f;
    CHECK(foc_runtime_apply_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    CHECK(fabsf(runtime.config.motion.impedance.torque_constant_nm_per_a - 0.12f) < 1.0e-7f);

    /* A fixed torque constant keeps the old rule: tuning is refused. */
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &config, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_read_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    tuning.command_timeout_us = FOC_RUNTIME_LEASE_MIN_US;
    CHECK(foc_runtime_apply_tuning(&runtime, &tuning) == FOC_RUNTIME_ERR_CONFIG);
    CHECK(runtime.config.motion.impedance.torque_constant_nm_per_a == 0.25f);
    return 0;
}

/* Cogging table: tuning switch, refusal without a table, and the interpolated q current
 * added to the impedance proposal at the sample's angle modulo one turn. */
static int test_cogging_table(void)
{
    static struct foc_runtime runtime;
    static struct foc_runtime_cogging_table table = {.scale_a = 0.01f};
    table.code[81] = 10;
    table.code[82] = 20;
    struct foc_runtime_config derived = config;
    derived.motion.impedance.torque_constant_from_profile = true;
    derived.motion.cogging_table = &table;
    derived.motion.cogging_frame_id = 7U;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &derived, 0U) == FOC_RUNTIME_OK);
    struct foc_runtime_tuning tuning;
    CHECK(foc_runtime_read_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    CHECK(tuning.cogging_enabled == 0U && !runtime.cogging_enabled);
    tuning.command_timeout_us = FOC_RUNTIME_LEASE_MIN_US;
    tuning.cogging_enabled = 2U;
    CHECK(foc_runtime_apply_tuning(&runtime, &tuning) == FOC_RUNTIME_ERR_CONFIG);
    tuning.cogging_enabled = 1U;
    CHECK(foc_runtime_apply_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    CHECK(runtime.cogging_enabled);
    CHECK(foc_runtime_read_tuning(&runtime, &tuning) == FOC_RUNTIME_OK && tuning.cogging_enabled == 1U);
    /* An empty table cannot be switched on. */
    table.scale_a = 0.0f;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &derived, 0U) == FOC_RUNTIME_OK);
    tuning.cogging_enabled = 1U;
    CHECK(foc_runtime_apply_tuning(&runtime, &tuning) == FOC_RUNTIME_ERR_CONFIG);
    table.scale_a = 0.01f;

    /* The same proposal with and without compensation differs by the table value. */
    float q[2];
    for (unsigned int pass = 0U; pass < 2U; ++pass) {
        struct foc_runtime_config fixed = config;
        fixed.motion.cogging_table = &table;
        fixed.motion.cogging_frame_id = 7U;
        CHECK(select_impedance(&runtime, &fixed, &pipeline_config) == 0);
        runtime.cogging_enabled = pass == 1U;
        const struct foc_runtime_command accepted = command_at(&runtime, 10U, 150U);
        CHECK(foc_runtime_publish_impedance_command(&runtime, &accepted, 150U) == FOC_RUNTIME_OK);
        CHECK(arm_start(&runtime, 200U) == 0);
        CHECK(sense(&runtime, 400U, 0.5f, 0.5f, 7U) == FOC_RUNTIME_OK);
        CHECK(proposal(&runtime)->target_valid);
        q[pass] = proposal(&runtime)->target.target_a.q;
    }
    const float position = 0.5f * 0.15915494309189535f * 1024.0f;
    const float blend = position - 81.0f;
    const float expected = 0.01f * ((1.0f - blend) * 10.0f + blend * 20.0f);
    CHECK(q[0] == 0.5f && fabsf(q[1] - q[0] - expected) < 1.0e-5f);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_config_and_admission() || test_rearm_and_frame() ||
        test_stop_preemption(FOC_RUNTIME_HOOK_COMMAND_WRITING) ||
        test_stop_preemption(FOC_RUNTIME_HOOK_SENSOR_WRITING) ||
        test_disabled_set_mode_observes_adc() || test_source_order_across_modes() ||
        test_damping_capture_and_session() || test_profile_torque_constant() ||
        test_cogging_table();
}

/* ---------------------------------------- end of file ----------------------------------------- */
