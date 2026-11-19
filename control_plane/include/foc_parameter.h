/**
 * @file foc_parameter.h
 * @brief C11 Foreground parameter discovery, detached configuration and Runtime observation.
 * @version 0.1
 * @date 2026-11-18
 *
 * Design overview
 * ---------------
 * The catalog describes immutable configuration, typed Runtime observations,
 * whole-command publication and existing lifecycle actions.
 *
 * Usage example
 * -------------
 * Enumerate foc_parameter_at(), then read a typed value or stage an edit
 * into detached storage. Use foc_runtime_init() to admit an initial candidate.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Runtime owns the live configuration and control state. Calls with an
 * instance inherit its H0 > H1 > Foreground and non-reentrant Foreground rules.
 *
 * Failure / recovery
 * ------------------
 * Failed edits and publications preserve caller outputs. Observe Runtime
 * inhibition and the session boundary before issuing any new command.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

#include <stddef.h>

/* Public Types ----------------------------------------------------------------- */

/** Stable C API IDs. The UART V1 parameter namespace is independent. */
enum foc_parameter_id {
    FOC_PARAMETER_POLE_PAIRS = 0x1001,
    FOC_PARAMETER_ELECTRICAL_OFFSET = 0x1002,
    FOC_PARAMETER_KT = 0x1003,
    FOC_PARAMETER_IMPEDANCE_FRAME = 0x1004,
    FOC_PARAMETER_MOTION_IQ_LIMIT = 0x1005,
    FOC_PARAMETER_TORQUE_LIMIT = 0x1006,
    FOC_PARAMETER_MAX_STIFFNESS = 0x1007,
    FOC_PARAMETER_MAX_DAMPING = 0x1008,
    FOC_PARAMETER_MAX_FEEDFORWARD = 0x1009,
    FOC_PARAMETER_POSITION_SPEED_LIMIT = 0x100a,
    FOC_PARAMETER_LIMITS_ENABLED = 0x100b,
    FOC_PARAMETER_LIMIT_POSITION_MIN = 0x100c,
    FOC_PARAMETER_LIMIT_POSITION_MAX = 0x100d,
    FOC_PARAMETER_LIMIT_TOLERANCE = 0x100e,
    FOC_PARAMETER_LIMIT_MECHANICAL_FRAME = 0x100f,
    FOC_PARAMETER_FOLLOWING_ENABLED = 0x1010,
    FOC_PARAMETER_FOLLOWING_THRESHOLD = 0x1011,
    FOC_PARAMETER_FOLLOWING_DURATION_US = 0x1012,
    FOC_PARAMETER_FOLLOWING_MAX_GAP_US = 0x1013,

    FOC_PARAMETER_TARGET_POSITION = 0x2001,
    FOC_PARAMETER_TARGET_VELOCITY = 0x2002,
    FOC_PARAMETER_STIFFNESS = 0x2003,
    FOC_PARAMETER_DAMPING = 0x2004,
    FOC_PARAMETER_FEEDFORWARD = 0x2005,

    FOC_PARAMETER_MODE = 0x3001,
    FOC_PARAMETER_STATE = 0x3002,
    FOC_PARAMETER_FAULTS = 0x3003,
    FOC_PARAMETER_SESSION = 0x3004,
    FOC_PARAMETER_COMMAND_SEQUENCE = 0x3005,
    FOC_PARAMETER_COMMAND_US = 0x3006,
    FOC_PARAMETER_SENSOR_US = 0x3007,
    FOC_PARAMETER_OUTPUT_ACTION = 0x3008,
    FOC_PARAMETER_OUTPUT_CONFIRMED = 0x3009,
    FOC_PARAMETER_UPDATES = 0x300a,
    FOC_PARAMETER_INHIBIT_PENDING = 0x300b,
    FOC_PARAMETER_SENSOR_VALID = 0x300c,
    FOC_PARAMETER_MECHANICAL_POSITION = 0x300d,
    FOC_PARAMETER_MECHANICAL_VELOCITY = 0x300e,
    FOC_PARAMETER_PIPELINE_STATUS = 0x300f,
    FOC_PARAMETER_FOLLOWING_STATE = 0x3010,
    FOC_PARAMETER_MOTION_REJECTION = 0x3011,

    FOC_PARAMETER_STOP = 0x4001,
    FOC_PARAMETER_CLEAR_TIMEOUT = 0x4002,
    FOC_PARAMETER_SET_MODE = 0x4003,
    FOC_PARAMETER_ARM = 0x4004,
    FOC_PARAMETER_START = 0x4005,
};

enum foc_parameter_type {
    FOC_PARAMETER_NONE = 0,
    FOC_PARAMETER_U32,
    FOC_PARAMETER_I32,
    FOC_PARAMETER_F32,
    /** Uses the u32 member. The descriptor specifies the owning enum. */
    FOC_PARAMETER_ENUM,
};

enum foc_parameter_category {
    FOC_PARAMETER_CONFIG = 0,
    FOC_PARAMETER_COMMAND,
    FOC_PARAMETER_TELEMETRY,
    FOC_PARAMETER_STATUS,
    FOC_PARAMETER_ACTION,
};

enum foc_parameter_access {
    FOC_PARAMETER_READ_ONLY = 0,
    FOC_PARAMETER_INITIAL_CONFIG,
    FOC_PARAMETER_COMMAND_ONLY,
    FOC_PARAMETER_ACTION_ONLY,
};

enum foc_parameter_status {
    FOC_PARAMETER_OK = 0,
    /** Detached candidate passed this module's validation. No live application. */
    FOC_PARAMETER_VALIDATED = 1,
    /** Runtime accepted publication. H0 application has not been witnessed. */
    FOC_PARAMETER_ACCEPTED = 2,
    /** Matching confirmed software UPDATE observed in a current H0 snapshot. */
    FOC_PARAMETER_APPLIED = 3,
    FOC_PARAMETER_ERR_INPUT = -1,
    FOC_PARAMETER_ERR_ID = -2,
    FOC_PARAMETER_ERR_TYPE = -3,
    FOC_PARAMETER_ERR_RANGE = -4,
    FOC_PARAMETER_ERR_READ_ONLY = -5,
    FOC_PARAMETER_ERR_STATE = -6,
    FOC_PARAMETER_ERR_SESSION = -7,
    FOC_PARAMETER_ERR_ORDER = -8,
    FOC_PARAMETER_ERR_BUSY = -9,
    FOC_PARAMETER_ERR_INHIBIT = -10,
    FOC_PARAMETER_ERR_FAULT = -11,
    FOC_PARAMETER_ERR_SENSOR = -12,
    FOC_PARAMETER_ERR_UNAVAILABLE = -13,
    FOC_PARAMETER_ERR_CONFIG = -14,
    FOC_PARAMETER_ERR_DUPLICATE = -15,
    FOC_PARAMETER_ERR_FRAME = -16,
    FOC_PARAMETER_ERR_MOTION_LIMIT = -17,
    FOC_PARAMETER_ERR_MOTION_MODE = -18,
};

union foc_parameter_scalar {
    uint32_t u32;
    int32_t i32;
    float f32;
};

struct foc_parameter_value {
    enum foc_parameter_type type;
    union foc_parameter_scalar data;
};

struct foc_parameter_range {
    union foc_parameter_scalar minimum;
    union foc_parameter_scalar maximum;
    bool minimum_exclusive;
};

struct foc_parameter_descriptor {
    enum foc_parameter_id id;
    const char *name;
    enum foc_parameter_type type;
    const char *unit;
    enum foc_parameter_category category;
    enum foc_parameter_access access;
    struct foc_parameter_range range;
    /** True when the range must be resolved against the active instance. */
    bool instance_range;
    bool requires_reinit;
    /** Eligible for the optional typed configuration store, subject to its schema and policy. */
    bool persistable;
    const char *description;
};

struct foc_parameter_edit {
    enum foc_parameter_id id;
    struct foc_parameter_value value;
};

/** Caller-owned detached result for use with an application-owned initial configuration. */
struct foc_parameter_candidate {
    struct foc_runtime_config config;
};

/** One Foreground publication. Valid only within the lifetime of its Runtime instance. */
struct foc_parameter_receipt {
    const struct foc_runtime *owner;
    uint32_t session;
    uint32_t sequence;
    uint32_t produced_us;
};

/*
 * Instance calls inherit Runtime's single-core H0 > H1 > Foreground ownership
 * and serialized, non-reentrant Foreground contract. Configuration remains
 * immutable throughout the bound Runtime lifetime.
 */

/* Public Functions ------------------------------------------------------------- */

/** The immutable catalog includes five action descriptors. */
size_t foc_parameter_count(void);
const struct foc_parameter_descriptor *foc_parameter_at(size_t index);
const struct foc_parameter_descriptor *foc_parameter_find(enum foc_parameter_id id);

/** Validate uniqueness of IDs and names in a fixed-size descriptor set. */
int foc_parameter_check_descriptors(const struct foc_parameter_descriptor *descriptors,
                                    size_t count);

/** Resolve nominal or instance-dependent bounds. The Runtime is required for instance bounds. */
int foc_parameter_get_range(struct foc_runtime *runtime,
                            enum foc_parameter_id id,
                            struct foc_parameter_range *range);

/** Read an effective configuration or one coherent Runtime snapshot field. */
int foc_parameter_read(struct foc_runtime *runtime,
                       enum foc_parameter_id id,
                       struct foc_parameter_value *value);

/** Read a static parameter from a detached Runtime configuration. */
int foc_parameter_read_config(const struct foc_runtime_config *config,
                              enum foc_parameter_id id,
                              struct foc_parameter_value *value);

/** Validate the exact scalar type and range. Runtime is required for instance-dependent ranges. */
int foc_parameter_validate(struct foc_runtime *runtime,
                           enum foc_parameter_id id,
                           const struct foc_parameter_value *value);

/** Check current write eligibility. A static result permits only detached preparation. */
int foc_parameter_write_access(struct foc_runtime *runtime,
                               enum foc_parameter_id id,
                               uint32_t session);

/**
 * Validate a batch of supported initial config edits and their relationships.
 * Apply the result through normal foc_runtime_init() with the same Pipeline
 * config on fresh caller-owned storage. Runtime retains final config admission.
 * Candidate storage must be detached. A failure leaves the output unchanged.
 */
int foc_parameter_prepare_initial(const struct foc_pipeline_config *pipeline,
                                  const struct foc_runtime_config *base,
                                  const struct foc_parameter_edit *edits,
                                  size_t count,
                                  struct foc_parameter_candidate *candidate);

/**
 * Prepare a detached candidate from the live configuration, only with a
 * coherent, confirmed, fault-free DISABLED view in the specified session.
 * This live-session path can lower the effective motion current cap.
 * For a fresh initial instance, prepare_initial() uses the qualified Pipeline
 * config to validate the outer cap against the current controller's limit.
 * No live state is changed. Store into a fresh caller-owned candidate.
 */
int foc_parameter_stage(struct foc_runtime *runtime,
                        uint32_t session,
                        const struct foc_parameter_edit *edits,
                        size_t count,
                        struct foc_parameter_candidate *candidate);

/** Publish the whole existing IMPEDANCE command, preserving its producer identity. */
int foc_parameter_publish_impedance(struct foc_runtime *runtime,
                                    const struct foc_runtime_command *command,
                                    uint32_t now_us,
                                    struct foc_parameter_receipt *receipt);

/**
 * Observe the exact command in the current coherent output snapshot.
 * This polling result is not a historical acknowledgement journal; a later
 * transition may hide an earlier UPDATE. Discard receipts at instance retirement.
 */
int foc_parameter_command_status(struct foc_runtime *runtime,
                                 const struct foc_parameter_receipt *receipt);

/**
 * Submit existing Runtime actions from Foreground. STOP calls
 * foc_runtime_revoke_command() without a snapshot or session check.
 * Independent event/ISR ingress uses foc_runtime_request_inhibit() directly.
 * For SET_MODE/ARM/START pass the Runtime lifecycle request and read its result
 * through foc_runtime_read_lifecycle_result(). CLEAR_TIMEOUT uses only the
 * existing restricted command-timeout recovery and snapshot clear_result.
 */
int foc_parameter_request_action(struct foc_runtime *runtime,
                                 enum foc_parameter_id id,
                                 const struct foc_runtime_lifecycle_request *request,
                                 uint32_t now_us);

/* ---------------------------------------- end of file ----------------------------------------- */
