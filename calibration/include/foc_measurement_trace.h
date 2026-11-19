/**
 * @file    foc_measurement_trace.h
 * @brief   Validate normalized measurement records before calibration replay.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Design overview
 * ---------------
 * A capture contract binds one source, identity, channel and clock to an
 * ordered stream of bounded, typed records. No file parser is required.
 *
 * Usage example
 * -------------
 * Init a capture, convert records in file order, and finish the capture.
 * Submit successful conversions to a separate staging Workflow and publish
 * its candidate only after trace integrity and Workflow review both pass.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The replay caller owns the context and buffers and serializes all calls.
 * One context tracks one capture, with no retained record buffer.
 *
 * Failure / recovery
 * ------------------
 * Rejected records leave outputs untouched and invalidate capture integrity.
 * Reinitialize the context to begin a fresh capture.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_workflow.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros ---------------------------------------------------------------- */

#define FOC_MEASUREMENT_TRACE_VERSION     1U
#define FOC_MEASUREMENT_TRACE_KT_UNITS    6U
#define FOC_MEASUREMENT_TRACE_MAX_SAMPLES UINT32_C(1048576)

/* Public Types ----------------------------------------------------------------- */

enum foc_measurement_trace_source {
    FOC_MEASUREMENT_TRACE_HOST_SYNTHETIC = 1,
    FOC_MEASUREMENT_TRACE_HOST_VIRTUAL,
    FOC_MEASUREMENT_TRACE_EXTERNAL_RECORDED,
    FOC_MEASUREMENT_TRACE_BOARD_RECORDED,
};

/** Replay preserves the supplied provenance status. The adapter does not authenticate it. */
enum foc_measurement_trace_provenance {
    FOC_MEASUREMENT_TRACE_PROVENANCE_DECLARED = 1,
    FOC_MEASUREMENT_TRACE_PROVENANCE_VERIFIED,
    FOC_MEASUREMENT_TRACE_PROVENANCE_UNVERIFIED,
};

enum foc_measurement_trace_stage {
    FOC_MEASUREMENT_TRACE_ADC = FOC_CAL_WORKFLOW_ADC,
    FOC_MEASUREMENT_TRACE_ELECTRICAL = FOC_CAL_WORKFLOW_ELECTRICAL,
    FOC_MEASUREMENT_TRACE_ROTOR = FOC_CAL_WORKFLOW_ROTOR,
    FOC_MEASUREMENT_TRACE_OFFSET = FOC_CAL_WORKFLOW_OFFSET,
    FOC_MEASUREMENT_TRACE_LUT = FOC_CAL_WORKFLOW_LUT,
    FOC_MEASUREMENT_TRACE_MECHANICAL = FOC_CAL_WORKFLOW_MECHANICAL,
    FOC_MEASUREMENT_TRACE_KT = FOC_CAL_WORKFLOW_TUNING,
    FOC_MEASUREMENT_TRACE_LUT_HOLDOUT = FOC_CAL_WORKFLOW_STAGE_COUNT,
};

enum foc_measurement_trace_evidence {
    FOC_MEASUREMENT_TRACE_EVIDENCE_NONE = 0,
    FOC_MEASUREMENT_TRACE_ZERO_CONFIRMED,
    FOC_MEASUREMENT_TRACE_TERMINAL_MEASURED,
    FOC_MEASUREMENT_TRACE_TERMINAL_VIRTUAL,
    FOC_MEASUREMENT_TRACE_INDEPENDENT_ANGLE,
    FOC_MEASUREMENT_TRACE_OBSERVED_DQ,
    FOC_MEASUREMENT_TRACE_SHAFT_MEASURED,
    FOC_MEASUREMENT_TRACE_INDEPENDENT_KT,
    FOC_MEASUREMENT_TRACE_COMMAND,
    FOC_MEASUREMENT_TRACE_DERIVED,
};

enum foc_measurement_trace_flags {
    FOC_MEASUREMENT_TRACE_VALID = 1U << 0,
    FOC_MEASUREMENT_TRACE_OUTPUT_CONFIRMED = 1U << 1,
    FOC_MEASUREMENT_TRACE_SATURATED = 1U << 2,
    FOC_MEASUREMENT_TRACE_FRAME_CONFIRMED = 1U << 3,
    FOC_MEASUREMENT_TRACE_WINDOW_CONFIRMED = 1U << 4,
    FOC_MEASUREMENT_TRACE_LOAD_OBSERVED = 1U << 5,
    FOC_MEASUREMENT_TRACE_LOAD_NEGLIGIBLE = 1U << 6,
    FOC_MEASUREMENT_TRACE_SOURCE_PROXY = 1U << 7,
    FOC_MEASUREMENT_TRACE_LIMITED = 1U << 8,
};

enum foc_measurement_trace_status {
    FOC_MEASUREMENT_TRACE_OK = 0,
    FOC_MEASUREMENT_TRACE_ERR_INPUT = -1,
    FOC_MEASUREMENT_TRACE_ERR_STATE = -2,
    FOC_MEASUREMENT_TRACE_ERR_VERSION = -3,
    FOC_MEASUREMENT_TRACE_ERR_SOURCE = -4,
    FOC_MEASUREMENT_TRACE_ERR_IDENTITY = -5,
    FOC_MEASUREMENT_TRACE_ERR_TYPE = -6,
    FOC_MEASUREMENT_TRACE_ERR_UNITS = -7,
    FOC_MEASUREMENT_TRACE_ERR_CLOCK = -8,
    FOC_MEASUREMENT_TRACE_ERR_SEQUENCE = -9,
    FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER = -10,
    FOC_MEASUREMENT_TRACE_ERR_AGE = -11,
    FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT = -12,
    FOC_MEASUREMENT_TRACE_ERR_GAP = -13,
    FOC_MEASUREMENT_TRACE_ERR_RANGE = -14,
    FOC_MEASUREMENT_TRACE_ERR_INVALID = -15,
    FOC_MEASUREMENT_TRACE_ERR_SATURATION = -16,
    FOC_MEASUREMENT_TRACE_ERR_PRECONDITION_REQUIRED = -17,
    FOC_MEASUREMENT_TRACE_ERR_DATA_REQUIRED = -18,
    FOC_MEASUREMENT_TRACE_ERR_REFERENCE_REQUIRED = -19,
    FOC_MEASUREMENT_TRACE_ERR_TORQUE_SOURCE_REQUIRED = -20,
    FOC_MEASUREMENT_TRACE_ERR_LOAD_REQUIRED = -21,
    FOC_MEASUREMENT_TRACE_ERR_INCOMPLETE = -22,
    FOC_MEASUREMENT_TRACE_ERR_CANCELLED = -23,
};

struct foc_measurement_trace_contract {
    uint32_t version;
    enum foc_measurement_trace_source source;
    enum foc_measurement_trace_provenance provenance;
    enum foc_measurement_trace_stage stage;
    uint32_t session;
    uint32_t capture;
    /** Clock ID zero means unknown. V1 requires already comparable microsecond timestamps. */
    uint32_t clock_id;
    uint32_t channel;
    uint32_t first_sequence;
    uint32_t expected_samples;
    uint32_t max_age_us;
    uint32_t max_interval_us;
    /** Allowed Rotor, Offset and ADC–encoder skew. LUT and J/B require exact pairs. */
    uint32_t max_skew_us;
    /** Source clock may wrap at uint32_t. The independent sequence always wraps. */
    bool allow_wrap;
    /** Required for LUT and holdout. The reference must be a separate instrument. */
    uint32_t lut_reference_id;
    /** LUT target frame direction, which can differ from the raw direction. */
    int32_t lut_direction;
    struct foc_cal_workflow_identity identity;
};

union foc_measurement_trace_payload {
    struct foc_cal_workflow_sample sample;
    struct foc_cal_workflow_kt kt;
};

struct foc_measurement_trace_record {
    enum foc_measurement_trace_stage stage;
    enum foc_measurement_trace_evidence evidence;
    uint32_t clock_id;
    /** Required for Rotor, Offset, LUT and J/B. Optional for an R/L voltage endpoint. */
    uint32_t reference_clock_id;
    /** Workflow units for regular stages, FOC_MEASUREMENT_TRACE_KT_UNITS for Kt. */
    uint32_t units;
    uint32_t channel;
    uint32_t flags;
    uint32_t sequence;
    uint32_t source_us;
    /** Combined record completion on the source clock, nondecreasing when observed. */
    uint32_t completion_us;
    uint32_t trigger_us;
    uint32_t reference_us;
    /** Optional encoder aperture associated with an electrical current endpoint. */
    uint32_t frame_encoder_clock_id;
    uint32_t frame_encoder_us;
    bool completion_observed;
    bool trigger_observed;
    /**
     * Set typed identities, values, raw encoder code, ADC valid mask, and
     * electrical or mechanical interval boundaries. Source, units and native
     * observation flags are derived from this record. Use .kt for the Kt stage.
     */
    union foc_measurement_trace_payload payload;
};

struct foc_measurement_trace_report {
    uint32_t accepted;
    uint32_t rejected;
    uint32_t completion_unobserved;
    uint32_t trigger_observed;
    uint32_t source_proxy;
    uint32_t first_rejected_sequence;
    enum foc_measurement_trace_status first_failure;
    enum foc_measurement_trace_status last_status;
    bool complete;
};

struct foc_measurement_trace {
    struct foc_measurement_trace_contract contract;
    struct foc_measurement_trace_report report;
    uint32_t last_source_us;
    uint32_t last_reference_us;
    uint32_t last_completion_us;
    bool active;
};

/* Public Functions ------------------------------------------------------------- */

/** A valid init starts a new capture. Invalid contracts leave the context untouched. */
enum foc_measurement_trace_status foc_measurement_trace_init(
    struct foc_measurement_trace *context,
    const struct foc_measurement_trace_contract *contract);

/** Successful conversion writes one payload and replay time. Inputs and outputs must be disjoint. */
enum foc_measurement_trace_status foc_measurement_trace_convert(
    struct foc_measurement_trace *context,
    const struct foc_measurement_trace_record *record,
    union foc_measurement_trace_payload *payload_out,
    uint32_t *now_us_out);

/** Final integrity gate, independent of the estimator's sample count and quality gates. */
enum foc_measurement_trace_status foc_measurement_trace_finish(
    struct foc_measurement_trace *context);
enum foc_measurement_trace_status foc_measurement_trace_cancel(
    struct foc_measurement_trace *context);
enum foc_cal_workflow_source foc_measurement_trace_workflow_source(
    enum foc_measurement_trace_source source);
const char *foc_measurement_trace_status_name(enum foc_measurement_trace_status status);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
