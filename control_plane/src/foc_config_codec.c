/**
 * @file    foc_config_codec.c
 * @brief   Encode and validate versioned FalconFOC configuration records.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Own the byte layout, eligible parameter IDs, identity checks and migration.
 * Delegate parameter types and candidate admission to the Control Plane.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_config_diagnostics.h"

#include "crc.h"

#include <float.h>
#include <limits.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

enum {
    FORMAT_OFFSET = 8U,
    SCHEMA_OFFSET = 10U,
    SEQUENCE_OFFSET = 12U,
    LENGTH_OFFSET = 16U,
    COUNT_OFFSET = 18U,
    POLE_PAIRS_OFFSET = 20U,
    FLAGS_OFFSET = 22U,
    CPR_OFFSET = 24U,
    DIRECTION_OFFSET = 28U,
    FRAME_OFFSET = 32U,
    ENCODER_OFFSET = 36U,
    ZERO_OFFSET = 40U,
    MOTOR_OFFSET = 44U,
    KT_OFFSET = 48U,
    CRC_OFFSET = 52U,
    TLV_BYTES = 8U,
    TLV_MAX = (FOC_CONFIG_STORE_SLOT_BYTES - FOC_CONFIG_STORE_HEADER_BYTES) / TLV_BYTES,
    IMPEDANCE_FLAG = 1U,
    BASE_FIELDS = 5U,
    FIELD_COUNT = 14U,
    TYPE_BOOL = 4U,
};

_Static_assert(
    CHAR_BIT == 8 && sizeof(float) == 4U && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
        FLT_MAX_EXP == 128,
    "configuration records require octets and IEEE binary32"
);

static const uint8_t cfg_magic[] = {'F', 'C', 'F', 'G'};

/* This is the schema allowlist. IDs, types and ranges are owned by Control Plane. */
static const enum foc_parameter_id fields[FIELD_COUNT] = {
    FOC_PARAMETER_MOTION_IQ_LIMIT,
    FOC_PARAMETER_TORQUE_LIMIT,
    FOC_PARAMETER_MAX_STIFFNESS,
    FOC_PARAMETER_MAX_DAMPING,
    FOC_PARAMETER_MAX_FEEDFORWARD,
    FOC_PARAMETER_LIMITS_ENABLED,
    FOC_PARAMETER_LIMIT_POSITION_MIN,
    FOC_PARAMETER_LIMIT_POSITION_MAX,
    FOC_PARAMETER_LIMIT_TOLERANCE,
    FOC_PARAMETER_LIMIT_MECHANICAL_FRAME,
    FOC_PARAMETER_FOLLOWING_ENABLED,
    FOC_PARAMETER_FOLLOWING_THRESHOLD,
    FOC_PARAMETER_FOLLOWING_DURATION_US,
    FOC_PARAMETER_FOLLOWING_MAX_GAP_US,
};

/* Private Helpers -------------------------------------------------------------- */

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
        ((uint32_t)p[3] << 24U);
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    write_u16(p, (uint16_t)value);
    write_u16(p + 2U, (uint16_t)(value >> 16U));
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool finite_bits(uint32_t bits)
{
    return (bits & UINT32_C(0x7F800000)) != UINT32_C(0x7F800000);
}

static uint32_t envelope_crc(const uint8_t *bytes, uint16_t payload)
{
    const uint32_t initial = crc32_ieee(CRC32_IEEE_INIT, bytes + 4U, CRC_OFFSET - 4U);
    return CRC32_IEEE_FINALIZE(crc32_ieee(initial, bytes + FOC_CONFIG_STORE_HEADER_BYTES, payload));
}

static int check_policy(const struct foc_config_store_policy *policy)
{
    if (policy == NULL || policy->pipeline == NULL || policy->base == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    const struct foc_config_store_identity *identity = &policy->identity;
    const struct foc_runtime_motion_config *motion = &policy->base->motion;
    if (identity->encoder_cpr < 2U || identity->encoder_cpr > 65536U ||
        (identity->encoder_direction != -1 && identity->encoder_direction != 1) ||
        identity->mechanical_frame_id == 0U || identity->encoder_identity == 0U ||
        identity->motor_setup_id == 0U || identity->pole_pairs == 0U) {
        return FOC_CONFIG_STORE_ERR_IDENTITY_UNKNOWN;
    }
    if (!motion->enabled || !finite_bits(float_bits(motion->rotor.electrical_offset_rad))) {
        return FOC_CONFIG_STORE_ERR_CONFIG;
    }
    if (motion->rotor.pole_pairs != identity->pole_pairs ||
        (motion->limits.mechanical_frame_id != 0U &&
         motion->limits.mechanical_frame_id != identity->mechanical_frame_id) ||
        (motion->impedance.enabled &&
         motion->impedance.mechanical_frame_id != identity->mechanical_frame_id)) {
        return FOC_CONFIG_STORE_ERR_IDENTITY;
    }
    return FOC_CONFIG_STORE_OK;
}

static bool config_matches(
    const struct foc_config_store_policy *policy,
    const struct foc_runtime_config *config
)
{
    const struct foc_runtime_motion_config *motion = &config->motion;
    const struct foc_runtime_motion_config *base = &policy->base->motion;
    const struct foc_config_store_identity *identity = &policy->identity;
    return motion->enabled && motion->rotor.pole_pairs == identity->pole_pairs &&
        float_bits(motion->rotor.electrical_offset_rad) ==
        float_bits(base->rotor.electrical_offset_rad) &&
        motion->limits.mechanical_frame_id == identity->mechanical_frame_id &&
        motion->impedance.enabled == base->impedance.enabled &&
        (!motion->impedance.enabled ||
         (motion->impedance.mechanical_frame_id == identity->mechanical_frame_id &&
          float_bits(motion->impedance.torque_constant_nm_per_a) ==
              float_bits(base->impedance.torque_constant_nm_per_a)));
}

static unsigned int field_index(uint16_t id)
{
    for (unsigned int index = 0U; index < FIELD_COUNT; ++index) {
        if ((uint16_t)fields[index] == id) {
            return index;
        }
    }
    return FIELD_COUNT;
}

static bool required(unsigned int field, uint16_t schema, bool impedance)
{
    if (field >= BASE_FIELDS) {
        return schema == FOC_CONFIG_STORE_SCHEMA_VERSION;
    }
    return field == 0U || impedance;
}

static int collect(
    const struct foc_runtime_config *config,
    unsigned int start,
    struct foc_parameter_edit *edits,
    size_t *count
)
{
    for (unsigned int index = start; index < FIELD_COUNT; ++index) {
        if (!required(index, FOC_CONFIG_STORE_SCHEMA_VERSION, config->motion.impedance.enabled)) {
            continue;
        }
        struct foc_parameter_value value;
        if (foc_parameter_read_config(config, fields[index], &value) != FOC_PARAMETER_OK) {
            return FOC_CONFIG_STORE_ERR_CONFIG;
        }
        edits[*count] = (struct foc_parameter_edit){fields[index], value};
        ++*count;
    }
    return FOC_CONFIG_STORE_OK;
}

static int prepare(
    const struct foc_config_store_policy *policy,
    const struct foc_parameter_edit *edits,
    size_t count,
    struct foc_parameter_candidate *candidate,
    struct foc_config_slot_diagnostic *detail
)
{
    for (size_t index = 0U; index < count; ++index) {
        if (edits[index].id == FOC_PARAMETER_LIMIT_MECHANICAL_FRAME &&
            edits[index].value.data.u32 != policy->identity.mechanical_frame_id) {
            return FOC_CONFIG_STORE_ERR_IDENTITY;
        }
    }
    struct foc_parameter_candidate trial;
    const int status =
        foc_parameter_prepare_initial(policy->pipeline, policy->base, edits, count, &trial);
    if (status != FOC_PARAMETER_VALIDATED) {
        if (detail != NULL && status == FOC_PARAMETER_ERR_RANGE) {
            detail->reason = FOC_DIAG_REASON_INVALID_PARAMETER;
        }
        return FOC_CONFIG_STORE_ERR_CONFIG;
    }
    /* The stored group must remain complete and internally valid when disabled. */
    struct foc_motion_limits_config bounds = trial.config.motion.limits;
    bounds.enabled = true;
    const uint32_t gap = trial.config.motion.following_error.max_gap_us;
    if (!foc_motion_limits_validate(&bounds) || gap < trial.config.h0.period_us ||
        gap < trial.config.h1.period_us) {
        return FOC_CONFIG_STORE_ERR_CONFIG;
    }
    *candidate = trial;
    return FOC_CONFIG_STORE_OK;
}

static int validated_edits(
    const struct foc_config_store_policy *policy,
    const struct foc_parameter_candidate *candidate,
    struct foc_parameter_edit *edits,
    size_t *count
)
{
    if (candidate == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    const int status = check_policy(policy);
    if (status != FOC_CONFIG_STORE_OK) {
        return status;
    }
    if (!config_matches(policy, &candidate->config)) {
        return FOC_CONFIG_STORE_ERR_IDENTITY;
    }
    const int collected = collect(&candidate->config, 0U, edits, count);
    if (collected != FOC_CONFIG_STORE_OK) {
        return collected;
    }
    struct foc_parameter_candidate validated;
    return prepare(policy, edits, *count, &validated, NULL);
}

/* Wire tags are fixed independently of the C enum representation. */
static uint8_t wire_type(enum foc_parameter_type type)
{
    switch (type) {
    case FOC_PARAMETER_U32:
        return 1U;
    case FOC_PARAMETER_I32:
        return 2U;
    case FOC_PARAMETER_F32:
        return 3U;
    default:
        return 0U;
    }
}

static uint32_t scalar_bits(const struct foc_parameter_value *value)
{
    switch (value->type) {
    case FOC_PARAMETER_F32:
        return float_bits(value->data.f32);
    case FOC_PARAMETER_I32:
        return (uint32_t)value->data.i32;
    default:
        return value->data.u32;
    }
}

static union foc_parameter_scalar from_bits(uint8_t type, uint32_t bits)
{
    union foc_parameter_scalar value;
    if (type == 3U) {
        memcpy(&value.f32, &bits, sizeof(bits));
    } else if (type == 2U) {
        value.i32 = bits <= INT32_MAX ? (int32_t)bits : -1 - (int32_t)(UINT32_MAX - bits);
    } else {
        value.u32 = bits;
    }
    return value;
}

static bool validate_wire_value(uint8_t type, uint32_t value)
{
    return (type != 3U || finite_bits(value)) && (type != TYPE_BOOL || value <= 1U);
}

static uint32_t
envelope_mismatches(const struct foc_config_store_policy *policy, const uint8_t *bytes)
{
    const struct foc_config_store_identity *identity = &policy->identity;
    const struct foc_runtime_motion_config *motion = &policy->base->motion;
    return (read_u16(bytes + POLE_PAIRS_OFFSET) == identity->pole_pairs ? 0U
                                                                        : FOC_DIAG_ID_POLE_PAIRS) |
        (read_u16(bytes + FLAGS_OFFSET) == (motion->impedance.enabled ? IMPEDANCE_FLAG : 0U)
             ? 0U
             : FOC_DIAG_ID_PROFILE) |
        (read_u32(bytes + CPR_OFFSET) == identity->encoder_cpr ? 0U : FOC_DIAG_ID_CPR) |
        (read_u32(bytes + DIRECTION_OFFSET) == (uint32_t)identity->encoder_direction
             ? 0U
             : FOC_DIAG_ID_DIRECTION) |
        (read_u32(bytes + FRAME_OFFSET) == identity->mechanical_frame_id ? 0U : FOC_DIAG_ID_FRAME) |
        (read_u32(bytes + ENCODER_OFFSET) == identity->encoder_identity ? 0U
                                                                        : FOC_DIAG_ID_ENCODER) |
        (read_u32(bytes + ZERO_OFFSET) == float_bits(motion->rotor.electrical_offset_rad)
             ? 0U
             : FOC_DIAG_ID_OFFSET) |
        (read_u32(bytes + MOTOR_OFFSET) == identity->motor_setup_id ? 0U : FOC_DIAG_ID_MOTOR) |
        (read_u32(bytes + KT_OFFSET) ==
                 (motion->impedance.enabled ? float_bits(motion->impedance.torque_constant_nm_per_a)
                                            : 0U)
             ? 0U
             : FOC_DIAG_ID_KT);
}

static int invalid_tlv(
    struct foc_config_slot_diagnostic *detail,
    enum foc_diagnostic_reason reason,
    int rejected
)
{
    if (detail != NULL) {
        detail->tlv = FOC_DIAG_FAIL;
        detail->tlv_reason = reason;
    }
    return rejected == FOC_CONFIG_STORE_OK ? FOC_CONFIG_STORE_ERR_RECORD : rejected;
}

static int decode_edits(
    const uint8_t *payload,
    const struct foc_config_store_record *record,
    bool impedance,
    bool known_schema,
    struct foc_parameter_edit *edits,
    size_t *count,
    struct foc_config_slot_diagnostic *detail
)
{
    bool seen[FIELD_COUNT] = {false};
    uint16_t ids[TLV_MAX];
    int rejected = FOC_CONFIG_STORE_OK;
    for (size_t index = 0U; index < record->count; ++index) {
        const uint8_t *tlv = payload + index * TLV_BYTES;
        const uint16_t id = read_u16(tlv);
        const uint8_t type = tlv[2];
        const uint32_t bits = read_u32(tlv + 4U);
        if (tlv[3] != 4U || type < 1U || type > TYPE_BOOL) {
            return invalid_tlv(detail, FOC_DIAG_REASON_BAD_TLV, rejected);
        }
        if (!validate_wire_value(type, bits)) {
            return invalid_tlv(detail, FOC_DIAG_REASON_INVALID_PARAMETER, rejected);
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            if (ids[previous] == id) {
                return invalid_tlv(detail, FOC_DIAG_REASON_DUPLICATE_PARAMETER, rejected);
            }
        }
        ids[index] = id;
        if (!known_schema || (id >= 0x8000U && id < 0xFFFFU)) {
            continue;
        }
        const unsigned int field = field_index(id);
        if (field == FIELD_COUNT || !required(field, record->schema, impedance)) {
            if (detail == NULL) {
                return FOC_CONFIG_STORE_ERR_SCHEMA;
            }
            rejected = FOC_CONFIG_STORE_ERR_SCHEMA;
            continue;
        }
        const struct foc_parameter_descriptor *descriptor = foc_parameter_find(fields[field]);
        if (descriptor == NULL || !descriptor->persistable || type != wire_type(descriptor->type)) {
            return invalid_tlv(detail, FOC_DIAG_REASON_BAD_TLV, rejected);
        }
        seen[field] = true;
        edits[*count] = (struct foc_parameter_edit){
            fields[field],
            {descriptor->type, from_bits(type, bits)},
        };
        ++*count;
    }
    if (detail != NULL) {
        detail->tlv = FOC_DIAG_PASS;
    }
    if (rejected != FOC_CONFIG_STORE_OK) {
        return rejected;
    }
    if (known_schema) {
        for (unsigned int field = 0U; field < FIELD_COUNT; ++field) {
            if (required(field, record->schema, impedance) && !seen[field]) {
                return FOC_CONFIG_STORE_ERR_INCOMPLETE;
            }
        }
    }
    return FOC_CONFIG_STORE_OK;
}

static void describe_header(struct foc_config_slot_diagnostic *detail, const uint8_t *bytes)
{
    const uint32_t marker = read_u32(bytes);
    detail->commit = marker == FOC_CONFIG_STORE_COMMIT_WORD
        ? FOC_DIAG_COMMIT_COMPLETE
        : (marker == UINT32_MAX ? FOC_DIAG_COMMIT_ERASED : FOC_DIAG_COMMIT_TORN);
    detail->header_present = memcmp(bytes + 4U, cfg_magic, sizeof(cfg_magic)) == 0;
    detail->magic = detail->header_present ? FOC_DIAG_PASS : FOC_DIAG_FAIL;
    if (!detail->header_present) {
        return;
    }
    detail->format = read_u16(bytes + FORMAT_OFFSET);
    detail->payload_length = read_u16(bytes + LENGTH_OFFSET);
    if (detail->format != FOC_CONFIG_STORE_FORMAT_VERSION) {
        return;
    }
    detail->record = (struct foc_config_store_record){
        .sequence = read_u32(bytes + SEQUENCE_OFFSET),
        .schema = read_u16(bytes + SCHEMA_OFFSET),
        .count = read_u16(bytes + COUNT_OFFSET),
        .size = FOC_CONFIG_STORE_HEADER_BYTES + detail->payload_length,
    };
    detail->recorded_identity = (struct foc_config_store_identity){
        .encoder_cpr = read_u32(bytes + CPR_OFFSET),
        .encoder_direction = from_bits(2U, read_u32(bytes + DIRECTION_OFFSET)).i32,
        .mechanical_frame_id = read_u32(bytes + FRAME_OFFSET),
        .encoder_identity = read_u32(bytes + ENCODER_OFFSET),
        .motor_setup_id = read_u32(bytes + MOTOR_OFFSET),
        .pole_pairs = read_u16(bytes + POLE_PAIRS_OFFSET),
    };
    detail->electrical_offset_bits = read_u32(bytes + ZERO_OFFSET);
    detail->kt_bits = read_u32(bytes + KT_OFFSET);
    detail->impedance_profile = (read_u16(bytes + FLAGS_OFFSET) & IMPEDANCE_FLAG) != 0U;
}

static int inspect_envelope(
    const uint8_t *bytes,
    size_t size,
    struct foc_config_store_record *record,
    struct foc_config_slot_diagnostic *detail
)
{
    if (bytes == NULL || record == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    if (size < FOC_CONFIG_STORE_HEADER_BYTES || size > FOC_CONFIG_STORE_SLOT_BYTES) {
        return FOC_CONFIG_STORE_ERR_RECORD;
    }
    const bool committed = read_u32(bytes) == FOC_CONFIG_STORE_COMMIT_WORD;
    const bool magic = memcmp(bytes + 4U, cfg_magic, sizeof(cfg_magic)) == 0;
    const uint16_t length = read_u16(bytes + LENGTH_OFFSET);
    const bool bounded = length <= size - FOC_CONFIG_STORE_HEADER_BYTES;
    bool crc_valid = false;
    if (magic && bounded && (committed || detail != NULL)) {
        crc_valid = read_u32(bytes + CRC_OFFSET) == envelope_crc(bytes, length);
    }
    if (detail != NULL) {
        describe_header(detail, bytes);
        if (magic && bounded) {
            detail->crc = crc_valid ? FOC_DIAG_PASS : FOC_DIAG_FAIL;
        }
    }
    if (!committed) {
        for (size_t index = 0U; index < size; ++index) {
            if (bytes[index] != 0xFFU) {
                return FOC_CONFIG_STORE_ERR_UNCOMMITTED;
            }
        }
        if (detail != NULL) {
            detail->magic = FOC_DIAG_UNKNOWN;
        }
        return FOC_CONFIG_STORE_EMPTY;
    }
    if (!magic || !bounded) {
        return FOC_CONFIG_STORE_ERR_RECORD;
    }
    if (!crc_valid) {
        return FOC_CONFIG_STORE_ERR_CRC;
    }
    if (read_u16(bytes + FORMAT_OFFSET) != FOC_CONFIG_STORE_FORMAT_VERSION) {
        return FOC_CONFIG_STORE_ERR_FORMAT;
    }
    const uint16_t count = read_u16(bytes + COUNT_OFFSET);
    if (count > TLV_MAX || length != count * TLV_BYTES) {
        return FOC_CONFIG_STORE_ERR_RECORD;
    }
    *record = (struct foc_config_store_record){
        .sequence = read_u32(bytes + SEQUENCE_OFFSET),
        .schema = read_u16(bytes + SCHEMA_OFFSET),
        .count = count,
        .size = FOC_CONFIG_STORE_HEADER_BYTES + length,
    };
    return FOC_CONFIG_STORE_OK;
}

static bool schema_supported(uint16_t schema)
{
    return schema == FOC_CONFIG_STORE_SCHEMA_VERSION || schema == FOC_CONFIG_STORE_SCHEMA_LEGACY;
}

static int decode_record(
    const struct foc_config_store_policy *policy,
    const uint8_t *bytes,
    const struct foc_config_store_record *record,
    struct foc_parameter_candidate *candidate,
    struct foc_config_slot_diagnostic *detail,
    int policy_status
)
{
    const bool supported = schema_supported(record->schema);
    const bool legacy = record->schema == FOC_CONFIG_STORE_SCHEMA_LEGACY;
    if (detail == NULL && !supported) {
        return FOC_CONFIG_STORE_ERR_SCHEMA;
    }
    const uint32_t mismatch =
        policy_status == FOC_CONFIG_STORE_OK ? envelope_mismatches(policy, bytes) : 0U;
    const int identity_status = mismatch != 0U ? FOC_CONFIG_STORE_ERR_IDENTITY : policy_status;
    if (detail == NULL && identity_status != FOC_CONFIG_STORE_OK) {
        return identity_status;
    }
    if (detail != NULL) {
        detail->schema = supported ? FOC_DIAG_PASS : FOC_DIAG_FAIL;
        detail->migration_needed = legacy;
        detail->identity_mismatches = mismatch;
        detail->identity = identity_status == FOC_CONFIG_STORE_OK
            ? FOC_DIAG_PASS
            : (identity_status == FOC_CONFIG_STORE_ERR_IDENTITY ? FOC_DIAG_FAIL : FOC_DIAG_UNKNOWN);
    }

    struct foc_parameter_edit edits[FIELD_COUNT];
    size_t count = 0U;
    const bool impedance = (read_u16(bytes + FLAGS_OFFSET) & IMPEDANCE_FLAG) != 0U;
    int status = decode_edits(
        bytes + FOC_CONFIG_STORE_HEADER_BYTES,
        record,
        impedance,
        supported,
        edits,
        &count,
        detail
    );
    if (policy != NULL && policy_status != FOC_CONFIG_STORE_OK) {
        return policy_status;
    }
    if (!supported) {
        return FOC_CONFIG_STORE_ERR_SCHEMA;
    }
    if (policy == NULL && status != FOC_CONFIG_STORE_OK) {
        return status;
    }
    if (identity_status != FOC_CONFIG_STORE_OK) {
        return identity_status;
    }
    if (status != FOC_CONFIG_STORE_OK) {
        return status;
    }
    if (legacy) {
        if (policy->confirmed_protection == NULL) {
            return FOC_CONFIG_STORE_ERR_RECONFIRM;
        }
        if (!config_matches(policy, &policy->confirmed_protection->config)) {
            return FOC_CONFIG_STORE_ERR_IDENTITY;
        }
        status = collect(&policy->confirmed_protection->config, BASE_FIELDS, edits, &count);
        if (status != FOC_CONFIG_STORE_OK) {
            return status;
        }
    }
    status = prepare(policy, edits, count, candidate, detail);
    if (detail != NULL && status == FOC_CONFIG_STORE_ERR_IDENTITY) {
        detail->identity_mismatches |= FOC_DIAG_ID_FRAME;
    }
    return status == FOC_CONFIG_STORE_OK && legacy ? FOC_CONFIG_STORE_MIGRATED : status;
}

static enum foc_diagnostic_reason diagnostic_reason(const struct foc_config_slot_diagnostic *slot)
{
    switch (slot->status) {
    case FOC_CONFIG_STORE_OK:
    case FOC_CONFIG_STORE_MIGRATED:
        return FOC_DIAG_REASON_NONE;
    case FOC_CONFIG_STORE_EMPTY:
        return FOC_DIAG_REASON_RECORD_NOT_FOUND;
    case FOC_CONFIG_STORE_ERR_UNCOMMITTED:
        return FOC_DIAG_REASON_INCOMPLETE_COMMIT;
    case FOC_CONFIG_STORE_ERR_CRC:
        return FOC_DIAG_REASON_BAD_CRC;
    case FOC_CONFIG_STORE_ERR_FORMAT:
        return FOC_DIAG_REASON_UNSUPPORTED_FORMAT;
    case FOC_CONFIG_STORE_ERR_SCHEMA:
        return FOC_DIAG_REASON_UNSUPPORTED_SCHEMA;
    case FOC_CONFIG_STORE_ERR_INCOMPLETE:
        return FOC_DIAG_REASON_MISSING_PARAMETER;
    case FOC_CONFIG_STORE_ERR_RECONFIRM:
        return FOC_DIAG_REASON_MIGRATION_REQUIRED;
    case FOC_CONFIG_STORE_ERR_IDENTITY:
        return FOC_DIAG_REASON_IDENTITY_MISMATCH;
    case FOC_CONFIG_STORE_ERR_IDENTITY_UNKNOWN:
        return FOC_DIAG_REASON_IDENTITY_UNKNOWN;
    case FOC_CONFIG_STORE_ERR_CONFIG:
        return slot->reason == FOC_DIAG_REASON_INVALID_PARAMETER
            ? slot->reason
            : FOC_DIAG_REASON_INVALID_CONFIGURATION;
    case FOC_CONFIG_STORE_ERR_RECORD:
        if (slot->tlv_reason != FOC_DIAG_REASON_NONE) {
            return slot->tlv_reason;
        }
        return slot->magic == FOC_DIAG_FAIL ? FOC_DIAG_REASON_BAD_MAGIC
                                            : FOC_DIAG_REASON_BAD_ENVELOPE;
    default:
        return FOC_DIAG_REASON_BAD_ENVELOPE;
    }
}

/* Public Functions ------------------------------------------------------------- */

int foc_config_store_validate(
    const struct foc_config_store_policy *policy,
    const struct foc_parameter_candidate *candidate
)
{
    struct foc_parameter_edit edits[FIELD_COUNT];
    size_t count = 0U;
    return validated_edits(policy, candidate, edits, &count);
}

int foc_config_store_encode(
    const struct foc_config_store_policy *policy,
    const struct foc_parameter_candidate *candidate,
    uint32_t sequence,
    uint8_t *bytes,
    size_t capacity,
    size_t *written
)
{
    if (bytes == NULL || written == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    struct foc_parameter_edit edits[FIELD_COUNT];
    size_t count = 0U;
    const int status = validated_edits(policy, candidate, edits, &count);
    if (status != FOC_CONFIG_STORE_OK) {
        return status;
    }
    const uint16_t length = (uint16_t)(count * TLV_BYTES);
    const size_t size = FOC_CONFIG_STORE_HEADER_BYTES + length;
    if (capacity < size) {
        return FOC_CONFIG_STORE_ERR_CAPACITY;
    }

    memset(bytes, 0xFF, FOC_CONFIG_STORE_PROGRAM_BYTES);
    memcpy(bytes + 4U, cfg_magic, sizeof(cfg_magic));
    write_u16(bytes + FORMAT_OFFSET, FOC_CONFIG_STORE_FORMAT_VERSION);
    write_u16(bytes + SCHEMA_OFFSET, FOC_CONFIG_STORE_SCHEMA_VERSION);
    write_u32(bytes + SEQUENCE_OFFSET, sequence);
    write_u16(bytes + LENGTH_OFFSET, length);
    write_u16(bytes + COUNT_OFFSET, (uint16_t)count);
    write_u16(bytes + POLE_PAIRS_OFFSET, policy->identity.pole_pairs);
    write_u16(bytes + FLAGS_OFFSET, policy->base->motion.impedance.enabled ? IMPEDANCE_FLAG : 0U);
    write_u32(bytes + CPR_OFFSET, policy->identity.encoder_cpr);
    write_u32(bytes + DIRECTION_OFFSET, (uint32_t)policy->identity.encoder_direction);
    write_u32(bytes + FRAME_OFFSET, policy->identity.mechanical_frame_id);
    write_u32(bytes + ENCODER_OFFSET, policy->identity.encoder_identity);
    write_u32(bytes + ZERO_OFFSET, float_bits(policy->base->motion.rotor.electrical_offset_rad));
    write_u32(bytes + MOTOR_OFFSET, policy->identity.motor_setup_id);
    write_u32(
        bytes + KT_OFFSET,
        policy->base->motion.impedance.enabled
            ? float_bits(policy->base->motion.impedance.torque_constant_nm_per_a)
            : 0U
    );
    for (size_t index = 0U; index < count; ++index) {
        uint8_t *tlv = bytes + FOC_CONFIG_STORE_HEADER_BYTES + index * TLV_BYTES;
        write_u16(tlv, (uint16_t)edits[index].id);
        tlv[2] = wire_type(edits[index].value.type);
        tlv[3] = 4U;
        write_u32(tlv + 4U, scalar_bits(&edits[index].value));
    }
    write_u32(bytes + CRC_OFFSET, envelope_crc(bytes, length));
    *written = size;
    return FOC_CONFIG_STORE_OK;
}

int foc_config_store_inspect(
    const uint8_t *bytes,
    size_t size,
    struct foc_config_store_record *record
)
{
    return inspect_envelope(bytes, size, record, NULL);
}

int foc_config_store_decode(
    const struct foc_config_store_policy *policy,
    const uint8_t *bytes,
    size_t size,
    struct foc_parameter_candidate *candidate
)
{
    if (candidate == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    const int policy_status = check_policy(policy);
    if (policy_status != FOC_CONFIG_STORE_OK) {
        return policy_status;
    }
    struct foc_config_store_record record;
    const int status = foc_config_store_inspect(bytes, size, &record);
    return status == FOC_CONFIG_STORE_OK
        ? decode_record(policy, bytes, &record, candidate, NULL, policy_status)
        : status;
}

int foc_config_inspect_slot(
    const uint8_t *bytes,
    size_t size,
    const struct foc_config_store_policy *policy,
    struct foc_config_slot_diagnostic *result
)
{
    if (bytes == NULL || result == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    *result = (struct foc_config_slot_diagnostic){.slot = FOC_CONFIG_STORE_SLOT_NONE};
    struct foc_config_store_record record;
    result->envelope_status = inspect_envelope(bytes, size, &record, result);
    result->status = result->envelope_status;
    if (result->envelope_status == FOC_CONFIG_STORE_OK) {
        const int policy_status =
            policy != NULL ? check_policy(policy) : FOC_CONFIG_STORE_ERR_IDENTITY_UNKNOWN;
        result->status =
            decode_record(policy, bytes, &record, &result->candidate, result, policy_status);
        result->applicable =
            result->status == FOC_CONFIG_STORE_OK || result->status == FOC_CONFIG_STORE_MIGRATED;
        result->integrity = result->tlv;
        if (result->status == FOC_CONFIG_STORE_ERR_SCHEMA) {
            result->schema = FOC_DIAG_FAIL;
        }
        if (result->status == FOC_CONFIG_STORE_ERR_IDENTITY) {
            result->identity = FOC_DIAG_FAIL;
        }
        if (result->applicable) {
            result->semantics = FOC_DIAG_PASS;
        } else if (
            result->status == FOC_CONFIG_STORE_ERR_CONFIG ||
            result->status == FOC_CONFIG_STORE_ERR_INCOMPLETE
        ) {
            result->semantics = FOC_DIAG_FAIL;
        }
    } else if (
        result->envelope_status != FOC_CONFIG_STORE_EMPTY &&
        result->envelope_status != FOC_CONFIG_STORE_ERR_FORMAT
    ) {
        result->integrity = FOC_DIAG_FAIL;
    }
    result->reason = diagnostic_reason(result);
    return result->status;
}

/* ---------------------------------------- end of file ----------------------------------------- */
