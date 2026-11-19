/**
 * @file    foc_calibration_workflow.h
 * @brief   Foreground workflow over the existing FalconFOC calibration algorithms.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Bind one capture at a time to the workflow identity, dispatch the native
 * estimator, and retain its quality result. Full and selected profiles share
 * dependency checks and explicit review.
 *
 * Usage example
 * -------------
 * Init, start a profile, open and feed each selected capture, finish and review.
 * Encoder LUT additionally needs a separately identified holdout capture.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Caller owns and serializes the initially zeroed workflow in Foreground. Inputs are read
 * during the call. The bundle is borrowed until a new session or cancellation.
 *
 * Failure / recovery
 * ------------------
 * A failed stage cannot be retried with another capture in the same session.
 * An identity change revokes the entire candidate. A new session discards all
 * previous accumulation. Only a complete, reviewed result can be applied.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration.h"
#include "foc_encoder_calibration.h"
#include "foc_impedance_tuning.h"
#include "foc_mechanical_id.h"
#include "foc_motor_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

enum foc_cal_workflow_state {
    FOC_CAL_WORKFLOW_UNINITIALIZED = 0,
    FOC_CAL_WORKFLOW_IDLE,
    FOC_CAL_WORKFLOW_READY,
    FOC_CAL_WORKFLOW_WAITING_FOR_DATA,
    FOC_CAL_WORKFLOW_RUNNING,
    FOC_CAL_WORKFLOW_COMPLETED,
    FOC_CAL_WORKFLOW_REJECTED,
    FOC_CAL_WORKFLOW_CANCELLED,
};

enum foc_cal_workflow_stage {
    FOC_CAL_WORKFLOW_ADC = 0,
    FOC_CAL_WORKFLOW_ELECTRICAL,
    FOC_CAL_WORKFLOW_ROTOR,
    FOC_CAL_WORKFLOW_OFFSET,
    FOC_CAL_WORKFLOW_LUT,
    FOC_CAL_WORKFLOW_MECHANICAL,
    FOC_CAL_WORKFLOW_TUNING,
    FOC_CAL_WORKFLOW_STAGE_COUNT,
};

#define FOC_CAL_WORKFLOW_STAGE_BIT(stage) (UINT32_C(1) << (stage))
#define FOC_CAL_WORKFLOW_ALL \
    (FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_STAGE_COUNT) - UINT32_C(1))
#define FOC_CAL_WORKFLOW_ADMISSION_MASK                   \
    (FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ADC) |   \
     FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ROTOR) | \
     FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_OFFSET))

enum foc_cal_workflow_profile {
    FOC_CAL_WORKFLOW_FULL_PROFILE = 1,
    FOC_CAL_WORKFLOW_SELECTED_STAGES,
};

enum foc_cal_workflow_source {
    FOC_CAL_WORKFLOW_SOURCE_NONE = 0,
    FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
    FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT,
    FOC_CAL_WORKFLOW_EXTERNAL_RECORDED_DATA,
};

enum foc_cal_workflow_units {
    FOC_CAL_WORKFLOW_RAW_COUNTS = 1,
    FOC_CAL_WORKFLOW_ELECTRICAL_SI,
    /** Paired estimator radians; a selected LUT replaces the supplied mechanical angle. */
    FOC_CAL_WORKFLOW_RAD_PAIR,
    FOC_CAL_WORKFLOW_ENCODER_COUNTS_RAD,
    FOC_CAL_WORKFLOW_MECHANICAL_SI,
};

enum foc_cal_workflow_status {
    FOC_CAL_WORKFLOW_OK = 0,
    FOC_CAL_WORKFLOW_PARTIAL = 1,
    FOC_CAL_WORKFLOW_ERR_INPUT = -1,
    FOC_CAL_WORKFLOW_ERR_STATE = -2,
    FOC_CAL_WORKFLOW_ERR_IDENTITY = -3,
    FOC_CAL_WORKFLOW_ERR_ORDER = -4,
    FOC_CAL_WORKFLOW_ERR_SOURCE = -5,
    FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED = -6,
    FOC_CAL_WORKFLOW_ERR_DEPENDENCY = -7,
    FOC_CAL_WORKFLOW_ERR_QUALITY = -8,
    FOC_CAL_WORKFLOW_ERR_REVIEW = -9,
};

enum foc_cal_workflow_quality {
    FOC_CAL_WORKFLOW_NOT_SELECTED = 0,
    FOC_CAL_WORKFLOW_PENDING,
    FOC_CAL_WORKFLOW_COLLECTING,
    FOC_CAL_WORKFLOW_PASS,
    FOC_CAL_WORKFLOW_DATA_REQUIRED,
    FOC_CAL_WORKFLOW_INSUFFICIENT_DATA,
    FOC_CAL_WORKFLOW_UNIDENTIFIABLE,
    FOC_CAL_WORKFLOW_IDENTITY_MISMATCH,
    FOC_CAL_WORKFLOW_INVALID_OBSERVATION,
    FOC_CAL_WORKFLOW_REJECTED_BY_QUALITY,
    FOC_CAL_WORKFLOW_STAGE_CANCELLED,
};

struct foc_cal_workflow_identity {
    /** Original ADC and raw encoder acquisition setup; pole_pairs is an authorized expectation. */
    struct foc_calibration_identity acquisition;
    uint32_t motor_setup_id;
    uint32_t encoder_id;
    uint32_t mechanical_frame_id;
};

struct foc_cal_workflow_request {
    uint32_t session;
    enum foc_cal_workflow_profile profile;
    /** Required selection for SELECTED_STAGES; FULL_PROFILE always selects all stages. */
    uint32_t stages;
    struct foc_cal_workflow_identity identity;
    struct foc_calibration_admission_limits admission;
};

struct foc_cal_workflow_stage_request {
    enum foc_cal_workflow_stage stage;
    enum foc_cal_workflow_source source;
    /** Nonzero capture identity, unique within this session, including LUT holdout. */
    uint32_t capture;
    /** Max latency between physical capture and supplied now_us on the same clock. */
    uint32_t max_age_us;
    union {
        struct foc_calibration_adc_config adc;
        struct foc_motor_id_config electrical;
        struct foc_calibration_rotor_config rotor;
        /** Pole pairs and direction come from the previous qualified rotor stage. */
        struct foc_calibration_offset_config offset;
        struct foc_encoder_cal_config lut;
        struct foc_mechanical_id_config mechanical;
    } policy;
};

struct foc_cal_workflow_sample {
    uint32_t session;
    uint32_t capture;
    struct foc_cal_workflow_identity identity;
    enum foc_cal_workflow_source source;
    enum foc_cal_workflow_units units;
    bool valid;
    /** Confirmed acquisition output for driven R/L intervals and J/B endpoints. */
    bool output_confirmed;
    bool saturated;
    /** Required for Rotor/Offset if an encoder LUT is part of the requested profile. */
    bool encoder_raw_valid;
    uint32_t encoder_raw_code;
    union {
        struct foc_calibration_adc_sample adc;
        struct foc_motor_id_sample electrical;
        struct foc_calibration_angle_sample angle;
        struct foc_encoder_cal_sample lut;
        struct foc_mechanical_id_sample mechanical;
    } data;
};

struct foc_cal_workflow_kt {
    struct foc_cal_workflow_identity identity;
    uint32_t torque_model_id;
    float nm_per_a;
    bool qualified;
    enum foc_cal_workflow_source source;
    uint32_t capture;
    uint32_t captured_us;
};

struct foc_cal_workflow_capture {
    uint32_t id;
    uint32_t first_us;
    uint32_t last_us;
    uint32_t last_sequence;
    uint32_t samples;
};

struct foc_cal_workflow_stage_result {
    enum foc_cal_workflow_quality quality;
    enum foc_cal_workflow_source source;
    enum foc_calibration_failure failure;
    /** Workflow rejection and underlying algorithm status are kept separate. */
    int status;
    int algorithm_status;
    struct foc_cal_workflow_capture primary;
    struct foc_cal_workflow_capture holdout;
};

struct foc_cal_workflow_bundle {
    struct foc_cal_workflow_request request;
    enum foc_cal_workflow_state state;
    /** STAGE_COUNT when none is open. */
    enum foc_cal_workflow_stage active;
    uint32_t passed;
    uint32_t missing;
    struct foc_cal_workflow_stage_result stages[FOC_CAL_WORKFLOW_STAGE_COUNT];
    struct foc_calibration_adc_result adc;
    struct foc_motor_id_result electrical;
    struct foc_calibration_rotor_result rotor;
    struct foc_calibration_offset_result offset;
    struct foc_encoder_cal_result lut;
    struct foc_mechanical_id_result mechanical;
    struct foc_impedance_tuning_candidate tuning;
    struct foc_cal_workflow_kt kt;
    struct foc_calibration_snapshot snapshot;
    enum foc_calibration_admission_failure admission_failure;
    /** All requested stages pass. */
    bool complete;
    /** Complete with an admitted ADC/rotor/offset snapshot. */
    bool applicable;
    /** An explicit review accepted the same session and setup. */
    bool reviewed;
};

struct foc_cal_workflow {
    struct foc_cal_workflow_bundle result;
    uint32_t max_age_us;
    bool holdout;
    struct foc_encoder_config lut_encoder;
    /** Native correction and continuity for paired angles on the frozen LUT. */
    struct foc_encoder_context angle_encoder;
    /** Only one estimator can consume observations at a time. */
    union {
        struct foc_calibration_adc_context adc;
        struct foc_motor_id_context electrical;
        struct foc_calibration_rotor_context rotor;
        struct foc_calibration_offset_context offset;
        struct foc_encoder_cal_context lut;
        struct foc_mechanical_id_context mechanical;
    } scratch;
};

/* Public Functions ------------------------------------------------------------- */

bool foc_cal_workflow_identity_matches(const struct foc_cal_workflow_identity *left,
                                       const struct foc_cal_workflow_identity *right);

int foc_cal_workflow_init(struct foc_cal_workflow *workflow);
int foc_cal_workflow_start(struct foc_cal_workflow *workflow,
                           const struct foc_cal_workflow_request *request);
int foc_cal_workflow_open(struct foc_cal_workflow *workflow,
                          const struct foc_cal_workflow_stage_request *request);
int foc_cal_workflow_push(struct foc_cal_workflow *workflow,
                          const struct foc_cal_workflow_sample *sample,
                          uint32_t now_us);
int foc_cal_workflow_begin_holdout(struct foc_cal_workflow *workflow, uint32_t capture);
int foc_cal_workflow_finish_stage(struct foc_cal_workflow *workflow);

/** Mark a selected unopened or active capture as unavailable. */
int foc_cal_workflow_data_required(struct foc_cal_workflow *workflow,
                                   enum foc_cal_workflow_stage stage);

/**
 * Use the admitted J/B result and qualified Kt for the selected tuning stage.
 * Model identity, J, B, Kt and damping source come from the workflow.
 * Other dynamics, timing and gain ceilings are supplied by the caller.
 */
int foc_cal_workflow_tune(struct foc_cal_workflow *workflow,
                          const struct foc_cal_workflow_kt *kt,
                          const struct foc_impedance_tuning_input *request);

int foc_cal_workflow_finish(struct foc_cal_workflow *workflow);
int foc_cal_workflow_review(struct foc_cal_workflow *workflow,
                            uint32_t session,
                            const struct foc_cal_workflow_identity *identity,
                            bool accepted);
int foc_cal_workflow_cancel(struct foc_cal_workflow *workflow);
const struct foc_cal_workflow_bundle *foc_cal_workflow_result(
    const struct foc_cal_workflow *workflow);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
