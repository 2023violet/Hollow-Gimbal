/**
 * @file    test_foc_config_diagnostics.c
 * @brief   Verify read-only diagnostics against formal storage and Runtime decisions.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Exercise real Host slots, validated candidates and the Runtime snapshot.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_config_diagnostics.h"
#include "foc_config_store_host.h"

#include "crc.h"

#include <stdio.h>
#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static const struct foc_pipeline_config pipeline = {
    .current = {{1.0f, 2.4f, 0.0f, 1.0f, 0.0003f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f},
    .adc = {0.01f, 0.002f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {5000U, 500U, 350U, 350U, 1U},
};

static const struct foc_runtime_config base = {
    .h0 = {100U, 20U, 10U},
    .h1 = {200U, 20U, 10U},
    .supervision = {100U, 20U, 10U},
    .telemetry = {10000U, 20U, 10U},
    .foreground_budget_us = 15U,
    .angle_timeout_us = 150U,
    .motion = {
        .enabled = true,
        .rotor = {7U, 0.25f},
        .prediction = {150U, 2000.0f, 0.001f},
        .velocity = {0.18f, 1.4f, 0.002f},
        .position = {4.0f, 12.0f},
        .iq_limit_a = 1.25f,
        .impedance = {true, 17U, 0.25f, 0.4f, 1.0f, 0.3f, 0.1f},
        .limits = {-1.0f, 2.0f, 0.125f, 17U, false},
        .following_error = {0.125f, 400U, 200U, false},
    },
};
static const struct foc_config_store_policy policy = {
    .pipeline = &pipeline,
    .base = &base,
    .identity = {4096U, -1, 17U, 51U, 61U, 7U},
};

static struct foc_parameter_candidate updated(void)
{
    struct foc_parameter_candidate candidate = {.config = base};
    candidate.config.motion.limits.enabled = true;
    candidate.config.motion.limits.position_min_rad = -0.5f;
    candidate.config.motion.limits.position_max_rad = 1.5f;
    candidate.config.motion.following_error.enabled = true;
    candidate.config.motion.following_error.threshold_rad = 0.25f;
    return candidate;
}

static void put_word(uint8_t *p, uint32_t word)
{
    for (unsigned int i = 0U; i < 4U; i++) {
        p[i] = (uint8_t)(word >> (i * 8U));
    }
}

static void fix_crc(uint8_t *p)
{
    const size_t crc_offset = FOC_CONFIG_STORE_HEADER_BYTES - 4U;
    uint32_t crc = crc32_ieee(CRC32_IEEE_INIT, p + 4U, crc_offset - 4U);
    const size_t size = (size_t)p[16] + (size_t)p[17] * 256U;
    crc = CRC32_IEEE_FINALIZE(crc32_ieee(crc, p + FOC_CONFIG_STORE_HEADER_BYTES, size));
    put_word(p + crc_offset, crc);
}

static int inspect_unchanged(
    struct foc_config_store_host *device,
    const struct foc_config_store_policy *expected_policy,
    int expected,
    enum foc_config_store_slot selected,
    struct foc_config_diagnostic *result
)
{
    struct foc_config_store_driver reader = foc_config_store_host_driver(device);
    reader.program = NULL;
    reader.erase = NULL;
    uint8_t original[2][FOC_CONFIG_STORE_SLOT_BYTES];
    memcpy(original, device->slots, sizeof(original));
    struct foc_config_store_workspace work;
    const uint32_t steps = device->steps;
    CHECK(foc_config_inspect_pair(&reader, expected_policy, &work, result) == expected);
    CHECK(result->restore_status == expected && result->recovery.active_slot == selected);
    CHECK(result->source == FOC_DIAG_SOURCE_HOST_STORAGE_IMAGE);
    CHECK(device->steps == steps + 2U && memcmp(original, device->slots, sizeof(original)) == 0);
    if (expected_policy != NULL) {
        struct foc_config_store_report recovered;
        struct foc_parameter_candidate candidate;
        CHECK(
            foc_config_store_load(&reader, expected_policy, &work, &candidate, &recovered) ==
            expected
        );
        CHECK(result->recovery.active_slot == recovered.active_slot);
        for (unsigned int slot = 0U; slot < 2U; ++slot) {
            CHECK(recovered.slot_status[slot] == result->recovery.slot_status[slot]);
        }
        if (expected == FOC_CONFIG_STORE_OK || expected == FOC_CONFIG_STORE_MIGRATED) {
            CHECK(
                result->slots[selected].applicable &&
                recovered.record.sequence == result->recovery.record.sequence
            );
            CHECK(
                candidate.config.motion.limits.position_min_rad ==
                result->slots[selected].candidate.config.motion.limits.position_min_rad
            );
        }
    }
    return 0;
}

static int storage_inspection(void)
{
    struct foc_config_store_host device;
    foc_config_store_host_init(&device);
    const struct foc_config_store_driver driver = foc_config_store_host_driver(&device);
    const struct foc_parameter_candidate old = {.config = base};
    const struct foc_parameter_candidate next = updated();
    struct foc_config_store_workspace work;
    struct foc_config_diagnostic diag;
    CHECK(
        inspect_unchanged(
            &device,
            &policy,
            FOC_CONFIG_STORE_EMPTY,
            FOC_CONFIG_STORE_SLOT_NONE,
            &diag
        ) == 0
    );
    CHECK(
        diag.slots[1].reason == FOC_DIAG_REASON_RECORD_NOT_FOUND && !diag.slots[1].header_present
    );
    CHECK(foc_config_store_save(&driver, &policy, &work, &old, NULL) == FOC_CONFIG_STORE_OK);
    CHECK(
        inspect_unchanged(&device, &policy, FOC_CONFIG_STORE_OK, FOC_CONFIG_STORE_SLOT_A, &diag) ==
        0
    );
    CHECK(
        diag.slots[0].integrity == FOC_DIAG_PASS && diag.slots[0].applicable &&
        diag.slots[0].record.sequence == 0U
    );
    CHECK(
        inspect_unchanged(
            &device,
            NULL,
            FOC_CONFIG_STORE_ERR_IDENTITY_UNKNOWN,
            FOC_CONFIG_STORE_SLOT_A,
            &diag
        ) == 0
    );
    CHECK(
        diag.slots[0].integrity == FOC_DIAG_PASS && diag.slots[0].identity == FOC_DIAG_UNKNOWN &&
        !diag.slots[0].applicable
    );

    foc_config_store_host_inject(&device, 80U, FOC_CONFIG_STORE_HOST_TORN);
    CHECK(foc_config_store_save(&driver, &policy, &work, &next, NULL) == FOC_CONFIG_STORE_ERR_IO);
    foc_config_store_host_reboot(&device);
    CHECK(
        inspect_unchanged(&device, &policy, FOC_CONFIG_STORE_OK, FOC_CONFIG_STORE_SLOT_A, &diag) ==
        0
    );
    CHECK(
        diag.slots[1].reason == FOC_DIAG_REASON_INCOMPLETE_COMMIT &&
        diag.slots[1].selection == FOC_DIAG_SELECTION_INELIGIBLE
    );

    foc_config_store_host_inject(&device, 109U, FOC_CONFIG_STORE_HOST_AFTER);
    CHECK(
        foc_config_store_save(&driver, &policy, &work, &next, NULL) ==
        FOC_CONFIG_STORE_ERR_UNCERTAIN
    );
    foc_config_store_host_reboot(&device);
    CHECK(
        inspect_unchanged(&device, &policy, FOC_CONFIG_STORE_OK, FOC_CONFIG_STORE_SLOT_B, &diag) ==
        0
    );
    CHECK(
        diag.slots[0].selection == FOC_DIAG_SELECTION_OLDER && diag.slots[0].applicable &&
        diag.slots[1].selection == FOC_DIAG_SELECTION_SELECTED
    );
    const struct foc_config_store_host committed = device;
    memset(device.slots[0], 0xFF, FOC_CONFIG_STORE_SLOT_BYTES);
    CHECK(
        inspect_unchanged(&device, &policy, FOC_CONFIG_STORE_OK, FOC_CONFIG_STORE_SLOT_B, &diag) ==
        0
    );

    device = committed;
    device.slots[1][FOC_CONFIG_STORE_HEADER_BYTES + 4U] ^= 1U;
    CHECK(
        inspect_unchanged(&device, &policy, FOC_CONFIG_STORE_OK, FOC_CONFIG_STORE_SLOT_A, &diag) ==
        0
    );
    CHECK(diag.slots[1].crc == FOC_DIAG_FAIL && diag.slots[1].reason == FOC_DIAG_REASON_BAD_CRC);

    device = committed;
    device.slots[1][10] = 12U;
    fix_crc(device.slots[1]);
    CHECK(
        inspect_unchanged(
            &device,
            &policy,
            FOC_CONFIG_STORE_ERR_SCHEMA,
            FOC_CONFIG_STORE_SLOT_B,
            &diag
        ) == 0
    );
    CHECK(
        diag.slots[1].schema == FOC_DIAG_FAIL && diag.slots[1].integrity == FOC_DIAG_PASS &&
        !diag.slots[1].applicable && diag.slots[0].applicable
    );

    device = committed;
    device.slots[1][10] = 1U;
    device.slots[1][16] = 40U;
    device.slots[1][18] = 5U;
    fix_crc(device.slots[1]);
    CHECK(
        inspect_unchanged(
            &device,
            &policy,
            FOC_CONFIG_STORE_ERR_RECONFIRM,
            FOC_CONFIG_STORE_SLOT_B,
            &diag
        ) == 0
    );
    CHECK(
        diag.slots[1].migration_needed && diag.slots[1].reason == FOC_DIAG_REASON_MIGRATION_REQUIRED
    );
    struct foc_config_store_policy confirmed = policy;
    confirmed.confirmed_protection = &next;
    CHECK(
        inspect_unchanged(
            &device,
            &confirmed,
            FOC_CONFIG_STORE_MIGRATED,
            FOC_CONFIG_STORE_SLOT_B,
            &diag
        ) == 0
    );
    CHECK(diag.slots[1].applicable && diag.slots[1].migration_needed);

    device = committed;
    struct foc_config_store_policy mismatched = policy;
    mismatched.identity.encoder_cpr /= 2U;
    CHECK(
        inspect_unchanged(
            &device,
            &mismatched,
            FOC_CONFIG_STORE_ERR_IDENTITY,
            FOC_CONFIG_STORE_SLOT_B,
            &diag
        ) == 0
    );
    CHECK(
        diag.slots[1].integrity == FOC_DIAG_PASS && diag.slots[1].identity == FOC_DIAG_FAIL &&
        diag.slots[1].identity_mismatches == FOC_DIAG_ID_CPR && !diag.slots[1].applicable
    );

    put_word(device.slots[0] + 12U, UINT32_MAX);
    put_word(device.slots[1] + 12U, 0U);
    fix_crc(device.slots[0]);
    fix_crc(device.slots[1]);
    CHECK(
        inspect_unchanged(&device, &policy, FOC_CONFIG_STORE_OK, FOC_CONFIG_STORE_SLOT_B, &diag) ==
        0
    );
    memcpy(device.slots[0], device.slots[1], FOC_CONFIG_STORE_SLOT_BYTES);
    CHECK(
        inspect_unchanged(&device, &policy, FOC_CONFIG_STORE_OK, FOC_CONFIG_STORE_SLOT_A, &diag) ==
        0
    );
    CHECK(diag.slots[1].selection == FOC_DIAG_SELECTION_EQUIVALENT);
    put_word(device.slots[1] + 12U, 0x80000000U);
    fix_crc(device.slots[1]);
    CHECK(
        inspect_unchanged(
            &device,
            &policy,
            FOC_CONFIG_STORE_ERR_SEQUENCE,
            FOC_CONFIG_STORE_SLOT_NONE,
            &diag
        ) == 0
    );
    CHECK(diag.slots[0].selection == FOC_DIAG_SELECTION_UNDECIDED);
    puts(
        "PAIR actual restore, lost acknowledgement, CRC, Schema, identity and Serial rollover PASS"
    );
    return 0;
}

static int effective_diff(void)
{
    const struct foc_parameter_candidate next = updated();
    const struct foc_config_diff_input left = {&next.config, FOC_CONFIG_STORE_OK};
    const struct foc_config_diff_input right = {&base, FOC_CONFIG_STORE_OK};
    struct foc_config_diff_entry entries[24];
    size_t count = 55U;
    CHECK(foc_config_diff(&left, &right, entries, 0U, &count) == FOC_CONFIG_STORE_ERR_CAPACITY);
    CHECK(count == 55U);
    CHECK(foc_config_diff(&left, &right, entries, 24U, &count) == FOC_CONFIG_STORE_OK);
    CHECK(count == foc_config_diff_count() && count == 19U);
    size_t protected = 0U, changes = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if (entries[i].motion_protection) {
            ++protected;
            CHECK(
                entries[i].parameter->requires_reinit && entries[i].left_present &&
                entries[i].right_present
            );
            if (entries[i].kind == FOC_DIAG_VALUE_CHANGED) {
                ++changes;
            }
        }
    }
    CHECK(protected == 9U && changes == 5U);
    struct foc_runtime runtime;
    struct foc_runtime_diagnostic observed;
    CHECK(foc_config_application_status(&left, NULL) == FOC_DIAG_NOT_OBSERVED);
    CHECK(foc_runtime_init(&runtime, &pipeline, &base, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_diagnose(&runtime, &observed) == FOC_RUNTIME_OK);
    CHECK(foc_config_application_status(&left, &observed) == FOC_DIAG_REINITIALIZATION_REQUIRED);
    struct foc_runtime reinitialized;
    CHECK(foc_runtime_init(&reinitialized, &pipeline, &next.config, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_diagnose(&reinitialized, &observed) == FOC_RUNTIME_OK);
    CHECK(foc_config_application_status(&left, &observed) == FOC_DIAG_MATCHES_EFFECTIVE);
    CHECK(!observed.snapshot.updates && observed.effective.motion.limits.enabled);
    puts("DIFF protection fields, source availability and effective observation PASS");
    return 0;
}

static int malformed_records(void)
{
    const struct foc_parameter_candidate next = updated();
    uint8_t original[FOC_CONFIG_STORE_SLOT_BYTES];
    size_t used;
    CHECK(
        foc_config_store_encode(&policy, &next, 1U, original, sizeof(original), &used) ==
        FOC_CONFIG_STORE_OK
    );
    put_word(original, FOC_CONFIG_STORE_COMMIT_WORD);
    struct foc_config_slot_diagnostic slot;
    CHECK(
        foc_config_inspect_slot(original, used - 1U, &policy, &slot) == FOC_CONFIG_STORE_ERR_RECORD
    );
    CHECK(slot.reason == FOC_DIAG_REASON_BAD_ENVELOPE && slot.crc == FOC_DIAG_UNKNOWN);
    static const struct {
        size_t offset;
        uint32_t word;
        int status;
        enum foc_diagnostic_reason reason;
    } mutations[] = {
        {4U, 0U, FOC_CONFIG_STORE_ERR_RECORD, FOC_DIAG_REASON_BAD_MAGIC},
        {8U, 0x00020003U, FOC_CONFIG_STORE_ERR_FORMAT, FOC_DIAG_REASON_UNSUPPORTED_FORMAT},
        {16U, UINT32_MAX, FOC_CONFIG_STORE_ERR_RECORD, FOC_DIAG_REASON_BAD_ENVELOPE},
        {FOC_CONFIG_STORE_HEADER_BYTES,
         0xFF031005U,
         FOC_CONFIG_STORE_ERR_RECORD,
         FOC_DIAG_REASON_BAD_TLV},
        {FOC_CONFIG_STORE_HEADER_BYTES + 4U,
         0x7FC00000U,
         FOC_CONFIG_STORE_ERR_RECORD,
         FOC_DIAG_REASON_INVALID_PARAMETER},
        {FOC_CONFIG_STORE_HEADER_BYTES + 8U,
         0x04031005U,
         FOC_CONFIG_STORE_ERR_RECORD,
         FOC_DIAG_REASON_DUPLICATE_PARAMETER},
        {FOC_CONFIG_STORE_HEADER_BYTES + 4U,
         0x40A00000U,
         FOC_CONFIG_STORE_ERR_CONFIG,
         FOC_DIAG_REASON_INVALID_PARAMETER},
        {FOC_CONFIG_STORE_HEADER_BYTES + 52U,
         0x40A00000U,
         FOC_CONFIG_STORE_ERR_CONFIG,
         FOC_DIAG_REASON_INVALID_CONFIGURATION},
    };
    for (size_t i = 0U; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        uint8_t bytes[FOC_CONFIG_STORE_SLOT_BYTES];
        memcpy(bytes, original, used);
        put_word(bytes + mutations[i].offset, mutations[i].word);
        if (mutations[i].offset != 16U) {
            fix_crc(bytes);
        }
        CHECK(foc_config_inspect_slot(bytes, used, &policy, &slot) == mutations[i].status);
        CHECK(slot.reason == mutations[i].reason && !slot.applicable);
        if (slot.reason == FOC_DIAG_REASON_DUPLICATE_PARAMETER) {
            CHECK(
                slot.crc == FOC_DIAG_PASS && slot.tlv == FOC_DIAG_FAIL &&
                slot.integrity == FOC_DIAG_FAIL
            );
            CHECK(foc_config_inspect_slot(bytes, used, NULL, &slot) == FOC_CONFIG_STORE_ERR_RECORD);
            CHECK(slot.reason == FOC_DIAG_REASON_DUPLICATE_PARAMETER);
        }
        if (slot.reason == FOC_DIAG_REASON_INVALID_CONFIGURATION) {
            CHECK(slot.integrity == FOC_DIAG_PASS && slot.semantics == FOC_DIAG_FAIL);
        }
    }
    uint8_t layered[FOC_CONFIG_STORE_SLOT_BYTES];
    memcpy(layered, original, used);
    put_word(layered + FOC_CONFIG_STORE_HEADER_BYTES, 0x04031070U);
    put_word(layered + FOC_CONFIG_STORE_HEADER_BYTES + 8U, 0x04031070U);
    fix_crc(layered);
    CHECK(foc_config_inspect_slot(layered, used, &policy, &slot) == FOC_CONFIG_STORE_ERR_SCHEMA);
    CHECK(
        slot.crc == FOC_DIAG_PASS && slot.schema == FOC_DIAG_FAIL && slot.tlv == FOC_DIAG_FAIL &&
        slot.reason == FOC_DIAG_REASON_UNSUPPORTED_SCHEMA &&
        slot.tlv_reason == FOC_DIAG_REASON_DUPLICATE_PARAMETER
    );
    puts("MALFORMED magic, bounds, unknown Format, TLV, duplicate and invalid protection PASS");
    return 0;
}

static int missing_diff(void)
{
    struct foc_config_diff_entry rows[24];
    size_t count;
    struct foc_runtime_config alternate = base;
    alternate.motion.impedance.enabled = false;
    alternate.motion.limits.tolerance_rad = -0.0f;
    struct foc_runtime_config default_config = base;
    default_config.motion.limits.tolerance_rad = 0.0f;
    const struct foc_config_diff_input other_profile = {&alternate, FOC_CONFIG_STORE_OK};
    const struct foc_config_diff_input defaults = {&default_config, FOC_CONFIG_STORE_OK};
    CHECK(foc_config_diff(&other_profile, &defaults, rows, 24U, &count) == FOC_CONFIG_STORE_OK);
    bool saw_missing = false, saw_signed_zero = false;
    for (size_t i = 0U; i < count; ++i) {
        if (rows[i].parameter->id == FOC_PARAMETER_TORQUE_LIMIT) {
            CHECK(
                rows[i].kind == FOC_DIAG_MISSING_LEFT && !rows[i].left_present &&
                rows[i].right_present
            );
            saw_missing = true;
        }
        if (rows[i].parameter->id == FOC_PARAMETER_LIMIT_TOLERANCE) {
            CHECK(rows[i].kind == FOC_DIAG_VALUE_CHANGED);
            saw_signed_zero = true;
        }
    }
    CHECK(saw_missing && saw_signed_zero);
    const struct foc_config_diff_input incompatible = {NULL, FOC_CONFIG_STORE_ERR_IDENTITY};
    CHECK(foc_config_diff(&defaults, &incompatible, rows, 24U, &count) == FOC_CONFIG_STORE_OK);
    for (size_t i = 0U; i < count; ++i) {
        CHECK(rows[i].kind == FOC_DIAG_IDENTITY_MISMATCH && !rows[i].right_present);
    }
    puts("DIFF exact signed zero, missing profile field and no values from rejected source PASS");
    return 0;
}

static void
concurrent_stop(struct foc_runtime *runtime, enum foc_runtime_hook_point point, void *argument)
{
    bool *pending = argument;
    if (*pending && point == FOC_RUNTIME_HOOK_TELEMETRY_READING) {
        *pending = false;
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_WDT);
        const struct foc_runtime_fast_input input = {.now_us = 0U};
        (void)foc_runtime_h0_step(runtime, &input);
    }
}

static int fault_snapshot(void)
{
    struct foc_runtime_config config = base;
    bool pending = true;
    config.hook = concurrent_stop;
    config.hook_arg = &pending;
    struct foc_runtime runtime;
    CHECK(foc_runtime_init(&runtime, &pipeline, &config, 0U) == FOC_RUNTIME_OK);
    struct foc_runtime_diagnostic rejected = {
        .source = FOC_DIAG_SOURCE_DEFAULT_CONFIG,
        .snapshot.motion_session = 77U,
    };
    CHECK(foc_runtime_diagnose(&runtime, &rejected) == FOC_RUNTIME_ERR_BUSY);
    CHECK(
        rejected.source == FOC_DIAG_SOURCE_DEFAULT_CONFIG && rejected.snapshot.motion_session == 77U
    );
    struct foc_runtime_snapshot before, after;
    CHECK(foc_runtime_read_snapshot(&runtime, &before) == FOC_RUNTIME_OK);
    CHECK(
        before.state == FOC_SAFE_STATE_FAULT_LATCHED && before.faults == FOC_SAFE_FAULT_WDT &&
        before.action == FOC_DEVICE_OUTPUT_INHIBIT && !before.updates
    );
    struct foc_runtime_diagnostic observed;
    CHECK(foc_runtime_diagnose(&runtime, &observed) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_read_snapshot(&runtime, &after) == FOC_RUNTIME_OK);
    CHECK(
        before.state == after.state && observed.snapshot.state == before.state &&
        before.faults == after.faults && observed.snapshot.faults == before.faults &&
        before.motion_session == after.motion_session &&
        observed.snapshot.motion_session == before.motion_session &&
        before.action == after.action && before.updates == after.updates &&
        before.output_confirmed == after.output_confirmed &&
        !observed.effective.motion.limits.enabled
    );
    puts("RUNTIME Snapshot busy preserves output, FAULT/INHIBIT/Session/UPDATE unchanged PASS");
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    CHECK(storage_inspection() == 0);
    CHECK(effective_diff() == 0);
    CHECK(malformed_records() == 0);
    CHECK(missing_diff() == 0);
    CHECK(fault_snapshot() == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
