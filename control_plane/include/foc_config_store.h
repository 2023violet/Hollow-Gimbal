/**
 * @file    foc_config_store.h
 * @brief   Versioned configuration codec and two-slot persistence operations.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Store complete sets of eligible Control Plane values using a typed,
 * little-endian record. Select the latest committed envelope before applying
 * schema, identity, and parameter checks.
 *
 * Usage example
 * -------------
 * Prepare a detached candidate, save it in boot/offline context, then load
 * it into a separate candidate for a caller-owned Runtime initialization.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Caller owns immutable policy, storage, and scratch. Serialize operations
 * on each device in Foreground. Keep the policy and its inputs valid for
 * the entire call. Scratch must not alias the device, input, or output.
 *
 * Failure / recovery
 * ------------------
 * Record output parameters are assigned only after success unless designated
 * as diagnostics. After an uncertain save, reload storage before retrying.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_parameter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

#define FOC_CONFIG_STORE_SLOT_BYTES     256U
#define FOC_CONFIG_STORE_PROGRAM_BYTES  4U
#define FOC_CONFIG_STORE_HEADER_BYTES   56U
#define FOC_CONFIG_STORE_FORMAT_VERSION 1U
#define FOC_CONFIG_STORE_COMMIT_WORD    UINT32_C(0xC35A4C7E)
#define FOC_CONFIG_STORE_SCHEMA_LEGACY  1U
#define FOC_CONFIG_STORE_SCHEMA_VERSION 2U

enum foc_config_store_status {
    FOC_CONFIG_STORE_OK = 0,
    FOC_CONFIG_STORE_MIGRATED = 1,
    FOC_CONFIG_STORE_EMPTY = 2,
    FOC_CONFIG_STORE_ERR_INPUT = -1,
    FOC_CONFIG_STORE_ERR_CAPACITY = -2,
    FOC_CONFIG_STORE_ERR_RECORD = -3,
    FOC_CONFIG_STORE_ERR_UNCOMMITTED = -4,
    FOC_CONFIG_STORE_ERR_CRC = -5,
    FOC_CONFIG_STORE_ERR_FORMAT = -6,
    FOC_CONFIG_STORE_ERR_SCHEMA = -7,
    FOC_CONFIG_STORE_ERR_IDENTITY_UNKNOWN = -8,
    FOC_CONFIG_STORE_ERR_IDENTITY = -9,
    FOC_CONFIG_STORE_ERR_INCOMPLETE = -10,
    FOC_CONFIG_STORE_ERR_CONFIG = -11,
    FOC_CONFIG_STORE_ERR_RECONFIRM = -12,
    FOC_CONFIG_STORE_ERR_NO_VALID = -13,
    FOC_CONFIG_STORE_ERR_SEQUENCE = -14,
    FOC_CONFIG_STORE_ERR_IO = -15,
    FOC_CONFIG_STORE_ERR_UNCERTAIN = -16,
    FOC_CONFIG_STORE_ERR_GATE = -17,
    FOC_CONFIG_STORE_ERR_VERIFY = -18,
};

/** Qualified setup from the application or calibration owner. Zero IDs are unknown. */
struct foc_config_store_identity {
    uint32_t encoder_cpr;
    int32_t encoder_direction;
    uint32_t mechanical_frame_id;
    uint32_t encoder_identity;
    /** From the setup owner, such as the Motor-ID capture setup. */
    uint32_t motor_setup_id;
    uint16_t pole_pairs;
};

struct foc_config_store_policy {
    /** Trusted initialization inputs for this setup, independent of storage. */
    const struct foc_pipeline_config *pipeline;
    const struct foc_runtime_config *base;
    struct foc_config_store_identity identity;
    /** Optional, explicitly reconfirmed complete SOFT-02 group for schema 1 only. */
    const struct foc_parameter_candidate *confirmed_protection;
};

struct foc_config_store_record {
    uint32_t sequence;
    uint16_t schema;
    uint16_t count;
    size_t size;
};

enum foc_config_store_slot {
    FOC_CONFIG_STORE_SLOT_A = 0,
    FOC_CONFIG_STORE_SLOT_B = 1,
    FOC_CONFIG_STORE_SLOT_NONE = 2,
};

/** Optional diagnostics, set even when load or save rejects a record. */
struct foc_config_store_report {
    enum foc_config_store_slot active_slot;
    int slot_status[2];
    struct foc_config_store_record record;
    /** Raw Control Plane result for active saves, when its gate was called. */
    int parameter_status;
};

/**
 * A device exposes two disjoint erased-0xff slots, each exactly SLOT_BYTES.
 * Read success returns all requested bytes. Program uses aligned multiples
 * of PROGRAM_BYTES and may only clear bits. The final commit is its own
 * program unit. Callbacks return zero on success.
 */
struct foc_config_store_driver {
    void *context;
    int (*read)(void *context, unsigned int slot, size_t offset, uint8_t *bytes, size_t size);
    int (*erase)(void *context, unsigned int slot);
    int (*program)(void *context,
                   unsigned int slot,
                   size_t offset,
                   const uint8_t *bytes,
                   size_t size);
};

/** Caller-owned temporary memory. No initialization or retained state is required. */
struct foc_config_store_workspace {
    uint8_t slots[2][FOC_CONFIG_STORE_SLOT_BYTES];
};

/* Public Functions ------------------------------------------------------------- */

/** Validate all eligible values against the trusted Control Plane base. */
int foc_config_store_validate(const struct foc_config_store_policy *policy,
                              const struct foc_parameter_candidate *candidate);

/** Encode schema 2 with an erased commit unit. Only the storage transaction commits it. */
int foc_config_store_encode(const struct foc_config_store_policy *policy,
                            const struct foc_parameter_candidate *candidate,
                            uint32_t sequence,
                            uint8_t *bytes,
                            size_t capacity,
                            size_t *written);

/** Parse the envelope and verify CRC. A recognized envelope may contain an unknown schema. */
int foc_config_store_inspect(const uint8_t *bytes,
                             size_t size,
                             struct foc_config_store_record *record);

/** Decode a committed record into a detached candidate. Schema 1 requires reconfirmation. */
int foc_config_store_decode(const struct foc_config_store_policy *policy,
                            const uint8_t *bytes,
                            size_t size,
                            struct foc_parameter_candidate *candidate);

/** Read both slots and select an envelope with the normal recovery order. */
int foc_config_store_select(const struct foc_config_store_driver *reader,
                            struct foc_config_store_workspace *workspace,
                            struct foc_config_store_report *report);

/** Select and decode a committed candidate. Loading writes neither storage nor Runtime. */
int foc_config_store_load(const struct foc_config_store_driver *driver,
                          const struct foc_config_store_policy *policy,
                          struct foc_config_store_workspace *workspace,
                          struct foc_parameter_candidate *candidate,
                          struct foc_config_store_report *report);

/** Explicit save for boot/offline use when no Runtime operates this device. */
int foc_config_store_save(const struct foc_config_store_driver *driver,
                          const struct foc_config_store_policy *policy,
                          struct foc_config_store_workspace *workspace,
                          const struct foc_parameter_candidate *candidate,
                          struct foc_config_store_report *report);

/**
 * Save edits to a live device's detached candidate under its existing static
 * configuration gate. Serialize with lifecycle producers, including direct H0
 * actions. A pending lifecycle request prevents saving. Edits are relative to
 * the initialized Runtime, so repeat intended changes until reinitializing.
 * Saving does not change Runtime.
 */
int foc_config_store_save_active(const struct foc_config_store_driver *driver,
                                 const struct foc_pipeline_config *pipeline,
                                 const struct foc_config_store_identity *identity,
                                 struct foc_runtime *runtime,
                                 uint32_t session,
                                 const struct foc_parameter_edit *edits,
                                 size_t count,
                                 struct foc_config_store_workspace *workspace,
                                 struct foc_config_store_report *report);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
