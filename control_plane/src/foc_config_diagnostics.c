/**
 * @file    foc_config_diagnostics.c
 * @brief   Read-only dual-slot decisions, parameter comparison and Runtime observation.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Reuse the Config Store selector, the parameter catalog and one Runtime
 * snapshot without adding a storage or Runtime state owner.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_config_diagnostics.h"

#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

static bool usable(const struct foc_config_diff_input *input)
{
    return input->config != NULL &&
        (input->status == FOC_CONFIG_STORE_OK || input->status == FOC_CONFIG_STORE_MIGRATED);
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool values_match(const struct foc_parameter_value *a, const struct foc_parameter_value *b)
{
    switch (a->type) {
    case FOC_PARAMETER_I32:
        return a->data.i32 == b->data.i32;
    case FOC_PARAMETER_F32:
        return float_bits(a->data.f32) == float_bits(b->data.f32);
    default:
        return a->data.u32 == b->data.u32;
    }
}

static int static_value(
    const struct foc_config_diff_input *input,
    enum foc_parameter_id id,
    struct foc_parameter_value *value
)
{
    if (!usable(input)) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    const int status = foc_parameter_read_config(input->config, id, value);
    if (status == FOC_PARAMETER_OK && value->type == FOC_PARAMETER_F32 &&
        (float_bits(value->data.f32) & UINT32_C(0x7F800000)) == UINT32_C(0x7F800000)) {
        return FOC_PARAMETER_ERR_RANGE;
    }
    return status;
}

static enum foc_config_diff_kind unavailable_source(
    const struct foc_config_diff_input *left,
    const struct foc_config_diff_input *right
)
{
    if (left->status == FOC_CONFIG_STORE_ERR_SCHEMA ||
        right->status == FOC_CONFIG_STORE_ERR_SCHEMA ||
        left->status == FOC_CONFIG_STORE_ERR_FORMAT ||
        right->status == FOC_CONFIG_STORE_ERR_FORMAT) {
        return FOC_DIAG_SCHEMA_INCOMPATIBLE;
    }
    if (left->status == FOC_CONFIG_STORE_ERR_IDENTITY ||
        right->status == FOC_CONFIG_STORE_ERR_IDENTITY) {
        return FOC_DIAG_IDENTITY_MISMATCH;
    }
    return FOC_DIAG_VALUE_UNAVAILABLE;
}

static struct foc_config_diff_entry compare(
    const struct foc_parameter_descriptor *parameter,
    const struct foc_config_diff_input *left,
    const struct foc_config_diff_input *right
)
{
    struct foc_config_diff_entry entry = {
        .parameter = parameter,
        .motion_protection = parameter->id >= FOC_PARAMETER_LIMITS_ENABLED &&
            parameter->id <= FOC_PARAMETER_FOLLOWING_MAX_GAP_US,
        .kind = FOC_DIAG_VALUE_UNAVAILABLE,
    };
    const int a = static_value(left, parameter->id, &entry.left);
    const int b = static_value(right, parameter->id, &entry.right);
    entry.left_present = a == FOC_PARAMETER_OK;
    entry.right_present = b == FOC_PARAMETER_OK;

    if (!usable(left) || !usable(right)) {
        entry.kind = unavailable_source(left, right);
    } else if (entry.left_present && entry.right_present) {
        entry.kind = entry.left.type != entry.right.type
            ? FOC_DIAG_TYPE_MISMATCH
            : (values_match(&entry.left, &entry.right) ? FOC_DIAG_UNCHANGED
                                                       : FOC_DIAG_VALUE_CHANGED);
    } else if (a == FOC_PARAMETER_ERR_UNAVAILABLE && entry.right_present) {
        entry.kind = FOC_DIAG_MISSING_LEFT;
    } else if (b == FOC_PARAMETER_ERR_UNAVAILABLE && entry.left_present) {
        entry.kind = FOC_DIAG_MISSING_RIGHT;
    }
    return entry;
}

/* Public Functions ------------------------------------------------------------- */

int foc_config_inspect_pair(
    const struct foc_config_store_driver *reader,
    const struct foc_config_store_policy *policy,
    struct foc_config_store_workspace *workspace,
    struct foc_config_diagnostic *result
)
{
    if (reader == NULL || reader->read == NULL || workspace == NULL || result == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    result->source = FOC_DIAG_SOURCE_HOST_STORAGE_IMAGE;
    const int chosen = foc_config_store_select(reader, workspace, &result->recovery);
    for (unsigned int index = 0U; index < 2U; ++index) {
        struct foc_config_slot_diagnostic *slot = &result->slots[index];
        const int status = result->recovery.slot_status[index];
        if (status == FOC_CONFIG_STORE_ERR_IO) {
            *slot = (struct foc_config_slot_diagnostic){
                .envelope_status = status,
                .status = status,
                .reason = FOC_DIAG_REASON_IO,
            };
        } else {
            (void)foc_config_inspect_slot(
                workspace->slots[index],
                FOC_CONFIG_STORE_SLOT_BYTES,
                policy,
                slot
            );
        }
        slot->slot = (enum foc_config_store_slot)index;
        if (chosen == FOC_CONFIG_STORE_OK) {
            if (slot->slot == result->recovery.active_slot) {
                slot->selection = FOC_DIAG_SELECTION_SELECTED;
            } else if (status != FOC_CONFIG_STORE_OK) {
                slot->selection = FOC_DIAG_SELECTION_INELIGIBLE;
            } else {
                /* The selector has already resolved order and rejected conflicting ties. */
                slot->selection = slot->record.sequence == result->recovery.record.sequence
                    ? FOC_DIAG_SELECTION_EQUIVALENT
                    : FOC_DIAG_SELECTION_OLDER;
            }
        }
    }
    result->restore_status =
        chosen == FOC_CONFIG_STORE_OK ? result->slots[result->recovery.active_slot].status : chosen;
    return result->restore_status;
}

size_t foc_config_diff_count(void)
{
    size_t count = 0U;
    for (size_t index = 0U; index < foc_parameter_count(); ++index) {
        if (foc_parameter_at(index)->category == FOC_PARAMETER_CONFIG) {
            ++count;
        }
    }
    return count;
}

int foc_config_diff(
    const struct foc_config_diff_input *left,
    const struct foc_config_diff_input *right,
    struct foc_config_diff_entry *entries,
    size_t capacity,
    size_t *written
)
{
    if (left == NULL || right == NULL || entries == NULL || written == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    if (capacity < foc_config_diff_count()) {
        return FOC_CONFIG_STORE_ERR_CAPACITY;
    }
    size_t cursor = 0U;
    for (size_t index = 0U; index < foc_parameter_count(); ++index) {
        const struct foc_parameter_descriptor *parameter = foc_parameter_at(index);
        if (parameter->category == FOC_PARAMETER_CONFIG) {
            entries[cursor++] = compare(parameter, left, right);
        }
    }
    *written = cursor;
    return FOC_CONFIG_STORE_OK;
}

int foc_runtime_diagnose(struct foc_runtime *runtime, struct foc_runtime_diagnostic *result)
{
    if (result == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    struct foc_runtime_snapshot snapshot;
    const int status = foc_runtime_read_snapshot(runtime, &snapshot);
    if (status == FOC_RUNTIME_OK) {
        result->source = FOC_DIAG_SOURCE_HOST_RUNTIME;
        result->snapshot = snapshot;
        /* The initialized configuration is immutable throughout the caller's observation. */
        result->effective = runtime->config;
    }
    return status;
}

enum foc_config_application foc_config_application_status(
    const struct foc_config_diff_input *candidate,
    const struct foc_runtime_diagnostic *runtime
)
{
    if (candidate == NULL || !usable(candidate)) {
        return FOC_DIAG_CONFIG_UNAVAILABLE;
    }
    if (runtime == NULL || runtime->source != FOC_DIAG_SOURCE_HOST_RUNTIME) {
        return FOC_DIAG_NOT_OBSERVED;
    }
    const struct foc_config_diff_input right = {&runtime->effective, FOC_CONFIG_STORE_OK};
    for (size_t index = 0U; index < foc_parameter_count(); ++index) {
        const struct foc_parameter_descriptor *parameter = foc_parameter_at(index);
        if (parameter->category != FOC_PARAMETER_CONFIG) {
            continue;
        }
        const struct foc_config_diff_entry entry = compare(parameter, candidate, &right);
        if (entry.kind == FOC_DIAG_VALUE_UNAVAILABLE) {
            struct foc_parameter_value value;
            if (static_value(candidate, parameter->id, &value) != FOC_PARAMETER_ERR_UNAVAILABLE ||
                static_value(&right, parameter->id, &value) != FOC_PARAMETER_ERR_UNAVAILABLE) {
                return FOC_DIAG_CONFIG_UNAVAILABLE;
            }
        } else if (entry.kind != FOC_DIAG_UNCHANGED) {
            return FOC_DIAG_REINITIALIZATION_REQUIRED;
        }
    }
    return FOC_DIAG_MATCHES_EFFECTIVE;
}

/* ---------------------------------------- end of file ----------------------------------------- */
