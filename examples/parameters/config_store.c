/**
 * @file    config_store.c
 * @brief   Exercise versioned configuration persistence from a Host application.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Save detached settings, recover from a torn program unit, and apply the
 * restored settings during initialization of a fresh virtual Runtime.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_config_store_host.h"
#include "foc_virtual_current.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                            \
    do {                                                            \
        if (!(condition)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            return 1;                                               \
        }                                                           \
    } while (0)

#define STATUS(stage, call, expected) CHECK(report_status((stage), (call), (expected)))

/* Private Helpers -------------------------------------------------------------- */

static bool report_status(const char *stage, int observed, int expected)
{
    printf("%s status=%d\n", stage, observed);
    if (observed != expected) {
        fprintf(stderr, "%s expected status=%d\n", stage, expected);
    }
    return observed == expected;
}

static int verify_runtime(
    struct foc_virtual_current_profile profile,
    const struct foc_parameter_candidate *restored,
    const struct foc_parameter_candidate *expected,
    uint16_t record_count
)
{
    struct foc_virtual_current vm;
    profile.runtime = restored->config;
    STATUS("RUNTIME_INIT", foc_virtual_current_init(&vm, &profile), 0);
    CHECK(foc_virtual_current_run_until(&vm, 0U) == 0);

    size_t checked = 0U;
    for (size_t index = 0U; index < foc_parameter_count(); ++index) {
        const struct foc_parameter_descriptor *descriptor = foc_parameter_at(index);
        if (!descriptor->persistable) {
            continue;
        }
        struct foc_parameter_value wanted;
        struct foc_parameter_value effective;
        CHECK(
            foc_parameter_read_config(&expected->config, descriptor->id, &wanted) ==
            FOC_PARAMETER_OK
        );
        CHECK(foc_parameter_read(&vm.runtime, descriptor->id, &effective) == FOC_PARAMETER_OK);
        CHECK(effective.type == wanted.type);
        if (wanted.type == FOC_PARAMETER_F32) {
            CHECK(effective.data.f32 == wanted.data.f32);
        } else {
            CHECK(wanted.type == FOC_PARAMETER_U32 && effective.data.u32 == wanted.data.u32);
        }
        ++checked;
    }
    CHECK(checked == record_count && checked > 0U);
    struct foc_runtime_snapshot snapshot;
    STATUS("RUNTIME_SNAPSHOT", foc_runtime_read_snapshot(&vm.runtime, &snapshot), FOC_RUNTIME_OK);
    CHECK(
        snapshot.state == FOC_SAFE_STATE_DISABLED && snapshot.output_confirmed &&
        snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.updates == 0U
    );
    printf(
        "EFFECTIVE values_checked=%zu output=INHIBIT confirmed=%u updates=%" PRIu32 "\n",
        checked,
        (unsigned)snapshot.output_confirmed,
        snapshot.updates
    );
    const struct foc_runtime_motion_config *motion = &restored->config.motion;
    printf(
        "PROTECTION limits=%u min_rad=%.2f max_rad=%.2f following=%u threshold_rad=%.2f "
        "duration_us=%" PRIu32 "\n",
        (unsigned)motion->limits.enabled,
        (double)motion->limits.position_min_rad,
        (double)motion->limits.position_max_rad,
        (unsigned)motion->following_error.enabled,
        (double)motion->following_error.threshold_rad,
        motion->following_error.duration_us
    );
    return 0;
}

static void print_report(const struct foc_config_store_report *report)
{
    const char slot = report->active_slot == FOC_CONFIG_STORE_SLOT_A ? 'A' : 'B';
    printf(
        "STORAGE slot=%c sequence=%" PRIu32 " schema=%u fields=%u bytes=%zu A=%d B=%d\n",
        slot,
        report->record.sequence,
        (unsigned)report->record.schema,
        (unsigned)report->record.count,
        report->record.size,
        report->slot_status[FOC_CONFIG_STORE_SLOT_A],
        report->slot_status[FOC_CONFIG_STORE_SLOT_B]
    );
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    const uint32_t mechanical_frame = 71U;
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = mechanical_frame,
        .torque_constant_nm_per_a =
            (float)(1.5 * profile.motor.pole_pairs * profile.motor.psi_f_wb),
        .torque_limit_nm = 0.315f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.10f,
        .max_feedforward_nm = 0.30f,
    };
    const struct foc_pipeline_config pipeline = {
        .current = profile.current,
        .adc = profile.adc,
        .pwm = profile.pwm,
        .safety = profile.safety,
    };
    const struct foc_config_store_policy policy = {
        .pipeline = &pipeline,
        .base = &profile.runtime,
        .identity = {
            .encoder_cpr = profile.encoder.counts_per_revolution,
            .encoder_direction = profile.encoder.direction,
            .mechanical_frame_id = mechanical_frame,
            /* IDs assigned to this deterministic Host setup by the application. */
            .encoder_identity = 101U,
            .motor_setup_id = 201U,
            .pole_pairs = profile.runtime.motion.rotor.pole_pairs,
        },
    };
    const struct foc_parameter_edit original_edits[] = {
        {FOC_PARAMETER_LIMITS_ENABLED, {FOC_PARAMETER_U32, {.u32 = 1U}}},
        {FOC_PARAMETER_LIMIT_POSITION_MIN, {FOC_PARAMETER_F32, {.f32 = 0.20f}}},
        {FOC_PARAMETER_LIMIT_POSITION_MAX, {FOC_PARAMETER_F32, {.f32 = 0.80f}}},
        {FOC_PARAMETER_LIMIT_TOLERANCE, {FOC_PARAMETER_F32, {.f32 = 0.005f}}},
        {FOC_PARAMETER_LIMIT_MECHANICAL_FRAME, {FOC_PARAMETER_U32, {.u32 = mechanical_frame}}},
        {FOC_PARAMETER_FOLLOWING_ENABLED, {FOC_PARAMETER_U32, {.u32 = 1U}}},
        {FOC_PARAMETER_FOLLOWING_THRESHOLD, {FOC_PARAMETER_F32, {.f32 = 0.10f}}},
        {FOC_PARAMETER_FOLLOWING_DURATION_US, {FOC_PARAMETER_U32, {.u32 = 20000U}}},
        {FOC_PARAMETER_FOLLOWING_MAX_GAP_US, {FOC_PARAMETER_U32, {.u32 = 1000U}}},
    };
    struct foc_parameter_candidate original;
    STATUS(
        "PARAMETER_PREPARE",
        foc_parameter_prepare_initial(
            &pipeline,
            &profile.runtime,
            original_edits,
            sizeof(original_edits) / sizeof(original_edits[0]),
            &original
        ),
        FOC_PARAMETER_VALIDATED
    );
    STATUS("CONFIG_VALIDATE", foc_config_store_validate(&policy, &original), FOC_CONFIG_STORE_OK);

    struct foc_config_store_host primary;
    struct foc_config_store_workspace workspace;
    struct foc_config_store_report report;
    struct foc_parameter_candidate restored;
    foc_config_store_host_init(&primary);
    const struct foc_config_store_driver driver = foc_config_store_host_driver(&primary);
    STATUS(
        "LOAD_ERASED",
        foc_config_store_load(&driver, &policy, &workspace, &restored, &report),
        FOC_CONFIG_STORE_EMPTY
    );
    STATUS(
        "SAVE_A",
        foc_config_store_save(&driver, &policy, &workspace, &original, &report),
        FOC_CONFIG_STORE_OK
    );
    foc_config_store_host_reboot(&primary);
    STATUS(
        "REBOOT_LOAD_A",
        foc_config_store_load(&driver, &policy, &workspace, &restored, &report),
        FOC_CONFIG_STORE_OK
    );
    CHECK(report.active_slot == FOC_CONFIG_STORE_SLOT_A);
    print_report(&report);
    const uint32_t original_sequence = report.record.sequence;
    CHECK(verify_runtime(profile, &restored, &original, report.record.count) == 0);

    uint8_t encoded[FOC_CONFIG_STORE_SLOT_BYTES];
    uint8_t committed[FOC_CONFIG_STORE_SLOT_BYTES];
    size_t size = 0U;
    STATUS(
        "ENCODE",
        foc_config_store_encode(
            &policy,
            &original,
            original_sequence,
            encoded,
            sizeof(encoded),
            &size
        ),
        FOC_CONFIG_STORE_OK
    );
    struct foc_config_store_record record;
    STATUS(
        "INSPECT_BEFORE_COMMIT",
        foc_config_store_inspect(encoded, size, &record),
        FOC_CONFIG_STORE_ERR_UNCOMMITTED
    );
    CHECK(driver.read(driver.context, FOC_CONFIG_STORE_SLOT_A, 0U, committed, size) == 0);
    STATUS(
        "INSPECT_COMMITTED",
        foc_config_store_inspect(committed, size, &record),
        FOC_CONFIG_STORE_OK
    );
    CHECK(
        record.schema == FOC_CONFIG_STORE_SCHEMA_VERSION && record.sequence == original_sequence &&
        record.size == size &&
        memcmp(
            encoded + FOC_CONFIG_STORE_PROGRAM_BYTES,
            committed + FOC_CONFIG_STORE_PROGRAM_BYTES,
            size - FOC_CONFIG_STORE_PROGRAM_BYTES
        ) == 0
    );
    STATUS(
        "DECODE",
        foc_config_store_decode(&policy, committed, size, &restored),
        FOC_CONFIG_STORE_OK
    );
    CHECK(verify_runtime(profile, &restored, &original, record.count) == 0);
    struct foc_config_store_policy other_encoder = policy;
    other_encoder.identity.encoder_cpr /= 2U;
    STATUS(
        "DECODE_OTHER_ENCODER",
        foc_config_store_decode(&other_encoder, committed, size, &restored),
        FOC_CONFIG_STORE_ERR_IDENTITY
    );
    committed[FOC_CONFIG_STORE_HEADER_BYTES] ^= 1U;
    STATUS(
        "DECODE_CORRUPT_PAYLOAD",
        foc_config_store_decode(&policy, committed, size, &restored),
        FOC_CONFIG_STORE_ERR_CRC
    );

    const struct foc_parameter_edit edits[] = {
        {FOC_PARAMETER_LIMIT_POSITION_MIN, {FOC_PARAMETER_F32, {.f32 = 0.30f}}},
        {FOC_PARAMETER_LIMIT_POSITION_MAX, {FOC_PARAMETER_F32, {.f32 = 0.70f}}},
        {FOC_PARAMETER_FOLLOWING_THRESHOLD, {FOC_PARAMETER_F32, {.f32 = 0.06f}}},
        {FOC_PARAMETER_FOLLOWING_DURATION_US, {FOC_PARAMETER_U32, {.u32 = 18000U}}},
    };
    struct foc_parameter_candidate updated;
    STATUS(
        "UPDATE_PREPARE",
        foc_parameter_prepare_initial(
            &pipeline,
            &original.config,
            edits,
            sizeof(edits) / sizeof(edits[0]),
            &updated
        ),
        FOC_PARAMETER_VALIDATED
    );

    /* Scan both slots, erase B, and tear the first payload unit after the header. */
    const uint32_t cut_step = 2U + FOC_CONFIG_STORE_SLOT_BYTES / FOC_CONFIG_STORE_PROGRAM_BYTES +
        FOC_CONFIG_STORE_HEADER_BYTES / FOC_CONFIG_STORE_PROGRAM_BYTES;
    foc_config_store_host_inject(&primary, cut_step, FOC_CONFIG_STORE_HOST_TORN);
    const int interrupted = foc_config_store_save(&driver, &policy, &workspace, &updated, &report);
    printf(
        "SAVE_B_TORN status=%d step=%" PRIu32 " offline=%u\n",
        interrupted,
        primary.steps,
        (unsigned)primary.offline
    );
    CHECK(interrupted == FOC_CONFIG_STORE_ERR_IO && primary.offline && primary.steps == cut_step);
    foc_config_store_host_reboot(&primary);
    STATUS(
        "REBOOT_AFTER_CUT",
        foc_config_store_load(&driver, &policy, &workspace, &restored, &report),
        FOC_CONFIG_STORE_OK
    );
    CHECK(
        report.active_slot == FOC_CONFIG_STORE_SLOT_A &&
        report.record.sequence == original_sequence &&
        report.slot_status[FOC_CONFIG_STORE_SLOT_B] == FOC_CONFIG_STORE_ERR_UNCOMMITTED
    );
    print_report(&report);
    CHECK(verify_runtime(profile, &restored, &original, report.record.count) == 0);

    struct foc_config_store_host secondary;
    foc_config_store_host_init(&secondary);
    const struct foc_config_store_driver independent = foc_config_store_host_driver(&secondary);
    STATUS(
        "SECOND_DEVICE_ERASED",
        foc_config_store_load(&independent, &policy, &workspace, &restored, &report),
        FOC_CONFIG_STORE_EMPTY
    );
    STATUS(
        "SECOND_DEVICE_SAVE_A",
        foc_config_store_save(&independent, &policy, &workspace, &original, &report),
        FOC_CONFIG_STORE_OK
    );

    STATUS(
        "SAVE_B",
        foc_config_store_save(&driver, &policy, &workspace, &updated, &report),
        FOC_CONFIG_STORE_OK
    );
    foc_config_store_host_reboot(&primary);
    STATUS(
        "REBOOT_AFTER_COMMIT",
        foc_config_store_load(&driver, &policy, &workspace, &restored, &report),
        FOC_CONFIG_STORE_OK
    );
    CHECK(
        report.active_slot == FOC_CONFIG_STORE_SLOT_B &&
        report.record.sequence == original_sequence + 1U
    );
    print_report(&report);
    CHECK(verify_runtime(profile, &restored, &updated, report.record.count) == 0);

    STATUS(
        "SECOND_DEVICE_LOAD_A",
        foc_config_store_load(&independent, &policy, &workspace, &restored, &report),
        FOC_CONFIG_STORE_OK
    );
    CHECK(
        report.active_slot == FOC_CONFIG_STORE_SLOT_A && report.record.sequence == original_sequence
    );
    print_report(&report);
    CHECK(verify_runtime(profile, &restored, &original, report.record.count) == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
