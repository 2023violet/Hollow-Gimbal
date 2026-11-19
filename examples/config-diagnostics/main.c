/**
 * @file    main.c
 * @brief   Inspect Host storage and compare it with observed Runtime settings.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Exercise the public configuration diagnostics API as a standalone C11 consumer.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_config_diagnostics.h"
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

/* Private Helpers -------------------------------------------------------------- */

static int read_slots(
    const struct foc_config_store_driver *reader,
    uint8_t bytes[2][FOC_CONFIG_STORE_SLOT_BYTES]
)
{
    for (unsigned int slot = 0U; slot < 2U; ++slot) {
        CHECK(reader->read(reader->context, slot, 0U, bytes[slot], sizeof(bytes[slot])) == 0);
    }
    return 0;
}

static void print_value(const struct foc_parameter_value *value)
{
    if (value->type == FOC_PARAMETER_F32) {
        printf("%.9g", (double)value->data.f32);
    } else {
        printf("%" PRIu32, value->data.u32);
    }
}

static int show_protection_diff(
    const struct foc_config_diff_input *left,
    const struct foc_config_diff_input *right
)
{
    struct foc_config_diff_entry entries[32];
    size_t count;
    CHECK(
        foc_config_diff(left, right, entries, sizeof(entries) / sizeof(entries[0]), &count) ==
        FOC_CONFIG_STORE_OK
    );
    CHECK(count == foc_config_diff_count());

    puts("DIFF slot_A -> slot_B motion_protection");
    size_t group = 0U;
    size_t changed = 0U;
    for (size_t index = 0U; index < count; ++index) {
        const struct foc_config_diff_entry *entry = &entries[index];
        if (!entry->motion_protection) {
            continue;
        }
        const struct foc_parameter_descriptor *field = entry->parameter;
        CHECK(field == foc_parameter_find(field->id) && field->requires_reinit);
        CHECK(entry->left_present && entry->right_present);
        CHECK(field->type == FOC_PARAMETER_F32 || field->type == FOC_PARAMETER_U32);
        CHECK(entry->left.type == field->type && entry->right.type == field->type);
        CHECK(entry->kind == FOC_DIAG_UNCHANGED || entry->kind == FOC_DIAG_VALUE_CHANGED);
        if (field->id == FOC_PARAMETER_LIMITS_ENABLED ||
            field->id == FOC_PARAMETER_FOLLOWING_ENABLED) {
            CHECK(entry->kind == FOC_DIAG_VALUE_CHANGED);
            CHECK(entry->left.data.u32 == 0U && entry->right.data.u32 == 1U);
        }
        ++group;
        changed += entry->kind == FOC_DIAG_VALUE_CHANGED;
        printf("  0x%04x %s [%s] ", (unsigned int)field->id, field->name, field->unit);
        print_value(&entry->left);
        printf(" -> ");
        print_value(&entry->right);
        printf(" %s\n", entry->kind == FOC_DIAG_VALUE_CHANGED ? "VALUE_CHANGED" : "UNCHANGED");
    }
    CHECK(group == 9U && changed == 6U);
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    const uint32_t frame = 71U;
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
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
            .mechanical_frame_id = frame,
            .encoder_identity = 101U,
            .motor_setup_id = 201U,
            .pole_pairs = profile.runtime.motion.rotor.pole_pairs,
        },
    };
    const struct foc_parameter_edit defaults[] = {
        {FOC_PARAMETER_LIMIT_POSITION_MIN, {FOC_PARAMETER_F32, {.f32 = -0.8f}}},
        {FOC_PARAMETER_LIMIT_POSITION_MAX, {FOC_PARAMETER_F32, {.f32 = 0.8f}}},
        {FOC_PARAMETER_LIMIT_TOLERANCE, {FOC_PARAMETER_F32, {.f32 = 0.005f}}},
        {FOC_PARAMETER_LIMIT_MECHANICAL_FRAME, {FOC_PARAMETER_U32, {.u32 = frame}}},
        {FOC_PARAMETER_FOLLOWING_THRESHOLD, {FOC_PARAMETER_F32, {.f32 = 0.1f}}},
        {FOC_PARAMETER_FOLLOWING_DURATION_US, {FOC_PARAMETER_U32, {.u32 = 20000U}}},
        {FOC_PARAMETER_FOLLOWING_MAX_GAP_US, {FOC_PARAMETER_U32, {.u32 = 1000U}}},
    };
    const struct foc_parameter_edit edits[] = {
        {FOC_PARAMETER_LIMITS_ENABLED, {FOC_PARAMETER_U32, {.u32 = 1U}}},
        {FOC_PARAMETER_LIMIT_POSITION_MIN, {FOC_PARAMETER_F32, {.f32 = -0.7f}}},
        {FOC_PARAMETER_LIMIT_POSITION_MAX, {FOC_PARAMETER_F32, {.f32 = 0.7f}}},
        {FOC_PARAMETER_FOLLOWING_ENABLED, {FOC_PARAMETER_U32, {.u32 = 1U}}},
        {FOC_PARAMETER_FOLLOWING_THRESHOLD, {FOC_PARAMETER_F32, {.f32 = 0.06f}}},
        {FOC_PARAMETER_FOLLOWING_DURATION_US, {FOC_PARAMETER_U32, {.u32 = 18000U}}},
    };
    struct foc_parameter_candidate original;
    struct foc_parameter_candidate edited;
    CHECK(
        foc_parameter_prepare_initial(
            &pipeline,
            policy.base,
            defaults,
            sizeof(defaults) / sizeof(defaults[0]),
            &original
        ) == FOC_PARAMETER_VALIDATED
    );
    CHECK(
        foc_parameter_prepare_initial(
            &pipeline,
            &original.config,
            edits,
            sizeof(edits) / sizeof(edits[0]),
            &edited
        ) == FOC_PARAMETER_VALIDATED
    );

    struct foc_config_store_host host;
    struct foc_config_store_workspace workspace;
    foc_config_store_host_init(&host);
    const struct foc_config_store_driver storage = foc_config_store_host_driver(&host);
    CHECK(
        foc_config_store_save(&storage, &policy, &workspace, &original, NULL) == FOC_CONFIG_STORE_OK
    );
    CHECK(
        foc_config_store_save(&storage, &policy, &workspace, &edited, NULL) == FOC_CONFIG_STORE_OK
    );
    const struct foc_config_store_driver reader = {
        .context = storage.context,
        .read = storage.read
    };
    uint8_t before[2][FOC_CONFIG_STORE_SLOT_BYTES];
    CHECK(read_slots(&reader, before) == 0);

    struct foc_config_diagnostic image;
    CHECK(foc_config_inspect_pair(&reader, &policy, &workspace, &image) == FOC_CONFIG_STORE_OK);
    CHECK(image.source == FOC_DIAG_SOURCE_HOST_STORAGE_IMAGE);
    CHECK(image.restore_status == FOC_CONFIG_STORE_OK);
    CHECK(image.recovery.active_slot == FOC_CONFIG_STORE_SLOT_B);
    CHECK(image.slots[FOC_CONFIG_STORE_SLOT_A].selection == FOC_DIAG_SELECTION_OLDER);
    CHECK(image.slots[FOC_CONFIG_STORE_SLOT_B].selection == FOC_DIAG_SELECTION_SELECTED);
    for (unsigned int index = 0U; index < 2U; ++index) {
        const struct foc_config_slot_diagnostic *slot = &image.slots[index];
        CHECK(slot->slot == (enum foc_config_store_slot)index);
        CHECK(slot->magic == FOC_DIAG_PASS && slot->crc == FOC_DIAG_PASS);
        CHECK(slot->commit == FOC_DIAG_COMMIT_COMPLETE && slot->applicable);
        printf(
            "HOST_STORAGE_IMAGE slot=%c sequence=%" PRIu32 " format=%u schema=%u "
            "crc=PASS commit=COMPLETE applicable=true selected=%s\n",
            'A' + (int)index,
            slot->record.sequence,
            (unsigned int)slot->format,
            (unsigned int)slot->record.schema,
            slot->selection == FOC_DIAG_SELECTION_SELECTED ? "true" : "false"
        );
    }
    const struct foc_config_diff_input left = {
        .config = &image.slots[FOC_CONFIG_STORE_SLOT_A].candidate.config,
        .status = FOC_CONFIG_STORE_OK,
    };
    const struct foc_config_diff_input right = {
        .config = &image.slots[FOC_CONFIG_STORE_SLOT_B].candidate.config,
        .status = FOC_CONFIG_STORE_OK,
    };
    CHECK(show_protection_diff(&left, &right) == 0);

    struct foc_config_store_policy other_encoder = policy;
    other_encoder.identity.encoder_cpr /= 2U;
    struct foc_config_diagnostic rejected;
    CHECK(
        foc_config_inspect_pair(&reader, &other_encoder, &workspace, &rejected) ==
        FOC_CONFIG_STORE_ERR_IDENTITY
    );
    CHECK(rejected.restore_status == FOC_CONFIG_STORE_ERR_IDENTITY);
    CHECK(rejected.recovery.active_slot == FOC_CONFIG_STORE_SLOT_B);
    const struct foc_config_slot_diagnostic *slot_b = &rejected.slots[FOC_CONFIG_STORE_SLOT_B];
    CHECK(slot_b->envelope_status == FOC_CONFIG_STORE_OK && !slot_b->applicable);
    CHECK(slot_b->crc == FOC_DIAG_PASS && slot_b->identity == FOC_DIAG_FAIL);
    CHECK(slot_b->reason == FOC_DIAG_REASON_IDENTITY_MISMATCH);
    CHECK(slot_b->identity_mismatches == FOC_DIAG_ID_CPR);
    printf(
        "HOST_STORAGE_IMAGE restore=IDENTITY_MISMATCH slot=B encoder_cpr stored=%" PRIu32
        " expected=%" PRIu32 "\n",
        slot_b->recorded_identity.encoder_cpr,
        other_encoder.identity.encoder_cpr
    );

    /* Initialize two separate instances. All Runtime reads are through the public APIs. */
    struct foc_virtual_current previous;
    profile.runtime = *left.config;
    CHECK(foc_virtual_current_init(&previous, &profile) == 0);
    CHECK(foc_virtual_current_run_until(&previous, 0U) == 0);
    struct foc_runtime_snapshot snapshot_before;
    CHECK(foc_runtime_read_snapshot(&previous.runtime, &snapshot_before) == FOC_RUNTIME_OK);
    struct foc_runtime_diagnostic observed;
    CHECK(foc_runtime_diagnose(&previous.runtime, &observed) == FOC_RUNTIME_OK);
    CHECK(observed.source == FOC_DIAG_SOURCE_HOST_RUNTIME);
    CHECK(foc_config_application_status(&left, &observed) == FOC_DIAG_MATCHES_EFFECTIVE);
    CHECK(foc_config_application_status(&right, &observed) == FOC_DIAG_REINITIALIZATION_REQUIRED);
    CHECK(right.config->motion.limits.enabled && right.config->motion.following_error.enabled);
    CHECK(
        !observed.effective.motion.limits.enabled &&
        !observed.effective.motion.following_error.enabled
    );
    CHECK(observed.snapshot.state == FOC_SAFE_STATE_DISABLED);
    CHECK(
        observed.snapshot.output_confirmed && observed.snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT
    );
    CHECK(observed.snapshot.updates == 0U);
    puts(
        "HOST_RUNTIME slot=B REINITIALIZATION_REQUIRED candidate_protection=on "
        "effective_protection=off"
    );

    struct foc_virtual_current fresh;
    profile.runtime = *right.config;
    CHECK(foc_virtual_current_init(&fresh, &profile) == 0);
    CHECK(foc_virtual_current_run_until(&fresh, 0U) == 0);
    struct foc_runtime_diagnostic applied;
    CHECK(foc_runtime_diagnose(&fresh.runtime, &applied) == FOC_RUNTIME_OK);
    CHECK(foc_config_application_status(&right, &applied) == FOC_DIAG_MATCHES_EFFECTIVE);
    CHECK(
        applied.snapshot.output_confirmed && applied.snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT
    );
    CHECK(applied.snapshot.updates == 0U);
    puts(
        "HOST_RUNTIME slot=B MATCHES_EFFECTIVE after fresh initialization output=INHIBIT updates=0"
    );

    struct foc_runtime_diagnostic after;
    CHECK(foc_runtime_diagnose(&previous.runtime, &after) == FOC_RUNTIME_OK);
    CHECK(foc_config_application_status(&left, &after) == FOC_DIAG_MATCHES_EFFECTIVE);
    CHECK(after.snapshot.motion_session == snapshot_before.motion_session);
    CHECK(
        after.snapshot.state == snapshot_before.state &&
        after.snapshot.faults == snapshot_before.faults
    );
    CHECK(
        after.snapshot.action == snapshot_before.action &&
        after.snapshot.updates == snapshot_before.updates
    );
    CHECK(after.snapshot.output_confirmed == snapshot_before.output_confirmed);
    CHECK(after.snapshot.inhibit_pending == snapshot_before.inhibit_pending);
    CHECK(after.snapshot.following_state == snapshot_before.following_state);
    CHECK(after.snapshot.motion_rejection == snapshot_before.motion_rejection);
    uint8_t bytes_after[2][FOC_CONFIG_STORE_SLOT_BYTES];
    CHECK(read_slots(&reader, bytes_after) == 0);
    CHECK(memcmp(before, bytes_after, sizeof(before)) == 0);
    printf(
        "READ_ONLY storage_bytes=%zu unchanged runtime_session=%" PRIu32 " unchanged\n",
        sizeof(before),
        after.snapshot.motion_session
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
