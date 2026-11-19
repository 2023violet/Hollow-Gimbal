/**
 * @file test_foc_parameter.c
 * @brief Public catalog, detached configuration and Runtime transaction regression.
 * @version 0.1
 * @date 2026-11-18
 *
 * Test role
 * ---------
 * Exercise schema, detached edits and real Runtime H0/adapter transactions.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_parameter.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static const struct foc_pipeline_config pipeline = {
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

static const struct foc_parameter_edit edits[] = {
    {FOC_PARAMETER_MOTION_IQ_LIMIT, {FOC_PARAMETER_F32, {.f32 = 1.5f}}},
    {FOC_PARAMETER_MAX_STIFFNESS, {FOC_PARAMETER_F32, {.f32 = 1.0f}}},
};

/* Private Helpers -------------------------------------------------------------- */

static int catalog(void)
{
    struct foc_parameter_descriptor descriptors[46];
    CHECK(foc_parameter_count() == sizeof descriptors / sizeof descriptors[0]);
    for (size_t index = 0; index < foc_parameter_count(); ++index) {
        const struct foc_parameter_descriptor *descriptor = foc_parameter_at(index);
        CHECK(descriptor != NULL && descriptor->name != NULL && descriptor->unit != NULL);
        CHECK(descriptor->persistable == (descriptor->access == FOC_PARAMETER_INITIAL_CONFIG));
        CHECK(foc_parameter_find(descriptor->id) == descriptor);
        descriptors[index] = *descriptor;
    }
    CHECK(foc_parameter_at(foc_parameter_count()) == NULL);
    CHECK(foc_parameter_find((enum foc_parameter_id)1) == NULL);
    CHECK(foc_parameter_check_descriptors(descriptors, foc_parameter_count()) == FOC_PARAMETER_OK);
    struct foc_parameter_descriptor duplicate[] = {descriptors[0], descriptors[1]};
    duplicate[1].id = duplicate[0].id;
    CHECK(foc_parameter_check_descriptors(duplicate, 2U) == FOC_PARAMETER_ERR_DUPLICATE);
    duplicate[1] = descriptors[1];
    duplicate[1].name = duplicate[0].name;
    CHECK(foc_parameter_check_descriptors(duplicate, 2U) == FOC_PARAMETER_ERR_DUPLICATE);
    CHECK(foc_parameter_check_descriptors(NULL, 2U) == FOC_PARAMETER_ERR_INPUT);
    return 0;
}

static int static_candidate(void)
{
    struct foc_parameter_candidate candidate = {.config = config};
    struct foc_parameter_edit invalid[] = {edits[0], edits[1]};
    invalid[1].value.type = FOC_PARAMETER_U32;
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, 2U, &candidate) ==
        FOC_PARAMETER_ERR_TYPE
    );
    CHECK(candidate.config.motion.iq_limit_a == config.motion.iq_limit_a);
    invalid[1] = edits[1];
    invalid[1].value.data.f32 = NAN;
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, 2U, &candidate) ==
        FOC_PARAMETER_ERR_RANGE
    );
    CHECK(candidate.config.motion.iq_limit_a == config.motion.iq_limit_a);
    invalid[1].value.data.f32 = INFINITY;
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, 2U, &candidate) ==
        FOC_PARAMETER_ERR_RANGE
    );
    invalid[1] = edits[1];
    invalid[0].value.data.f32 = pipeline.current.current_limit_a + 0.25f;
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, 2U, &candidate) ==
        FOC_PARAMETER_ERR_RANGE
    );
    invalid[0] = edits[0];
    struct foc_runtime_config inconsistent = config;
    inconsistent.motion.impedance.torque_constant_nm_per_a = FLT_MAX;
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &inconsistent, edits, 2U, &candidate) ==
        FOC_PARAMETER_ERR_CONFIG
    );
    CHECK(candidate.config.motion.iq_limit_a == config.motion.iq_limit_a);
    invalid[1] = edits[0];
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, 2U, &candidate) ==
        FOC_PARAMETER_ERR_DUPLICATE
    );
    invalid[1] =
        (struct foc_parameter_edit){FOC_PARAMETER_SESSION, {FOC_PARAMETER_U32, {.u32 = 0U}}};
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, 2U, &candidate) ==
        FOC_PARAMETER_ERR_READ_ONLY
    );
    CHECK(candidate.config.motion.iq_limit_a == config.motion.iq_limit_a);

    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, edits, 2U, &candidate) ==
        FOC_PARAMETER_VALIDATED
    );
    struct foc_runtime first;
    struct foc_runtime second;
    CHECK(foc_runtime_init(&first, &pipeline, &config, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_init(&second, &pipeline, &candidate.config, 0U) == FOC_RUNTIME_OK);
    struct foc_parameter_value value;
    CHECK(foc_parameter_read(&first, FOC_PARAMETER_MOTION_IQ_LIMIT, &value) == FOC_PARAMETER_OK);
    CHECK(value.type == FOC_PARAMETER_F32 && value.data.f32 == 1.25f);
    CHECK(foc_parameter_read(&second, FOC_PARAMETER_MOTION_IQ_LIMIT, &value) == FOC_PARAMETER_OK);
    CHECK(value.type == FOC_PARAMETER_F32 && value.data.f32 == 1.5f);
    const struct foc_parameter_edit lower = {
        FOC_PARAMETER_MOTION_IQ_LIMIT,
        {FOC_PARAMETER_F32, {.f32 = 1.0f}},
    };
    CHECK(foc_parameter_stage(&first, 0U, edits, 2U, &candidate) == FOC_PARAMETER_ERR_RANGE);
    CHECK(foc_parameter_stage(&first, 1U, &lower, 1U, &candidate) == FOC_PARAMETER_ERR_SESSION);
    CHECK(foc_parameter_stage(&first, 0U, &lower, 1U, &candidate) == FOC_PARAMETER_VALIDATED);
    CHECK(foc_parameter_read(&first, FOC_PARAMETER_MOTION_IQ_LIMIT, &value) == FOC_PARAMETER_OK);
    CHECK(value.data.f32 == 1.25f && candidate.config.motion.iq_limit_a == 1.0f);
    CHECK(foc_parameter_read(&first, (enum foc_parameter_id)1, &value) == FOC_PARAMETER_ERR_ID);
    CHECK(
        foc_parameter_read(&first, FOC_PARAMETER_MECHANICAL_POSITION, &value) ==
        FOC_PARAMETER_ERR_SENSOR
    );
    CHECK(
        foc_parameter_read(&first, FOC_PARAMETER_COMMAND_SEQUENCE, &value) ==
        FOC_PARAMETER_ERR_UNAVAILABLE
    );
    CHECK(foc_parameter_read(NULL, FOC_PARAMETER_MODE, &value) == FOC_PARAMETER_ERR_INPUT);
    return 0;
}

static int protection_candidate(void)
{
    enum {
        EDIT_LIMIT_ENABLE,
        EDIT_MINIMUM,
        EDIT_MAXIMUM,
        EDIT_TOLERANCE,
        EDIT_FRAME,
        EDIT_FOLLOWING_ENABLE,
        EDIT_THRESHOLD,
        EDIT_DURATION,
        EDIT_MAX_GAP,
    };
    const struct foc_parameter_edit valid[] = {
        [EDIT_LIMIT_ENABLE] = {FOC_PARAMETER_LIMITS_ENABLED, {FOC_PARAMETER_U32, {.u32 = 1U}}},
        [EDIT_MINIMUM] = {FOC_PARAMETER_LIMIT_POSITION_MIN, {FOC_PARAMETER_F32, {.f32 = -1.0f}}},
        [EDIT_MAXIMUM] = {FOC_PARAMETER_LIMIT_POSITION_MAX, {FOC_PARAMETER_F32, {.f32 = 1.0f}}},
        [EDIT_TOLERANCE] = {FOC_PARAMETER_LIMIT_TOLERANCE, {FOC_PARAMETER_F32, {.f32 = 0.125f}}},
        [EDIT_FRAME] = {FOC_PARAMETER_LIMIT_MECHANICAL_FRAME, {FOC_PARAMETER_U32, {.u32 = 7U}}},
        [EDIT_FOLLOWING_ENABLE] =
            {FOC_PARAMETER_FOLLOWING_ENABLED, {FOC_PARAMETER_U32, {.u32 = 1U}}},
        [EDIT_THRESHOLD] = {FOC_PARAMETER_FOLLOWING_THRESHOLD, {FOC_PARAMETER_F32, {.f32 = 0.25f}}},
        [EDIT_DURATION] = {FOC_PARAMETER_FOLLOWING_DURATION_US, {FOC_PARAMETER_U32, {.u32 = 300U}}},
        [EDIT_MAX_GAP] = {FOC_PARAMETER_FOLLOWING_MAX_GAP_US, {FOC_PARAMETER_U32, {.u32 = 200U}}},
    };
    const size_t count = sizeof valid / sizeof valid[0];
    struct foc_parameter_candidate candidate = {.config = config};
    unsigned char original[sizeof candidate];
    memcpy(original, &candidate, sizeof candidate);

    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, valid, 1U, &candidate) ==
        FOC_PARAMETER_ERR_CONFIG
    );
    CHECK(memcmp(original, &candidate, sizeof candidate) == 0);

    struct foc_parameter_edit invalid[sizeof valid / sizeof valid[0]];
    memcpy(invalid, valid, sizeof invalid);
    invalid[EDIT_MINIMUM].value.data.f32 = 1.0f;
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, count, &candidate) ==
        FOC_PARAMETER_ERR_CONFIG
    );
    CHECK(memcmp(original, &candidate, sizeof candidate) == 0);

    memcpy(invalid, valid, sizeof invalid);
    invalid[EDIT_TOLERANCE].value.data.f32 = 1.25f;
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, count, &candidate) ==
        FOC_PARAMETER_ERR_CONFIG
    );
    CHECK(memcmp(original, &candidate, sizeof candidate) == 0);

    memcpy(invalid, valid, sizeof invalid);
    invalid[EDIT_FRAME].value.data.u32 = 8U;
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, count, &candidate) ==
        FOC_PARAMETER_ERR_CONFIG
    );
    CHECK(memcmp(original, &candidate, sizeof candidate) == 0);

    memcpy(invalid, valid, sizeof invalid);
    invalid[EDIT_THRESHOLD].value.data.f32 = 0.0f;
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, count, &candidate) ==
        FOC_PARAMETER_ERR_RANGE
    );
    CHECK(memcmp(original, &candidate, sizeof candidate) == 0);

    memcpy(invalid, valid, sizeof invalid);
    invalid[EDIT_DURATION].value.data.u32 = UINT32_C(0x80000000);
    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, invalid, count, &candidate) ==
        FOC_PARAMETER_ERR_RANGE
    );
    CHECK(memcmp(original, &candidate, sizeof candidate) == 0);

    CHECK(
        foc_parameter_prepare_initial(&pipeline, &config, valid, count, &candidate) ==
        FOC_PARAMETER_VALIDATED
    );

    struct foc_runtime first;
    struct foc_runtime second;
    CHECK(foc_runtime_init(&first, &pipeline, &config, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_init(&second, &pipeline, &candidate.config, 0U) == FOC_RUNTIME_OK);

    struct foc_parameter_value value;
    CHECK(foc_parameter_read(&first, FOC_PARAMETER_LIMITS_ENABLED, &value) == FOC_PARAMETER_OK);
    CHECK(value.type == FOC_PARAMETER_U32 && value.data.u32 == 0U);
    CHECK(
        foc_parameter_read(&first, FOC_PARAMETER_LIMIT_MECHANICAL_FRAME, &value) == FOC_PARAMETER_OK
    );
    CHECK(value.data.u32 == 0U);

    for (size_t i = 0U; i < count; ++i) {
        CHECK(foc_parameter_read(&second, valid[i].id, &value) == FOC_PARAMETER_OK);
        CHECK(value.type == valid[i].value.type);
        if (value.type == FOC_PARAMETER_F32) {
            CHECK(value.data.f32 == valid[i].value.data.f32);
        } else {
            CHECK(value.data.u32 == valid[i].value.data.u32);
        }
    }

    CHECK(foc_parameter_read(&second, FOC_PARAMETER_FOLLOWING_STATE, &value) == FOC_PARAMETER_OK);
    CHECK(value.type == FOC_PARAMETER_ENUM && value.data.u32 == FOC_FOLLOWING_INACTIVE);
    CHECK(foc_parameter_read(&second, FOC_PARAMETER_MOTION_REJECTION, &value) == FOC_PARAMETER_OK);
    CHECK(value.type == FOC_PARAMETER_ENUM && value.data.u32 == FOC_MOTION_REJECT_NONE);
    CHECK(
        foc_parameter_write_access(&second, FOC_PARAMETER_MOTION_REJECTION, 0U) ==
        FOC_PARAMETER_ERR_READ_ONLY
    );

    struct foc_parameter_candidate staged = {.config = config};
    CHECK(foc_parameter_stage(&first, 0U, valid, count, &staged) == FOC_PARAMETER_VALIDATED);
    CHECK(staged.config.motion.limits.enabled && staged.config.motion.following_error.enabled);
    CHECK(foc_parameter_read(&first, FOC_PARAMETER_LIMITS_ENABLED, &value) == FOC_PARAMETER_OK);
    CHECK(value.data.u32 == 0U);
    CHECK(foc_parameter_read(&first, FOC_PARAMETER_FOLLOWING_ENABLED, &value) == FOC_PARAMETER_OK);
    CHECK(value.data.u32 == 0U);
    return 0;
}

struct adapter_fixture {
    bool reject_update;
    unsigned rejected;
};

static bool submit(void *argument, const struct foc_device_pwm_request *request)
{
    struct adapter_fixture *fixture = argument;
    if (request->action == FOC_DEVICE_OUTPUT_UPDATE && fixture->reject_update) {
        fixture->rejected++;
        return false;
    }
    return true;
}

static int cycle(struct foc_runtime *runtime, uint32_t time, bool valid, int expected)
{
    const struct foc_runtime_sensor sensor = {
        .theta_elec_rad = 0.37f,
        .captured_us = time,
        .seq = time / 100U,
        .valid = valid,
        .mechanical_frame_id = 7U,
        .electrical_phase_valid = valid,
    };
    const struct foc_runtime_fast_input data = {
        .adc = {1200U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, time / 100U, time},
        .now_us = time,
    };
    CHECK(
        foc_runtime_h1_step(runtime, &sensor, time, 0U) ==
        (valid ? FOC_RUNTIME_OK : FOC_RUNTIME_ERR_INPUT)
    );
    CHECK(foc_runtime_h0_step(runtime, &data) == expected);
    struct foc_runtime_foreground_result foreground;
    CHECK(foc_runtime_foreground_poll(runtime, time, 0U, 0U, &foreground) >= 0);
    return 0;
}

static int request(
    struct foc_runtime *runtime,
    uint32_t time,
    enum foc_parameter_id id,
    enum foc_runtime_action action,
    enum foc_runtime_request_result expected,
    int h0_status
)
{
    struct foc_runtime_snapshot view;
    CHECK(foc_runtime_read_snapshot(runtime, &view) == FOC_RUNTIME_OK);
    const struct foc_runtime_lifecycle_request request = {
        .action = action,
        .requested_mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .request_sequence = time / 100U,
        .source_session = view.motion_session,
        .produced_us = time - 1U,
    };
    CHECK(foc_parameter_request_action(runtime, id, &request, time - 1U) == FOC_PARAMETER_ACCEPTED);
    struct foc_runtime_lifecycle_result result;
    CHECK(foc_runtime_read_lifecycle_result(runtime, &result) == FOC_RUNTIME_OK);
    CHECK(
        result.result == FOC_RUNTIME_REQUEST_PENDING &&
        result.request_sequence == request.request_sequence
    );
    CHECK(cycle(runtime, time, true, h0_status) == 0);
    CHECK(foc_runtime_read_lifecycle_result(runtime, &result) == FOC_RUNTIME_OK);
    CHECK(
        result.result == expected && result.action == action &&
        result.request_sequence == request.request_sequence
    );
    return 0;
}

static struct foc_runtime_command command(uint32_t sequence, uint32_t time, uint32_t session)
{
    return (struct foc_runtime_command){
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = sequence,
        .produced_us = time,
        .target.impedance = {
            .position_rad = 0.12f,
            .velocity_rad_s = 0.03f,
            .stiffness_nm_per_rad = 0.6f,
            .damping_nm_s_per_rad = 0.04f,
            .torque_feedforward_nm = 0.01f,
            .mechanical_frame_id = 7U,
            .source_session = session,
        },
    };
}

static int protected_command_publication(void)
{
    struct foc_runtime_config settings = config;
    settings.motion.limits = (struct foc_motion_limits_config){
        .position_min_rad = -1.0f,
        .position_max_rad = 1.0f,
        .mechanical_frame_id = 7U,
        .enabled = true,
    };
    struct foc_runtime runtime;
    CHECK(foc_runtime_init(&runtime, &pipeline, &settings, 0U) == FOC_RUNTIME_OK);

    const struct foc_runtime_fast_input change = {
        .now_us = 0U,
        .action = FOC_RUNTIME_ACTION_SET_MODE,
        .requested_mode = FOC_RUNTIME_MODE_IMPEDANCE,
    };
    CHECK(foc_runtime_h0_step(&runtime, &change) == FOC_RUNTIME_IDLE);
    struct foc_runtime_snapshot view;
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
    CHECK(view.mode == FOC_RUNTIME_MODE_IMPEDANCE);

    struct foc_runtime_command target = command(1U, 1U, view.motion_session);
    struct foc_parameter_receipt receipt = {.sequence = 77U};
    target.target.impedance.position_rad = 1.25f;
    CHECK(
        foc_parameter_publish_impedance(&runtime, &target, 1U, &receipt) ==
        FOC_PARAMETER_ERR_MOTION_LIMIT
    );
    CHECK(receipt.owner == NULL && receipt.sequence == 77U);

    target.target.impedance.position_rad = -1.25f;
    CHECK(
        foc_parameter_publish_impedance(&runtime, &target, 1U, &receipt) ==
        FOC_PARAMETER_ERR_MOTION_LIMIT
    );
    CHECK(receipt.owner == NULL && receipt.sequence == 77U);

    target.target.impedance.position_rad = 0.5f;
    CHECK(
        foc_parameter_publish_impedance(&runtime, &target, 1U, &receipt) == FOC_PARAMETER_ACCEPTED
    );
    CHECK(receipt.owner == &runtime && receipt.sequence == 1U);
    return 0;
}

static int start(struct foc_runtime *runtime, struct adapter_fixture *fixture)
{
    struct foc_pipeline_config pipe = pipeline;
    pipe.output_submit = submit;
    pipe.output_arg = fixture;
    CHECK(foc_runtime_init(runtime, &pipe, &config, 0U) == FOC_RUNTIME_OK);
    CHECK(cycle(runtime, 0U, true, FOC_RUNTIME_IDLE) == 0);
    CHECK(
        request(
            runtime,
            100U,
            FOC_PARAMETER_START,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_REQUEST_REJECTED,
            FOC_RUNTIME_ERR_CONTROL
        ) == 0
    );
    CHECK(
        request(
            runtime,
            200U,
            FOC_PARAMETER_SET_MODE,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_REQUEST_APPLIED,
            FOC_RUNTIME_IDLE
        ) == 0
    );

    const struct foc_runtime_command priming = command(1U, 201U, 1U);
    struct foc_parameter_receipt receipt;
    CHECK(
        foc_parameter_publish_impedance(runtime, &priming, 201U, &receipt) == FOC_PARAMETER_ACCEPTED
    );
    CHECK(foc_parameter_command_status(runtime, &receipt) == FOC_PARAMETER_ACCEPTED);
    CHECK(
        request(
            runtime,
            300U,
            FOC_PARAMETER_ARM,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_REQUEST_APPLIED,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    CHECK(foc_parameter_command_status(runtime, &receipt) == FOC_PARAMETER_ACCEPTED);
    CHECK(
        request(
            runtime,
            400U,
            FOC_PARAMETER_START,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_REQUEST_APPLIED,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    CHECK(foc_parameter_command_status(runtime, &receipt) == FOC_PARAMETER_ACCEPTED);
    return 0;
}

static int controlled_command(void)
{
    struct foc_runtime runtime;
    struct adapter_fixture fixture = {0};
    CHECK(start(&runtime, &fixture) == 0);

    struct foc_runtime_command update = command(2U, 499U, 1U);
    struct foc_parameter_receipt receipt = {0};
    update.target.impedance.mechanical_frame_id = 99U;
    CHECK(
        foc_parameter_publish_impedance(&runtime, &update, 499U, &receipt) ==
        FOC_PARAMETER_ERR_FRAME
    );
    CHECK(receipt.owner == NULL);
    update.target.impedance.mechanical_frame_id = 7U;
    update.target.impedance.source_session = 0U;
    CHECK(
        foc_parameter_publish_impedance(&runtime, &update, 499U, &receipt) ==
        FOC_PARAMETER_ERR_SESSION
    );
    update.target.impedance.source_session = 1U;
    update.target.impedance.damping_nm_s_per_rad = INFINITY;
    CHECK(
        foc_parameter_publish_impedance(&runtime, &update, 499U, &receipt) ==
        FOC_PARAMETER_ERR_RANGE
    );
    update.target.impedance.damping_nm_s_per_rad = 0.04f;
    struct foc_parameter_range range;
    CHECK(foc_parameter_get_range(&runtime, FOC_PARAMETER_FEEDFORWARD, &range) == FOC_PARAMETER_OK);
    CHECK(range.minimum.f32 == -0.4f && range.maximum.f32 == 0.4f);
    CHECK(
        foc_parameter_publish_impedance(&runtime, &update, 499U, &receipt) == FOC_PARAMETER_ACCEPTED
    );
    CHECK(
        foc_parameter_publish_impedance(&runtime, &update, 499U, &receipt) ==
        FOC_PARAMETER_ERR_ORDER
    );
    CHECK(receipt.sequence == 2U && receipt.produced_us == 499U);
    CHECK(foc_parameter_command_status(&runtime, &receipt) == FOC_PARAMETER_ACCEPTED);
    CHECK(cycle(&runtime, 500U, true, FOC_RUNTIME_OK) == 0);
    CHECK(foc_parameter_command_status(&runtime, &receipt) == FOC_PARAMETER_APPLIED);

    struct foc_runtime other;
    CHECK(foc_runtime_init(&other, &pipeline, &config, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_parameter_command_status(&other, &receipt) == FOC_PARAMETER_ERR_INPUT);
    struct foc_runtime_snapshot view;
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
    CHECK(
        view.updates == 1U && view.sensor_valid && view.command_seq == 2U && view.command_us == 499U
    );
    struct foc_parameter_value value;
    CHECK(foc_parameter_read(&runtime, FOC_PARAMETER_COMMAND_SEQUENCE, &value) == FOC_PARAMETER_OK);
    CHECK(value.data.u32 == view.command_seq);
    CHECK(foc_parameter_read(&runtime, FOC_PARAMETER_COMMAND_US, &value) == FOC_PARAMETER_OK);
    CHECK(value.data.u32 == view.command_us);
    CHECK(foc_parameter_read(&runtime, FOC_PARAMETER_SENSOR_US, &value) == FOC_PARAMETER_OK);
    CHECK(value.data.u32 == 500U && value.data.u32 == view.sensor_us);
    CHECK(foc_parameter_read(&runtime, FOC_PARAMETER_OUTPUT_CONFIRMED, &value) == FOC_PARAMETER_OK);
    CHECK(value.data.u32 == 1U);
    CHECK(
        foc_parameter_read(&runtime, FOC_PARAMETER_STIFFNESS, &value) ==
        FOC_PARAMETER_ERR_UNAVAILABLE
    );
    const struct foc_parameter_edit lower = {
        FOC_PARAMETER_MOTION_IQ_LIMIT,
        {FOC_PARAMETER_F32, {.f32 = 1.0f}},
    };
    struct foc_parameter_candidate candidate = {.config = config};
    CHECK(foc_parameter_write_access(&runtime, lower.id, 1U) == FOC_PARAMETER_ERR_STATE);
    CHECK(foc_parameter_stage(&runtime, 1U, &lower, 1U, &candidate) == FOC_PARAMETER_ERR_STATE);
    CHECK(candidate.config.motion.iq_limit_a == 1.25f);

    CHECK(
        foc_parameter_request_action(&runtime, FOC_PARAMETER_STOP, NULL, 599U) ==
        FOC_PARAMETER_ACCEPTED
    );
    CHECK(foc_parameter_command_status(&runtime, &receipt) == FOC_PARAMETER_ERR_INHIBIT);
    CHECK(cycle(&runtime, 600U, true, FOC_RUNTIME_ERR_STOP) == 0);
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
    CHECK(view.action == FOC_DEVICE_OUTPUT_INHIBIT && view.output_confirmed && view.updates == 1U);
    CHECK(foc_parameter_command_status(&runtime, &receipt) == FOC_PARAMETER_ERR_SESSION);
    CHECK(
        foc_parameter_publish_impedance(&runtime, &update, 600U, &receipt) ==
        FOC_PARAMETER_ERR_SESSION
    );
    CHECK(
        foc_parameter_stage(&runtime, view.motion_session, &lower, 1U, &candidate) ==
        FOC_PARAMETER_VALIDATED
    );
    CHECK(candidate.config.motion.iq_limit_a == 1.0f);
    return 0;
}

static int rejected_update(bool sensor_valid)
{
    struct foc_runtime runtime;
    struct adapter_fixture fixture = {0};
    CHECK(start(&runtime, &fixture) == 0);
    CHECK(cycle(&runtime, 500U, true, FOC_RUNTIME_OK) == 0);

    const struct foc_runtime_command update = command(2U, 599U, 1U);
    struct foc_parameter_receipt receipt;
    CHECK(
        foc_parameter_publish_impedance(&runtime, &update, 599U, &receipt) == FOC_PARAMETER_ACCEPTED
    );
    fixture.reject_update = sensor_valid;
    CHECK(
        cycle(
            &runtime,
            600U,
            sensor_valid,
            sensor_valid ? FOC_RUNTIME_ERR_CONTROL : FOC_RUNTIME_ERR_ALIGNMENT
        ) == 0
    );
    CHECK(fixture.rejected == (sensor_valid ? 1U : 0U));
    struct foc_runtime_snapshot view;
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
    CHECK(view.action == FOC_DEVICE_OUTPUT_INHIBIT && view.output_confirmed && view.updates == 1U);
    CHECK(foc_parameter_command_status(&runtime, &receipt) == FOC_PARAMETER_ERR_SESSION);
    if (!sensor_valid) {
        struct foc_parameter_value value;
        CHECK(
            foc_parameter_read(&runtime, FOC_PARAMETER_SENSOR_US, &value) ==
            FOC_PARAMETER_ERR_SENSOR
        );
    }

    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_SAMPLE_INVALID);
    CHECK(cycle(&runtime, 700U, true, FOC_RUNTIME_ERR_STOP) == 0);
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
    CHECK(
        view.state == FOC_SAFE_STATE_FAULT_LATCHED && view.faults == FOC_SAFE_FAULT_SAMPLE_INVALID
    );
    const struct foc_parameter_edit lower = {
        FOC_PARAMETER_MOTION_IQ_LIMIT,
        {FOC_PARAMETER_F32, {.f32 = 1.0f}},
    };
    struct foc_parameter_candidate candidate = {.config = config};
    CHECK(
        foc_parameter_stage(&runtime, view.motion_session, &lower, 1U, &candidate) ==
        FOC_PARAMETER_ERR_FAULT
    );
    CHECK(candidate.config.motion.iq_limit_a == 1.25f);
    CHECK(
        foc_parameter_request_action(&runtime, FOC_PARAMETER_CLEAR_TIMEOUT, NULL, 701U) ==
        FOC_PARAMETER_ERR_STATE
    );
    CHECK(foc_parameter_command_status(&runtime, &receipt) == FOC_PARAMETER_ERR_FAULT);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    CHECK(catalog() == 0);
    CHECK(static_candidate() == 0);
    CHECK(protection_candidate() == 0);
    CHECK(protected_command_publication() == 0);
    CHECK(controlled_command() == 0);
    CHECK(rejected_update(true) == 0);
    CHECK(rejected_update(false) == 0);
    puts("Parameter catalog, detached configuration, Runtime identities and rejection paths PASS");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
