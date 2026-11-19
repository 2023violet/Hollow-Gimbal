/**
 * @file    foc_config_diagnostics.h
 * @brief   Read-only device configuration inspection, comparison and Runtime capture.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Observe integrity, applicability and effective Runtime values separately.
 * Pair inspection uses the same recovery selection as the Config Store.
 *
 * Usage example
 * -------------
 * Inspect two slots into caller-owned reports and compare applicable
 * candidates with a trusted default or a captured effective configuration.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Call from serialized Foreground; do not overlap storage writes or
 * Runtime initialization. The storage driver only needs its read callback.
 * Runtime capture copies the immutable config alongside one coherent snapshot.
 *
 * Failure / recovery
 * ------------------
 * An unexecuted check is UNKNOWN. A failed source exposes no candidate.
 * A failed capture or insufficient diff capacity leaves outputs unchanged.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

enum foc_diagnostic_check {
    FOC_DIAG_UNKNOWN = 0,
    FOC_DIAG_PASS,
    FOC_DIAG_FAIL,
};

enum foc_diagnostic_source {
    FOC_DIAG_SOURCE_HOST_STORAGE_IMAGE = 1,
    FOC_DIAG_SOURCE_HOST_RUNTIME,
    FOC_DIAG_SOURCE_DEFAULT_CONFIG,
    FOC_DIAG_SOURCE_PENDING_CONFIG,
};

enum foc_diagnostic_reason {
    FOC_DIAG_REASON_NONE = 0,
    FOC_DIAG_REASON_RECORD_NOT_FOUND,
    FOC_DIAG_REASON_BAD_MAGIC,
    FOC_DIAG_REASON_BAD_CRC,
    FOC_DIAG_REASON_INCOMPLETE_COMMIT,
    FOC_DIAG_REASON_UNSUPPORTED_FORMAT,
    FOC_DIAG_REASON_UNSUPPORTED_SCHEMA,
    FOC_DIAG_REASON_BAD_ENVELOPE,
    FOC_DIAG_REASON_BAD_TLV,
    FOC_DIAG_REASON_DUPLICATE_PARAMETER,
    FOC_DIAG_REASON_INVALID_PARAMETER,
    FOC_DIAG_REASON_INVALID_CONFIGURATION,
    FOC_DIAG_REASON_MISSING_PARAMETER,
    FOC_DIAG_REASON_IDENTITY_UNKNOWN,
    FOC_DIAG_REASON_IDENTITY_MISMATCH,
    FOC_DIAG_REASON_MIGRATION_REQUIRED,
    FOC_DIAG_REASON_IO,
};

enum foc_diagnostic_commit {
    FOC_DIAG_COMMIT_UNKNOWN = 0,
    FOC_DIAG_COMMIT_ERASED,
    FOC_DIAG_COMMIT_COMPLETE,
    FOC_DIAG_COMMIT_TORN,
};

enum foc_diagnostic_selection {
    FOC_DIAG_SELECTION_UNDECIDED = 0,
    FOC_DIAG_SELECTION_SELECTED,
    FOC_DIAG_SELECTION_OLDER,
    FOC_DIAG_SELECTION_EQUIVALENT,
    FOC_DIAG_SELECTION_INELIGIBLE,
};

enum foc_diagnostic_identity_field {
    FOC_DIAG_ID_POLE_PAIRS = 1U << 0,
    FOC_DIAG_ID_PROFILE = 1U << 1,
    FOC_DIAG_ID_CPR = 1U << 2,
    FOC_DIAG_ID_DIRECTION = 1U << 3,
    FOC_DIAG_ID_FRAME = 1U << 4,
    FOC_DIAG_ID_ENCODER = 1U << 5,
    FOC_DIAG_ID_OFFSET = 1U << 6,
    FOC_DIAG_ID_MOTOR = 1U << 7,
    FOC_DIAG_ID_KT = 1U << 8,
};

struct foc_config_slot_diagnostic {
    enum foc_config_store_slot slot;
    enum foc_diagnostic_selection selection;
    enum foc_diagnostic_commit commit;
    enum foc_diagnostic_check magic;
    enum foc_diagnostic_check crc;
    enum foc_diagnostic_check integrity;
    enum foc_diagnostic_check schema;
    enum foc_diagnostic_check tlv;
    enum foc_diagnostic_check semantics;
    enum foc_diagnostic_check identity;
    /** Format and length are observable when true. Other raw fields require Format 1. */
    bool header_present;
    uint16_t format;
    uint16_t payload_length;
    struct foc_config_store_record record;
    struct foc_config_store_identity recorded_identity;
    uint32_t identity_mismatches;
    uint32_t electrical_offset_bits;
    uint32_t kt_bits;
    bool impedance_profile;
    bool migration_needed;
    bool applicable;
    /** Envelope status is the existing Config Store integrity result. */
    int envelope_status;
    /** Full trusted decode result, or the first rejection that prevented it. */
    int status;
    enum foc_diagnostic_reason reason;
    /** Independent payload error when a format, schema or identity rejection takes precedence. */
    enum foc_diagnostic_reason tlv_reason;
    /** Valid only when applicable is true. */
    struct foc_parameter_candidate candidate;
};

struct foc_config_diagnostic {
    enum foc_diagnostic_source source;
    struct foc_config_store_report recovery;
    /** Same status as store load when a trusted policy is supplied. */
    int restore_status;
    struct foc_config_slot_diagnostic slots[2];
};

enum foc_config_diff_kind {
    FOC_DIAG_UNCHANGED = 0,
    FOC_DIAG_VALUE_CHANGED,
    FOC_DIAG_MISSING_LEFT,
    FOC_DIAG_MISSING_RIGHT,
    FOC_DIAG_TYPE_MISMATCH,
    FOC_DIAG_SCHEMA_INCOMPATIBLE,
    FOC_DIAG_IDENTITY_MISMATCH,
    FOC_DIAG_VALUE_UNAVAILABLE,
};

struct foc_config_diff_input {
    /** NULL for an untrusted, invalid or unavailable source. */
    const struct foc_runtime_config *config;
    /** If absent, a Config Store rejection can explain why. */
    int status;
};

struct foc_config_diff_entry {
    const struct foc_parameter_descriptor *parameter;
    struct foc_parameter_value left;
    struct foc_parameter_value right;
    bool left_present;
    bool right_present;
    bool motion_protection;
    enum foc_config_diff_kind kind;
};

struct foc_runtime_diagnostic {
    enum foc_diagnostic_source source;
    struct foc_runtime_snapshot snapshot;
    struct foc_runtime_config effective;
};

enum foc_config_application {
    FOC_DIAG_NOT_OBSERVED = 0,
    FOC_DIAG_REINITIALIZATION_REQUIRED,
    FOC_DIAG_MATCHES_EFFECTIVE,
    FOC_DIAG_CONFIG_UNAVAILABLE,
};

/* Public Functions ------------------------------------------------------------- */

/** Diagnose a single slot. A NULL policy allows integrity and structure checks only. */
int foc_config_inspect_slot(const uint8_t *bytes,
                            size_t size,
                            const struct foc_config_store_policy *policy,
                            struct foc_config_slot_diagnostic *result);

/** Read two slots and report the restore decision without writing to the device. */
int foc_config_inspect_pair(const struct foc_config_store_driver *reader,
                            const struct foc_config_store_policy *policy,
                            struct foc_config_store_workspace *workspace,
                            struct foc_config_diagnostic *result);

/** Number of statically described fields returned by a complete comparison. */
size_t foc_config_diff_count(void);

/** Compare static fields by stable ID and exact Float32 object bits. */
int foc_config_diff(const struct foc_config_diff_input *left,
                    const struct foc_config_diff_input *right,
                    struct foc_config_diff_entry *entries,
                    size_t capacity,
                    size_t *written);

/** Return the existing Runtime status and preserve the output if capture fails. */
int foc_runtime_diagnose(struct foc_runtime *runtime, struct foc_runtime_diagnostic *result);

/**
 * Compare a candidate's effective static fields with an optional Host observation.
 * Equality proves observed field values, not the provenance of initialization.
 */
enum foc_config_application foc_config_application_status(
    const struct foc_config_diff_input *candidate,
    const struct foc_runtime_diagnostic *runtime);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
