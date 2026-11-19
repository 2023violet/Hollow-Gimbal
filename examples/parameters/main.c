/**
 * @file    main.c
 * @brief   Discover, configure and observe a Host Servo through the parameter API.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Role
 * ----
 * Exercise the public API with the deterministic Virtual Motor as the device.
 *
 * Implementation role: exercise the public parameter API on the Host model.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_parameter.h"
#include "foc_virtual_current.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#define EXPECT(condition)                                           \
    do {                                                            \
        if (!(condition)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            return 1;                                               \
        }                                                           \
    } while (0)

#define EXPECT_STATUS(expression, expected)               \
    do {                                                  \
        const int observed_status = (expression);         \
        if (observed_status != (expected)) {              \
            fprintf(                                      \
                stderr,                                   \
                "line %d: %s returned %d, expected %d\n", \
                __LINE__,                                 \
                #expression,                              \
                observed_status,                          \
                (expected)                                \
            );                                            \
            return 1;                                     \
        }                                                 \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static const uint32_t frame_id = 71U;
static const char *const type_names[] = {
    [FOC_PARAMETER_NONE] = "none",
    [FOC_PARAMETER_U32] = "u32",
    [FOC_PARAMETER_I32] = "i32",
    [FOC_PARAMETER_F32] = "f32",
    [FOC_PARAMETER_ENUM] = "enum",
};

static void print_scalar(enum foc_parameter_type type, union foc_parameter_scalar value)
{
    switch (type) {
    case FOC_PARAMETER_F32:
        printf("%.6g", (double)value.f32);
        break;
    case FOC_PARAMETER_I32:
        printf("%" PRId32, value.i32);
        break;
    case FOC_PARAMETER_ENUM:
    case FOC_PARAMETER_U32:
        printf("%" PRIu32, value.u32);
        break;
    case FOC_PARAMETER_NONE:
        fputs("none", stdout);
        break;
    }
}

static const char *access_name(enum foc_parameter_access access)
{
    switch (access) {
    case FOC_PARAMETER_READ_ONLY:
        return "read-only";
    case FOC_PARAMETER_INITIAL_CONFIG:
        return "initial-config";
    case FOC_PARAMETER_COMMAND_ONLY:
        return "command-only";
    case FOC_PARAMETER_ACTION_ONLY:
        return "action-only";
    }
    return "unknown";
}

static int show_catalog(struct foc_runtime *runtime)
{
    printf("PARAMETER_CATALOG count=%zu\n", foc_parameter_count());
    for (size_t index = 0U; index < foc_parameter_count(); ++index) {
        const struct foc_parameter_descriptor *entry = foc_parameter_at(index);
        EXPECT(entry != NULL && foc_parameter_find(entry->id) == entry);
        EXPECT((unsigned)entry->type < sizeof type_names / sizeof type_names[0]);
        printf(
            "PARAM id=0x%04x name=%s type=%s unit=%s access=%s",
            (unsigned)entry->id,
            entry->name,
            type_names[entry->type],
            entry->unit,
            access_name(entry->access)
        );
        if (entry->type != FOC_PARAMETER_NONE) {
            struct foc_parameter_range range;
            EXPECT_STATUS(foc_parameter_get_range(runtime, entry->id, &range), FOC_PARAMETER_OK);
            printf(" range=%c", range.minimum_exclusive ? '(' : '[');
            print_scalar(entry->type, range.minimum);
            putchar(',');
            print_scalar(entry->type, range.maximum);
            putchar(']');
        }
        putchar('\n');
    }
    return 0;
}

static int configure(struct foc_virtual_current *vm)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = frame_id,
        .torque_constant_nm_per_a =
            (float)(1.5 * profile.motor.pole_pairs * profile.motor.psi_f_wb),
        .torque_limit_nm = 0.315f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.1f,
        .max_feedforward_nm = 0.3f,
    };
    const struct foc_pipeline_config pipeline = {
        .current = profile.current,
        .adc = profile.adc,
        .pwm = profile.pwm,
        .safety = profile.safety,
    };
    const struct foc_parameter_edit cap = {
        .id = FOC_PARAMETER_MOTION_IQ_LIMIT,
        .value = {FOC_PARAMETER_F32, {.f32 = 2.2f}},
    };
    struct foc_parameter_candidate candidate;
    EXPECT_STATUS(
        foc_parameter_prepare_initial(&pipeline, &profile.runtime, &cap, 1U, &candidate),
        FOC_PARAMETER_VALIDATED
    );
    profile.runtime = candidate.config;
    EXPECT_STATUS(foc_virtual_current_init(vm, &profile), 0);
    struct foc_parameter_value effective;
    EXPECT_STATUS(foc_parameter_read(&vm->runtime, cap.id, &effective), FOC_PARAMETER_OK);
    EXPECT(effective.type == FOC_PARAMETER_F32 && effective.data.f32 == cap.value.data.f32);
    printf(
        "INITIAL_CONFIG result=VALIDATED init=OK effective_current_limit_A=%.1f\n",
        (double)effective.data.f32
    );
    return 0;
}

static int lifecycle(
    struct foc_virtual_current *vm,
    enum foc_parameter_id id,
    enum foc_runtime_action action,
    uint32_t sequence,
    uint32_t now_us,
    uint32_t h0_us,
    enum foc_safe_state state
)
{
    struct foc_runtime_snapshot snapshot;
    EXPECT_STATUS(foc_runtime_read_snapshot(&vm->runtime, &snapshot), FOC_RUNTIME_OK);
    const struct foc_runtime_lifecycle_request request = {
        .action = action,
        .requested_mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .request_sequence = sequence,
        .source_session = snapshot.motion_session,
        .produced_us = now_us,
    };
    EXPECT_STATUS(
        foc_parameter_request_action(&vm->runtime, id, &request, now_us),
        FOC_PARAMETER_ACCEPTED
    );

    struct foc_runtime_lifecycle_result result;
    EXPECT_STATUS(foc_runtime_read_lifecycle_result(&vm->runtime, &result), FOC_RUNTIME_OK);
    EXPECT(result.result == FOC_RUNTIME_REQUEST_PENDING && result.request_sequence == sequence);
    EXPECT_STATUS(foc_virtual_current_run_until(vm, h0_us), 0);
    EXPECT_STATUS(foc_runtime_read_lifecycle_result(&vm->runtime, &result), FOC_RUNTIME_OK);
    EXPECT(
        result.result == FOC_RUNTIME_REQUEST_APPLIED && result.action == action &&
        result.request_sequence == sequence && result.h0_us == h0_us && result.state == state
    );
    return 0;
}

static struct foc_runtime_command reference(uint32_t sequence, uint32_t source_us, uint32_t session)
{
    return (struct foc_runtime_command){
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = sequence,
        .produced_us = source_us,
        .target.impedance = {
            .position_rad = 0.1f,
            .stiffness_nm_per_rad = 0.65f,
            .damping_nm_s_per_rad = 0.048f,
            .mechanical_frame_id = frame_id,
            .source_session = session,
        },
    };
}

static int start_impedance(struct foc_virtual_current *vm, uint32_t *session)
{
    EXPECT_STATUS(foc_virtual_current_run_until(vm, 0U), 0);
    EXPECT(
        lifecycle(
            vm,
            FOC_PARAMETER_SET_MODE,
            FOC_RUNTIME_ACTION_SET_MODE,
            1U,
            0U,
            200U,
            FOC_SAFE_STATE_DISABLED
        ) == 0
    );
    struct foc_runtime_snapshot snapshot;
    EXPECT_STATUS(foc_runtime_read_snapshot(&vm->runtime, &snapshot), FOC_RUNTIME_OK);
    EXPECT(snapshot.mode == FOC_RUNTIME_MODE_IMPEDANCE);
    *session = snapshot.motion_session;
    EXPECT_STATUS(foc_virtual_current_run_until(vm, 201U), 0);
    struct foc_runtime_command command = reference(1U, 201U, *session);
    command.target.impedance.position_rad = 0.0f;
    struct foc_parameter_receipt priming;
    EXPECT_STATUS(
        foc_parameter_publish_impedance(&vm->runtime, &command, 201U, &priming),
        FOC_PARAMETER_ACCEPTED
    );
    EXPECT_STATUS(foc_virtual_current_run_until(vm, 500U), 0);
    EXPECT(
        lifecycle(
            vm,
            FOC_PARAMETER_ARM,
            FOC_RUNTIME_ACTION_ARM,
            2U,
            500U,
            600U,
            FOC_SAFE_STATE_READY
        ) == 0
    );
    EXPECT(
        lifecycle(
            vm,
            FOC_PARAMETER_START,
            FOC_RUNTIME_ACTION_START,
            3U,
            600U,
            800U,
            FOC_SAFE_STATE_RUNNING
        ) == 0
    );
    EXPECT_STATUS(foc_parameter_command_status(&vm->runtime, &priming), FOC_PARAMETER_ACCEPTED);
    EXPECT_STATUS(foc_virtual_current_run_until(vm, 999U), 0);
    return 0;
}

static int run_parameter_example(void)
{
    struct foc_virtual_current vm;
    EXPECT(configure(&vm) == 0);
    EXPECT(show_catalog(&vm.runtime) == 0);

    uint32_t session;
    EXPECT(start_impedance(&vm, &session) == 0);
    const struct foc_parameter_edit cap = {
        .id = FOC_PARAMETER_MOTION_IQ_LIMIT,
        .value = {FOC_PARAMETER_F32, {.f32 = 1.8f}},
    };
    struct foc_parameter_candidate candidate;
    EXPECT_STATUS(
        foc_parameter_write_access(&vm.runtime, cap.id, session),
        FOC_PARAMETER_ERR_STATE
    );
    EXPECT_STATUS(
        foc_parameter_stage(&vm.runtime, session, &cap, 1U, &candidate),
        FOC_PARAMETER_ERR_STATE
    );
    const struct foc_parameter_descriptor *cap_descriptor = foc_parameter_find(cap.id);
    EXPECT(cap_descriptor != NULL);
    printf(
        "RUNNING_STATIC_WRITE result=REJECTED detail=STATE parameter=%s\n",
        cap_descriptor->name
    );

    struct foc_runtime_command command = reference(2U, 999U, session);
    struct foc_parameter_receipt receipt;
    command.target.impedance.stiffness_nm_per_rad = NAN;
    EXPECT_STATUS(
        foc_parameter_publish_impedance(&vm.runtime, &command, 999U, &receipt),
        FOC_PARAMETER_ERR_RANGE
    );
    puts("INVALID_COMMAND result=REJECTED detail=RANGE");
    command.target.impedance.stiffness_nm_per_rad = 0.65f;
    EXPECT_STATUS(
        foc_parameter_write_access(&vm.runtime, FOC_PARAMETER_STIFFNESS, session),
        FOC_PARAMETER_OK
    );
    EXPECT_STATUS(
        foc_parameter_publish_impedance(&vm.runtime, &command, 999U, &receipt),
        FOC_PARAMETER_ACCEPTED
    );
    EXPECT_STATUS(foc_parameter_command_status(&vm.runtime, &receipt), FOC_PARAMETER_ACCEPTED);
    printf(
        "COMMAND result=ACCEPTED session=%" PRIu32 " sequence=%" PRIu32 " produced_us=%" PRIu32
        " stiffness=%.2f\n",
        receipt.session,
        receipt.sequence,
        receipt.produced_us,
        (double)command.target.impedance.stiffness_nm_per_rad
    );

    EXPECT_STATUS(foc_virtual_current_run_until(&vm, 1000U), 0);
    EXPECT_STATUS(foc_parameter_command_status(&vm.runtime, &receipt), FOC_PARAMETER_APPLIED);
    struct foc_runtime_snapshot snapshot;
    EXPECT_STATUS(foc_runtime_read_snapshot(&vm.runtime, &snapshot), FOC_RUNTIME_OK);
    EXPECT(
        snapshot.updates == 1U && snapshot.output_confirmed && snapshot.sensor_valid &&
        snapshot.mode == FOC_RUNTIME_MODE_IMPEDANCE
    );
    struct foc_parameter_value mode;
    EXPECT_STATUS(foc_parameter_read(&vm.runtime, FOC_PARAMETER_MODE, &mode), FOC_PARAMETER_OK);
    EXPECT(mode.type == FOC_PARAMETER_ENUM && mode.data.u32 == (uint32_t)snapshot.mode);
    printf(
        "COMMAND result=APPLIED mode=IMPEDANCE sensor_us=%" PRIu32 " updates=%" PRIu32
        " output=HOST_VIRTUAL physical_applied=false\n",
        snapshot.sensor_us,
        snapshot.updates
    );

    EXPECT_STATUS(
        foc_parameter_request_action(&vm.runtime, FOC_PARAMETER_STOP, NULL, 1000U),
        FOC_PARAMETER_ACCEPTED
    );
    EXPECT_STATUS(foc_virtual_current_run_until(&vm, 1200U), 0);
    EXPECT_STATUS(foc_runtime_read_snapshot(&vm.runtime, &snapshot), FOC_RUNTIME_OK);
    EXPECT(
        snapshot.output_confirmed && snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        snapshot.faults == 0U && snapshot.state == FOC_SAFE_STATE_DISABLED &&
        snapshot.motion_session != session && snapshot.updates == 1U
    );
    EXPECT_STATUS(foc_parameter_command_status(&vm.runtime, &receipt), FOC_PARAMETER_ERR_SESSION);
    EXPECT_STATUS(
        foc_parameter_publish_impedance(&vm.runtime, &command, 1200U, &receipt),
        FOC_PARAMETER_ERR_SESSION
    );
    printf(
        "STOP output=INHIBIT confirmed=%u faults=0x%" PRIx32 " session=%" PRIu32 "\n",
        (unsigned)snapshot.output_confirmed,
        snapshot.faults,
        snapshot.motion_session
    );

    EXPECT_STATUS(
        foc_parameter_stage(&vm.runtime, snapshot.motion_session, &cap, 1U, &candidate),
        FOC_PARAMETER_VALIDATED
    );
    struct foc_parameter_value effective;
    EXPECT_STATUS(foc_parameter_read(&vm.runtime, cap.id, &effective), FOC_PARAMETER_OK);
    EXPECT(candidate.config.motion.iq_limit_a == cap.value.data.f32 && effective.data.f32 == 2.2f);
    printf(
        "STOPPED_STATIC_WRITE result=VALIDATED candidate_A=%.1f effective_A=%.1f "
        "reinit_required=true\n",
        (double)candidate.config.motion.iq_limit_a,
        (double)effective.data.f32
    );
    return 0;
}

static int reject_adapter_or_sensor(bool reject_adapter)
{
    struct foc_virtual_current vm;
    EXPECT(configure(&vm) == 0);
    uint32_t session;
    EXPECT(start_impedance(&vm, &session) == 0);
    const struct foc_runtime_command command = reference(2U, 999U, session);
    struct foc_parameter_receipt receipt;
    EXPECT_STATUS(
        foc_parameter_publish_impedance(&vm.runtime, &command, 999U, &receipt),
        FOC_PARAMETER_ACCEPTED
    );
    if (reject_adapter) {
        EXPECT_STATUS(foc_virtual_current_reject_next_update(&vm), 0);
        EXPECT_STATUS(foc_virtual_current_run_until(&vm, 1000U), 0);
        struct foc_virtual_current_observation observation;
        EXPECT_STATUS(foc_virtual_current_observe(&vm, &observation), 0);
        EXPECT(observation.rejected_updates == 1U);
    } else {
        EXPECT_STATUS(foc_virtual_current_mark_sensor_fault(&vm, 1000U), 0);
        for (uint32_t event_us = 1000U; event_us <= 2500U; event_us += 500U) {
            EXPECT_STATUS(foc_virtual_current_fail_next_sensor(&vm), 0);
            EXPECT_STATUS(foc_virtual_current_run_until(&vm, event_us), 0);
        }
    }

    struct foc_runtime_snapshot snapshot;
    EXPECT_STATUS(foc_runtime_read_snapshot(&vm.runtime, &snapshot), FOC_RUNTIME_OK);
    EXPECT(
        snapshot.updates == 0U && snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        snapshot.output_confirmed && snapshot.motion_session != session
    );
    EXPECT(foc_parameter_command_status(&vm.runtime, &receipt) < 0);
    if (!reject_adapter) {
        const struct foc_parameter_edit cap = {
            .id = FOC_PARAMETER_MOTION_IQ_LIMIT,
            .value = {FOC_PARAMETER_F32, {.f32 = 1.8f}},
        };
        struct foc_parameter_candidate candidate;
        EXPECT(
            snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED && !snapshot.sensor_valid &&
            (snapshot.faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U
        );
        EXPECT_STATUS(
            foc_parameter_stage(&vm.runtime, snapshot.motion_session, &cap, 1U, &candidate),
            FOC_PARAMETER_ERR_FAULT
        );
    }
    printf(
        "%s result=REJECTED output=INHIBIT updates=%" PRIu32 " sensor_valid=%u faults=0x%" PRIx32
        " physical_applied=false\n",
        reject_adapter ? "ADAPTER_REJECTION" : "INVALID_SENSOR",
        snapshot.updates,
        (unsigned)snapshot.sensor_valid,
        snapshot.faults
    );
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    EXPECT(run_parameter_example() == 0);
    EXPECT(reject_adapter_or_sensor(true) == 0);
    EXPECT(reject_adapter_or_sensor(false) == 0);
    puts("PASS falconfoc-parameter-demo");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
