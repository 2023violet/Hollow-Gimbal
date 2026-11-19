/**
 * @file    diagnostics_output.c
 * @brief   Render C11 diagnostic states without making new restore decisions.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Explanation strings are local to the Host CLI. Selection, applicability
 * and field comparisons arrive already classified by the C11 diagnostics.
 */

/* Includes --------------------------------------------------------------------- */
#include "diagnostics_output.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */
#define JSON_SCHEMA_VERSION 1U
#define STORAGE_BOUNDARY                                                                  \
    "Host storage image only. CRC-32 detects corruption. Runtime and physical Flash are " \
    "unobserved."
#define REASON(code, text) [FOC_DIAG_REASON_##code] = {#code, text}
#define NOTE(code, text)   [NOTE_##code] = {#code, text}

/* Private Types ---------------------------------------------------------------- */
enum inspector_note {
    NOTE_SEQUENCE_AMBIGUOUS = FOC_DIAG_REASON_IO + 1,
    NOTE_NO_VALID_RECORD,
    NOTE_OLDER,
    NOTE_EQUIVALENT,
    NOTE_INELIGIBLE,
    NOTE_REINITIALIZATION_REQUIRED,
    NOTE_NOT_APPLIED,
    NOTE_OUTPUT_LOCKED,
    NOTE_INVALID_PROTECTION_CONFIG,
    NOTE_SNAPSHOT_UNAVAILABLE,
};

struct inspector_explanation {
    const char *code;
    const char *message;
};

/* Private State ---------------------------------------------------------------- */
static const struct inspector_explanation explanations[] = {
    REASON(NONE, "This check found no rejection."),
    REASON(RECORD_NOT_FOUND, "The storage slot is erased."),
    REASON(BAD_MAGIC, "The slot contains bytes without the expected FalconFOC record magic."),
    REASON(BAD_CRC, "The stored CRC-32 does not match the verified record bytes."),
    REASON(
        INCOMPLETE_COMMIT,
        "The final commit word is erased or torn. Recovery ignores this record."
    ),
    REASON(
        UNSUPPORTED_FORMAT,
        "The record format is unsupported. Restore cannot establish sequence order safely."
    ),
    REASON(
        UNSUPPORTED_SCHEMA,
        "The selected record uses an unsupported schema. Restore does not fall back to older "
        "settings."
    ),
    REASON(BAD_ENVELOPE, "The recorded size or header values fall outside the binary contract."),
    REASON(BAD_TLV, "A typed field has an invalid length or extends outside the payload."),
    REASON(DUPLICATE_PARAMETER, "The record repeats a stable parameter ID."),
    REASON(INVALID_PARAMETER, "A stored parameter has an invalid ID, type or value."),
    REASON(
        INVALID_CONFIGURATION,
        "The values fail the combined configuration rules, which include motion protection and "
        "mechanical frame constraints."
    ),
    REASON(MISSING_PARAMETER, "The record omits at least one required configuration field."),
    REASON(
        IDENTITY_UNKNOWN,
        "A separately qualified setup identity is needed to evaluate applicability."
    ),
    REASON(
        IDENTITY_MISMATCH,
        "Stored motor, encoder or mechanical frame identity differs from the supplied setup. Check "
        "the reported identity fields."
    ),
    REASON(
        MIGRATION_REQUIRED,
        "Schema 1 lacks the current motion protection group. Restore requires explicit "
        "reconfirmation of that complete group."
    ),
    REASON(IO, "A storage read did not return the expected bytes."),
    NOTE(
        SEQUENCE_AMBIGUOUS,
        "The sequence numbers are half a cycle apart, or equal with different record contents. "
        "Restore refuses to guess."
    ),
    NOTE(
        NO_VALID_RECORD,
        "Neither slot contains an eligible committed envelope. Check the rejection reported for "
        "each slot."
    ),
    NOTE(OLDER, "The Storage Manager selected the other slot using its wrap-aware sequence rule."),
    NOTE(
        EQUIVALENT,
        "Both slots contain the same committed record at the same sequence. The Storage Manager "
        "selects Slot A."
    ),
    NOTE(
        INELIGIBLE,
        "This slot did not qualify for selection. Check its record reason and the overall recovery "
        "reason."
    ),
    NOTE(
        REINITIALIZATION_REQUIRED,
        "Static parameters only take effect after legal initialization of a fresh Runtime with the "
        "candidate."
    ),
    NOTE(
        NOT_APPLIED,
        "Storage recovery provides a detached candidate. A coherent Runtime observation is needed "
        "to compare effective values."
    ),
    NOTE(
        OUTPUT_LOCKED,
        "The reference firmware locks output. Diagnostic reads do not grant permission to drive "
        "the power stage."
    ),
    NOTE(
        INVALID_PROTECTION_CONFIG,
        "Motion protection must satisfy the setup's qualified bounds, timing and mechanical frame "
        "rules."
    ),
    NOTE(
        SNAPSHOT_UNAVAILABLE,
        "The coherent Runtime snapshot could not be captured. Current effective state is unknown."
    ),
};

static const char *const source_names[] = {
    [FOC_DIAG_SOURCE_HOST_STORAGE_IMAGE] = "HOST_STORAGE_IMAGE",
    [FOC_DIAG_SOURCE_HOST_RUNTIME] = "HOST_RUNTIME",
    [FOC_DIAG_SOURCE_DEFAULT_CONFIG] = "DEFAULT_CONFIG",
    [FOC_DIAG_SOURCE_PENDING_CONFIG] = "PENDING_CONFIG",
};

static const char *const commit_names[] = {
    [FOC_DIAG_COMMIT_ERASED] = "ERASED",
    [FOC_DIAG_COMMIT_COMPLETE] = "COMPLETE",
    [FOC_DIAG_COMMIT_TORN] = "TORN",
};

static const char *const selection_names[] = {
    [FOC_DIAG_SELECTION_UNDECIDED] = "UNDECIDED",
    [FOC_DIAG_SELECTION_SELECTED] = "SELECTED",
    [FOC_DIAG_SELECTION_OLDER] = "OLDER",
    [FOC_DIAG_SELECTION_EQUIVALENT] = "EQUIVALENT",
    [FOC_DIAG_SELECTION_INELIGIBLE] = "INELIGIBLE",
};

static const char *const diff_names[] = {
    [FOC_DIAG_UNCHANGED] = "UNCHANGED",
    [FOC_DIAG_VALUE_CHANGED] = "VALUE_CHANGED",
    [FOC_DIAG_MISSING_LEFT] = "MISSING_LEFT",
    [FOC_DIAG_MISSING_RIGHT] = "MISSING_RIGHT",
    [FOC_DIAG_TYPE_MISMATCH] = "TYPE_MISMATCH",
    [FOC_DIAG_SCHEMA_INCOMPATIBLE] = "SCHEMA_INCOMPATIBLE",
    [FOC_DIAG_IDENTITY_MISMATCH] = "IDENTITY_MISMATCH",
    [FOC_DIAG_VALUE_UNAVAILABLE] = "VALUE_UNAVAILABLE",
};

static const char *const type_names[] = {
    [FOC_PARAMETER_NONE] = "NONE",
    [FOC_PARAMETER_U32] = "U32",
    [FOC_PARAMETER_I32] = "I32",
    [FOC_PARAMETER_F32] = "F32",
    [FOC_PARAMETER_ENUM] = "ENUM",
};

static const struct {
    uint32_t mask;
    const char *name;
} identity_fields[] = {
    {FOC_DIAG_ID_POLE_PAIRS, "pole_pairs"},
    {FOC_DIAG_ID_PROFILE, "impedance_profile"},
    {FOC_DIAG_ID_CPR, "encoder_cpr"},
    {FOC_DIAG_ID_DIRECTION, "encoder_direction"},
    {FOC_DIAG_ID_FRAME, "mechanical_frame_id"},
    {FOC_DIAG_ID_ENCODER, "encoder_identity"},
    {FOC_DIAG_ID_OFFSET, "electrical_offset_bits"},
    {FOC_DIAG_ID_MOTOR, "motor_setup_id"},
    {FOC_DIAG_ID_KT, "kt_bits"},
};

/* Private Helpers -------------------------------------------------------------- */
static const struct inspector_explanation *find_explanation(const char *code)
{
    for (size_t index = 0U; index < sizeof(explanations) / sizeof(explanations[0]); ++index) {
        if (strcmp(code, explanations[index].code) == 0) {
            return &explanations[index];
        }
    }
    return NULL;
}

static const char *slot_name(enum foc_config_store_slot slot)
{
    static const char *const names[] = {"A", "B", NULL};
    return names[slot];
}

static const char *check_name(enum foc_diagnostic_check check)
{
    static const char *const names[] = {NULL, "PASS", "FAIL"};
    return names[check];
}

static enum foc_diagnostic_check slot_applicability(const struct foc_config_slot_diagnostic *slot)
{
    if (slot->reason == FOC_DIAG_REASON_IDENTITY_UNKNOWN) {
        return FOC_DIAG_UNKNOWN;
    }
    return slot->applicable ? FOC_DIAG_PASS : FOC_DIAG_FAIL;
}

static enum foc_diagnostic_check recovery_availability(const struct foc_config_diagnostic *report)
{
    switch (report->restore_status) {
    case FOC_CONFIG_STORE_OK:
    case FOC_CONFIG_STORE_MIGRATED:
        return FOC_DIAG_PASS;
    case FOC_CONFIG_STORE_ERR_IDENTITY_UNKNOWN:
        return FOC_DIAG_UNKNOWN;
    default:
        return FOC_DIAG_FAIL;
    }
}

static const char *unknown_text(const char *value)
{
    return value != NULL ? value : "UNKNOWN";
}

static void json_string(const char *value)
{
    if (value == NULL) {
        fputs("null", stdout);
        return;
    }
    putchar('"');
    for (const unsigned char *byte = (const unsigned char *)value; *byte != '\0'; ++byte) {
        if (*byte == '"' || *byte == '\\') {
            putchar('\\');
            putchar(*byte);
        } else if (*byte < 0x20U) {
            printf("\\u%04x", (unsigned)*byte);
        } else {
            putchar(*byte);
        }
    }
    putchar('"');
}

static void json_check_boolean(enum foc_diagnostic_check check)
{
    static const char *const values[] = {"null", "true", "false"};
    fputs(values[check], stdout);
}

static void json_optional_u32(bool present, uint32_t value)
{
    if (present) {
        printf("%" PRIu32, value);
    } else {
        fputs("null", stdout);
    }
}

static void json_bits(uint32_t bits)
{
    printf("\"0x%08" PRIx32 "\"", bits);
}

static void json_prefix(const char *command, const char *profile)
{
    printf("{\"schema_version\":%u,\"command\":", JSON_SCHEMA_VERSION);
    json_string(command);
    fputs(",\"source\":\"HOST_STORAGE_IMAGE\",\"trust_boundary\":", stdout);
    json_string(STORAGE_BOUNDARY);
    fputs(",\"profile\":", stdout);
    json_string(profile);
    fputs(",\"runtime_applied\":null", stdout);
}

static bool format_known(const struct foc_config_slot_diagnostic *slot)
{
    return slot->header_present && slot->format == FOC_CONFIG_STORE_FORMAT_VERSION;
}

static void json_identity(const struct foc_config_slot_diagnostic *slot)
{
    if (!format_known(slot)) {
        fputs("null", stdout);
        return;
    }
    const struct foc_config_store_identity *identity = &slot->recorded_identity;
    printf(
        "{\"encoder_cpr\":%" PRIu32 ",\"encoder_direction\":%" PRId32
        ",\"mechanical_frame_id\":%" PRIu32 ",\"encoder_identity\":%" PRIu32
        ",\"motor_setup_id\":%" PRIu32 ",\"pole_pairs\":%u,\"impedance_profile\":%s"
        ",\"electrical_offset_bits\":",
        identity->encoder_cpr,
        identity->encoder_direction,
        identity->mechanical_frame_id,
        identity->encoder_identity,
        identity->motor_setup_id,
        (unsigned)identity->pole_pairs,
        slot->impedance_profile ? "true" : "false"
    );
    json_bits(slot->electrical_offset_bits);
    fputs(",\"kt_bits\":", stdout);
    json_bits(slot->kt_bits);
    putchar('}');
}

static void print_mismatches(const struct foc_config_slot_diagnostic *slot, bool json)
{
    if (json) {
        if (slot->identity == FOC_DIAG_UNKNOWN) {
            fputs("null", stdout);
            return;
        }
        putchar('[');
    }
    bool first = true;
    for (size_t index = 0U; index < sizeof(identity_fields) / sizeof(identity_fields[0]); ++index) {
        if ((slot->identity_mismatches & identity_fields[index].mask) == 0U) {
            continue;
        }
        if (json) {
            if (!first) {
                putchar(',');
            }
            json_string(identity_fields[index].name);
        } else {
            printf(" %s", identity_fields[index].name);
        }
        first = false;
    }
    putchar(json ? ']' : '\n');
}

static void json_slot(const struct foc_config_slot_diagnostic *slot)
{
    const char *reason = inspector_slot_reason(slot);
    printf(
        "{\"slot\":\"%s\",\"record_present\":%s,\"format_version\":",
        slot_name(slot->slot),
        slot->header_present ? "true" : "false"
    );
    json_optional_u32(slot->header_present, slot->format);
    fputs(",\"schema_version\":", stdout);
    json_optional_u32(format_known(slot), slot->record.schema);
    fputs(",\"sequence\":", stdout);
    json_optional_u32(format_known(slot), slot->record.sequence);
    fputs(",\"payload_length\":", stdout);
    json_optional_u32(slot->header_present, slot->payload_length);
    fputs(",\"record_count\":", stdout);
    json_optional_u32(format_known(slot), slot->record.count);
    fputs(",\"magic\":", stdout);
    json_string(check_name(slot->magic));
    fputs(",\"crc\":", stdout);
    json_string(check_name(slot->crc));
    fputs(",\"integrity_valid\":", stdout);
    json_check_boolean(slot->integrity);
    fputs(",\"commit\":", stdout);
    json_string(commit_names[slot->commit]);
    fputs(",\"schema\":", stdout);
    json_string(check_name(slot->schema));
    fputs(",\"tlv\":", stdout);
    json_string(check_name(slot->tlv));
    fputs(",\"tlv_reason\":", stdout);
    json_string(
        slot->tlv_reason == FOC_DIAG_REASON_NONE ? NULL : explanations[slot->tlv_reason].code
    );
    fputs(",\"semantics\":", stdout);
    json_string(check_name(slot->semantics));
    fputs(",\"identity\":", stdout);
    json_string(check_name(slot->identity));
    fputs(",\"recorded_identity\":", stdout);
    json_identity(slot);
    fputs(",\"identity_mismatches\":", stdout);
    print_mismatches(slot, true);
    fputs(",\"migration_needed\":", stdout);
    const bool evaluated = slot->schema != FOC_DIAG_UNKNOWN;
    fputs(evaluated ? (slot->migration_needed ? "true" : "false") : "null", stdout);
    fputs(",\"applicable\":", stdout);
    json_check_boolean(slot_applicability(slot));
    printf(",\"selection\":\"%s\",\"reason\":", selection_names[slot->selection]);
    json_string(reason);
    fputs(",\"explanation\":", stdout);
    json_string(find_explanation(reason)->message);
    printf(",\"envelope_status\":%d,\"status\":%d}", slot->envelope_status, slot->status);
}

static void text_slot(const struct foc_config_slot_diagnostic *slot)
{
    const char *reason = inspector_slot_reason(slot);
    printf("Slot %s: %s (%s)\n", slot_name(slot->slot), reason, selection_names[slot->selection]);
    if (slot->header_present) {
        printf("  format=%u payload=%u", (unsigned)slot->format, (unsigned)slot->payload_length);
        if (format_known(slot)) {
            printf(
                " schema_version=%u sequence=%" PRIu32 " fields=%u",
                (unsigned)slot->record.schema,
                slot->record.sequence,
                (unsigned)slot->record.count
            );
        }
        putchar('\n');
    }
    printf(
        "  magic=%s commit=%s crc=%s integrity=%s tlv=%s\n"
        "  schema=%s semantics=%s identity=%s applicable=%s\n",
        unknown_text(check_name(slot->magic)),
        unknown_text(commit_names[slot->commit]),
        unknown_text(check_name(slot->crc)),
        unknown_text(check_name(slot->integrity)),
        unknown_text(check_name(slot->tlv)),
        unknown_text(check_name(slot->schema)),
        unknown_text(check_name(slot->semantics)),
        unknown_text(check_name(slot->identity)),
        unknown_text(check_name(slot_applicability(slot)))
    );
    if (slot->migration_needed) {
        puts("  Migration needed.");
    }
    if (format_known(slot)) {
        const struct foc_config_store_identity *identity = &slot->recorded_identity;
        printf(
            "  Recorded setup%s: cpr=%" PRIu32 " direction=%" PRId32 " frame=%" PRIu32
            " encoder=%" PRIu32 " motor=%" PRIu32 " poles=%u\n",
            slot->crc == FOC_DIAG_PASS ? "" : " (CRC unverified)",
            identity->encoder_cpr,
            identity->encoder_direction,
            identity->mechanical_frame_id,
            identity->encoder_identity,
            identity->motor_setup_id,
            (unsigned)identity->pole_pairs
        );
    }
    if (slot->identity_mismatches != 0U) {
        fputs("  Unmatched setup identity fields:", stdout);
        print_mismatches(slot, false);
    }
    if (slot->reason != FOC_DIAG_REASON_NONE) {
        printf("  %s\n", find_explanation(reason)->message);
    }
    if (slot->tlv_reason != FOC_DIAG_REASON_NONE && slot->tlv_reason != slot->reason) {
        const struct inspector_explanation *note = &explanations[slot->tlv_reason];
        printf("  TLV %s. %s\n", note->code, note->message);
    }
    if (slot->selection == FOC_DIAG_SELECTION_OLDER ||
        slot->selection == FOC_DIAG_SELECTION_EQUIVALENT) {
        printf("  %s\n", find_explanation(selection_names[slot->selection])->message);
    }
}

static void json_diff_source(const struct inspector_diff_source *side)
{
    fputs("{\"source\":", stdout);
    json_string(source_names[side->source]);
    fputs(",\"slot\":", stdout);
    json_string(slot_name(side->slot));
    printf(",\"available\":%s,\"reason\":", side->input.config != NULL ? "true" : "false");
    json_string(side->reason);
    printf(",\"status\":%d}", side->input.status);
}

static void text_diff_source(const char *label, const struct inspector_diff_source *side)
{
    printf("%s: %s", label, source_names[side->source]);
    if (side->slot != FOC_CONFIG_STORE_SLOT_NONE) {
        printf(" slot=%s", slot_name(side->slot));
    }
    printf(" available=%s reason=%s\n", side->input.config != NULL ? "yes" : "no", side->reason);
}

static void print_value(const struct foc_parameter_value *value, bool present, bool json)
{
    if (present) {
        switch (value->type) {
        case FOC_PARAMETER_U32:
        case FOC_PARAMETER_ENUM:
            printf("%" PRIu32, value->data.u32);
            return;
        case FOC_PARAMETER_I32:
            printf("%" PRId32, value->data.i32);
            return;
        case FOC_PARAMETER_F32:
            if (isfinite(value->data.f32)) {
                printf("%.9g", (double)value->data.f32);
                return;
            }
            break;
        default:
            break;
        }
    }
    fputs(json ? "null" : "UNAVAILABLE", stdout);
}

static void print_f32_bits(const struct foc_parameter_value *value, bool present)
{
    if (present && value->type == FOC_PARAMETER_F32) {
        uint32_t bits;
        memcpy(&bits, &value->data.f32, sizeof(bits));
        json_bits(bits);
    } else {
        fputs("null", stdout);
    }
}

static void json_diff_field(const struct foc_config_diff_entry *field)
{
    const struct foc_parameter_descriptor *parameter = field->parameter;
    printf("{\"parameter_id\":%u,\"name\":", (unsigned)parameter->id);
    json_string(parameter->name);
    fputs(",\"type\":", stdout);
    json_string(type_names[parameter->type]);
    fputs(",\"unit\":", stdout);
    json_string(parameter->unit);
    fputs(",\"left\":", stdout);
    print_value(&field->left, field->left_present, true);
    fputs(",\"right\":", stdout);
    print_value(&field->right, field->right_present, true);
    fputs(",\"left_f32_bits\":", stdout);
    print_f32_bits(&field->left, field->left_present);
    fputs(",\"right_f32_bits\":", stdout);
    print_f32_bits(&field->right, field->right_present);
    printf(
        ",\"classification\":\"%s\",\"motion_protection\":%s,\"requires_reinitialization\":%s}",
        diff_names[field->kind],
        field->motion_protection ? "true" : "false",
        parameter->requires_reinit ? "true" : "false"
    );
}

static void text_diff_field(const struct foc_config_diff_entry *field)
{
    const struct foc_parameter_descriptor *parameter = field->parameter;
    printf(
        "  0x%04x %s (%s",
        (unsigned)parameter->id,
        parameter->name,
        type_names[parameter->type]
    );
    if (parameter->unit != NULL && parameter->unit[0] != '\0') {
        printf(", %s", parameter->unit);
    }
    fputs("): ", stdout);
    print_value(&field->left, field->left_present, false);
    fputs(" -> ", stdout);
    print_value(&field->right, field->right_present, false);
    printf(
        "  %s%s\n",
        diff_names[field->kind],
        parameter->requires_reinit ? " [reinitialize]" : ""
    );
}

/* Public Functions ------------------------------------------------------------- */
const char *inspector_slot_reason(const struct foc_config_slot_diagnostic *slot)
{
    return explanations[slot->reason].code;
}

const char *inspector_restore_reason(const struct foc_config_diagnostic *report)
{
    switch (report->restore_status) {
    case FOC_CONFIG_STORE_OK:
    case FOC_CONFIG_STORE_MIGRATED:
        return explanations[FOC_DIAG_REASON_NONE].code;
    case FOC_CONFIG_STORE_EMPTY:
        return explanations[FOC_DIAG_REASON_RECORD_NOT_FOUND].code;
    case FOC_CONFIG_STORE_ERR_FORMAT:
        return explanations[FOC_DIAG_REASON_UNSUPPORTED_FORMAT].code;
    case FOC_CONFIG_STORE_ERR_SEQUENCE:
        return explanations[NOTE_SEQUENCE_AMBIGUOUS].code;
    case FOC_CONFIG_STORE_ERR_IO:
        return explanations[FOC_DIAG_REASON_IO].code;
    default:
        if (report->recovery.active_slot != FOC_CONFIG_STORE_SLOT_NONE) {
            return inspector_slot_reason(&report->slots[report->recovery.active_slot]);
        }
        return explanations[NOTE_NO_VALID_RECORD].code;
    }
}

void inspector_print_diagnostics(
    const struct foc_config_diagnostic *report,
    const char *profile,
    bool json
)
{
    const char *slot = slot_name(report->recovery.active_slot);
    const char *reason = inspector_restore_reason(report);
    const struct inspector_explanation *note = find_explanation(reason);
    const enum foc_diagnostic_check available = recovery_availability(report);
    if (json) {
        json_prefix("inspect", profile);
        fputs(",\"recovery\":{\"selected_slot\":", stdout);
        json_string(slot);
        fputs(",\"available\":", stdout);
        json_check_boolean(available);
        printf(",\"status\":%d,\"reason\":", report->restore_status);
        json_string(reason);
        fputs(",\"explanation\":", stdout);
        json_string(note->message);
        fputs("},\"slots\":[", stdout);
        json_slot(&report->slots[0]);
        putchar(',');
        json_slot(&report->slots[1]);
        puts("]}");
        return;
    }

    printf(
        "SOURCE=%s profile=%s\n%s\n",
        source_names[report->source],
        profile != NULL ? profile : "integrity-only",
        STORAGE_BOUNDARY
    );
    printf(
        "Recovery: selected=%s available=%s reason=%s runtime_applied=UNKNOWN\n",
        slot != NULL ? slot : "NONE",
        unknown_text(check_name(available)),
        reason
    );
    if (report->restore_status != FOC_CONFIG_STORE_OK) {
        printf("  %s\n", note->message);
    }
    text_slot(&report->slots[0]);
    text_slot(&report->slots[1]);
}

void inspector_print_diff(
    const struct inspector_diff_source *left,
    const struct inspector_diff_source *right,
    const struct foc_config_diff_entry *fields,
    size_t count,
    const char *profile,
    bool json
)
{
    if (json) {
        json_prefix("diff", profile);
        fputs(",\"float32_comparison\":\"EXACT_IEEE_754_BITS\",\"left\":", stdout);
        json_diff_source(left);
        fputs(",\"right\":", stdout);
        json_diff_source(right);
        fputs(",\"fields\":[", stdout);
        for (size_t index = 0U; index < count; ++index) {
            if (index != 0U) {
                putchar(',');
            }
            json_diff_field(&fields[index]);
        }
        puts("]}");
        return;
    }

    printf("SOURCE=HOST_STORAGE_IMAGE profile=%s\n%s\n", profile, STORAGE_BOUNDARY);
    text_diff_source("Left", left);
    text_diff_source("Right", right);
    puts("Float32 comparison uses exact IEEE-754 bits.\nOther static parameters");
    for (size_t index = 0U; index < count; ++index) {
        if (!fields[index].motion_protection) {
            text_diff_field(&fields[index]);
        }
    }
    puts("Motion protection");
    for (size_t index = 0U; index < count; ++index) {
        if (fields[index].motion_protection) {
            text_diff_field(&fields[index]);
        }
    }
}

bool inspector_explain_reason(const char *code, bool json)
{
    const struct inspector_explanation *note = find_explanation(code);
    if (note == NULL) {
        return false;
    }
    if (json) {
        printf("{\"schema_version\":%u,\"command\":\"explain\",\"reason\":", JSON_SCHEMA_VERSION);
        json_string(note->code);
        fputs(",\"explanation\":", stdout);
        json_string(note->message);
        puts("}");
    } else {
        printf("%s\n%s\n", note->code, note->message);
    }
    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
