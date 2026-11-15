/**
 * @file    foc_calibration.h
 * @brief   Portable Float32 current and rotor calibration candidates.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Design overview
 * ---------------
 * This module estimates current-sense zero offsets, identifies direction and
 * pole-pair candidates from explicit mechanical/electrical reference data, and
 * estimates electrical angle offset. It deliberately separates calculation,
 * validation, and application. It never writes Runtime, Board, Flash, PWM, or
 * Safety state.
 *
 * Usage example
 * -------------
 * Initialize a caller-owned estimator, collect source-identified samples only
 * while an external owner confirms the required physical precondition, then
 * finalize a candidate and check its identity metadata before any later owner
 * considers applying it. A result is not applied by this module.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Every estimator owns its bounded accumulators or fixed sample array. There
 * is no dynamic allocation or global mutable state. Instances are independent
 * and may be reset explicitly for a new calibration session.
 *
 * Failure / recovery
 * ------------------
 * Missing preconditions, invalid masks, sequence/time errors, changing input,
 * excessive noise, insufficient span, ambiguous candidates, and residual
 * failures produce explicit invalid results. A failed session stays invalid
 * until reset; no guessed parameter is marked usable.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Calibration lifecycle state. */
enum foc_calibration_state {
    /** No session has been initialized. */
    FOC_CALIBRATION_STATE_UNINITIALIZED = 0,
    /** Samples are being collected. */
    FOC_CALIBRATION_STATE_COLLECTING,
    /** A mathematically complete candidate awaits quality validation. */
    FOC_CALIBRATION_STATE_CANDIDATE,
    /** The candidate passed all configured quality checks. */
    FOC_CALIBRATION_STATE_VALID,
    /** The session failed and requires explicit reset. */
    FOC_CALIBRATION_STATE_INVALID,
};

/** Why a calibration session is not usable. */
enum foc_calibration_failure {
    /** No failure. */
    FOC_CALIBRATION_FAILURE_NONE = 0,
    /** Configuration values are invalid. */
    FOC_CALIBRATION_FAILURE_INVALID_CONFIG,
    /** Required external physical precondition was not asserted. */
    FOC_CALIBRATION_FAILURE_PRECONDITION,
    /** Too few samples were available. */
    FOC_CALIBRATION_FAILURE_SAMPLE_COUNT,
    /** A channel, sample, or source identity was invalid. */
    FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
    /** Sequence or timestamp order was invalid. */
    FOC_CALIBRATION_FAILURE_TIME_ORDER,
    /** Samples changed too quickly for a zero-current estimate. */
    FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
    /** Noise exceeded the configured bound. */
    FOC_CALIBRATION_FAILURE_NOISE,
    /** A result was outside the physical range. */
    FOC_CALIBRATION_FAILURE_RANGE,
    /** Reference data cannot distinguish the requested parameters. */
    FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE,
    /** Fitted residual exceeded the configured bound. */
    FOC_CALIBRATION_FAILURE_RESIDUAL,
    /** Result metadata does not match the current configuration. */
    FOC_CALIBRATION_FAILURE_CONFIG_MISMATCH,
    /** A calculated value was not finite. */
    FOC_CALIBRATION_FAILURE_NUMERIC,
};

/** Stable metadata used to prevent applying a result to another setup. */
struct foc_calibration_identity {
    /** ADC full-scale count used by the current-sense setup. */
    uint32_t adc_full_scale_counts;
    /** Current conversion gain in A/count. */
    float current_a_per_count;
    /** Absolute encoder counts per mechanical revolution. */
    uint32_t encoder_counts_per_revolution;
    /** Encoder direction convention, +1 or -1. */
    int32_t encoder_direction;
    /** Board phase-order value, kept numeric to avoid Device dependency. */
    uint32_t phase_order;
    /** Confirmed pole-pair value, or zero while identifying it. */
    uint16_t pole_pairs;
};

/** Three raw ADC phase values and their source identity. */
struct foc_calibration_adc_sample {
    /** Raw physical phase-A/B/C counts. */
    uint16_t phase_a;
    uint16_t phase_b;
    uint16_t phase_c;
    /** Required phase-valid bits: bit 0=A, bit 1=B, bit 2=C. */
    uint32_t valid_mask;
    /** Monotonic producer sequence, including zero. */
    uint32_t sequence;
    /** Source capture timestamp in microseconds. */
    uint32_t captured_us;
    /** External owner confirmation that phase current is zero. */
    bool zero_current_confirmed;
};

/** Policy for three-phase current-zero estimation. */
struct foc_calibration_adc_config {
    /** Metadata copied into the result. */
    struct foc_calibration_identity identity;
    /** Required number of phase-valid bits. */
    uint32_t required_valid_mask;
    /** Minimum samples required before finalization. */
    uint32_t minimum_samples;
    /** Maximum samples retained by the bounded session. */
    uint32_t maximum_samples;
    /** Maximum accepted adjacent raw-count change. */
    float maximum_sample_step_counts;
    /** Maximum permitted per-channel noise RMS in counts. */
    float maximum_noise_rms_counts;
    /** Inclusive physical offset range. */
    float minimum_offset_counts;
    float maximum_offset_counts;
    /** Maximum source interval between samples. */
    uint32_t maximum_interval_us;
};

/** Current-zero result with independent per-channel statistics. */
struct foc_calibration_adc_result {
    struct foc_calibration_identity identity;
    float offset_a_counts;
    float offset_b_counts;
    float offset_c_counts;
    float noise_rms_a_counts;
    float noise_rms_b_counts;
    float noise_rms_c_counts;
    uint32_t samples;
    enum foc_calibration_state state;
    enum foc_calibration_failure failure;
    bool usable;
};

/** Caller-owned bounded current-zero estimator. */
struct foc_calibration_adc_context {
    struct foc_calibration_adc_config config;
    struct foc_calibration_adc_result result;
    float mean[3];
    float m2[3];
    uint16_t previous[3];
    uint32_t samples;
    uint32_t previous_sequence;
    uint32_t previous_captured_us;
    bool previous_seen;
    bool initialized;
};

/** One paired mechanical/electrical reference observation. */
struct foc_calibration_angle_sample {
    /** Continuous mechanical position in rad. */
    float mechanical_rad;
    /** Trusted electrical reference angle in rad. */
    float electrical_rad;
    /** Monotonic source capture time. */
    uint32_t captured_us;
    /** Monotonic producer sequence. */
    uint32_t sequence;
    /** External validity of both references. */
    bool valid;
};

/** Policy shared by direction/pole-pair identification. */
struct foc_calibration_rotor_config {
    /** Metadata copied to the result. pole_pairs may be zero during search. */
    struct foc_calibration_identity identity;
    /** Inclusive candidate pole-pair range. */
    uint16_t minimum_pole_pairs;
    uint16_t maximum_pole_pairs;
    /** Minimum mechanical span needed to make candidates distinguishable. */
    float minimum_mechanical_span_rad;
    /** Maximum permitted circular RMS residual. */
    float maximum_residual_rad;
    /** Minimum separation between best and second candidate residual. */
    float minimum_candidate_separation_rad;
    /** Maximum source interval between reference samples. */
    uint32_t maximum_interval_us;
    /** Maximum bounded reference sample count. */
    uint32_t maximum_samples;
};

/** Direction/pole-pair candidate result. */
struct foc_calibration_rotor_result {
    struct foc_calibration_identity identity;
    int32_t direction;
    uint16_t pole_pairs;
    float electrical_offset_rad;
    float residual_rms_rad;
    float residual_max_rad;
    float candidate_separation_rad;
    float mechanical_span_rad;
    uint32_t samples;
    enum foc_calibration_state state;
    enum foc_calibration_failure failure;
    bool identifiable;
};

/** Caller-owned bounded rotor identification session. */
#define FOC_CALIBRATION_MAX_REFERENCE_SAMPLES 64U
struct foc_calibration_rotor_context {
    struct foc_calibration_rotor_config config;
    struct foc_calibration_angle_sample samples[FOC_CALIBRATION_MAX_REFERENCE_SAMPLES];
    struct foc_calibration_rotor_result result;
    uint32_t sample_count;
    bool initialized;
};

/** Policy for offset estimation after direction and pole pairs are known. */
struct foc_calibration_offset_config {
    /** Metadata copied to the result. direction and pole_pairs must be known. */
    struct foc_calibration_identity identity;
    /** Maximum circular RMS residual. */
    float maximum_residual_rad;
    /** Maximum source interval between reference samples. */
    uint32_t maximum_interval_us;
    /** Maximum bounded reference sample count. */
    uint32_t maximum_samples;
};

/** Electrical angle zero-offset result. */
struct foc_calibration_offset_result {
    struct foc_calibration_identity identity;
    float electrical_offset_rad;
    float residual_rms_rad;
    float residual_max_rad;
    uint32_t samples;
    enum foc_calibration_state state;
    enum foc_calibration_failure failure;
    bool usable;
};

/** Caller-owned bounded electrical-offset estimation session. */
struct foc_calibration_offset_context {
    struct foc_calibration_offset_config config;
    struct foc_calibration_angle_sample samples[FOC_CALIBRATION_MAX_REFERENCE_SAMPLES];
    struct foc_calibration_offset_result result;
    uint32_t sample_count;
    bool initialized;
};

/* Configuration admission ---------------------------------------------------- */

/** Failure category reported by the all-or-nothing configuration admission. */
enum foc_calibration_admission_failure {
    /** No failure. */
    FOC_CALIBRATION_ADMISSION_NONE = 0,
    /** A required admission pointer was NULL. */
    FOC_CALIBRATION_ADMISSION_NULL,
    /** The expected identity or admission limits are invalid. */
    FOC_CALIBRATION_ADMISSION_INVALID_CONFIG,
    /** The ADC candidate was not a usable, quality-validated result. */
    FOC_CALIBRATION_ADMISSION_ADC_QUALITY,
    /** The ADC candidate identity did not match the expected setup. */
    FOC_CALIBRATION_ADMISSION_ADC_IDENTITY,
    /** An ADC offset or noise value was outside the admission limits. */
    FOC_CALIBRATION_ADMISSION_ADC_RANGE,
    /** The rotor candidate was not identifiable or quality-validated. */
    FOC_CALIBRATION_ADMISSION_ROTOR_QUALITY,
    /** The rotor candidate identity did not match the expected setup. */
    FOC_CALIBRATION_ADMISSION_ROTOR_IDENTITY,
    /** The rotor direction or pole-pair result was not acceptable. */
    FOC_CALIBRATION_ADMISSION_ROTOR_MAPPING,
    /** The electrical-offset candidate was not usable or quality-validated. */
    FOC_CALIBRATION_ADMISSION_OFFSET_QUALITY,
    /** The electrical-offset identity did not match the rotor candidate. */
    FOC_CALIBRATION_ADMISSION_OFFSET_IDENTITY,
    /** The three candidates disagree on a required configuration field. */
    FOC_CALIBRATION_ADMISSION_CANDIDATE_MISMATCH,
    /** The current Core coordinate contract cannot represent the mapping. */
    FOC_CALIBRATION_ADMISSION_UNSUPPORTED_MAPPING,
    /** A final snapshot field was not finite or representable. */
    FOC_CALIBRATION_ADMISSION_NUMERIC,
};

/** Result code returned by the atomic configuration admission. */
enum foc_calibration_admission_status {
    /** The complete snapshot passed validation and was committed. */
    FOC_CALIBRATION_ADMISSION_OK = 0,
    /** A required pointer was NULL. */
    FOC_CALIBRATION_ADMISSION_ERR_NULL = -1,
    /** The expected setup or admission limits were invalid. */
    FOC_CALIBRATION_ADMISSION_ERR_CONFIG = -2,
    /** A candidate was incomplete, invalid, or failed its quality contract. */
    FOC_CALIBRATION_ADMISSION_ERR_CANDIDATE = -3,
    /** Candidate identities or coordinate mappings were inconsistent. */
    FOC_CALIBRATION_ADMISSION_ERR_IDENTITY = -4,
    /** A candidate field could not be represented as a finite snapshot value. */
    FOC_CALIBRATION_ADMISSION_ERR_NUMERIC = -5,
};

/** Quality and range limits applied again at the application boundary. */
struct foc_calibration_admission_limits {
    /** Minimum accepted ADC candidate sample count. */
    uint32_t minimum_adc_samples;
    /** Inclusive accepted ADC offset range in raw counts. */
    float minimum_offset_counts;
    float maximum_offset_counts;
    /** Maximum accepted per-channel ADC noise RMS in raw counts. */
    float maximum_noise_rms_counts;
    /** Maximum accepted rotor-fit circular RMS residual in rad. */
    float maximum_rotor_residual_rad;
    /** Maximum accepted electrical-offset circular RMS residual in rad. */
    float maximum_offset_residual_rad;
};

/** Read-only input bundle for one complete calibration admission attempt. */
struct foc_calibration_admission_input {
    /** Validated three-phase current-zero candidate. */
    const struct foc_calibration_adc_result *adc;
    /** Validated direction and pole-pair candidate. */
    const struct foc_calibration_rotor_result *rotor;
    /** Validated electrical angle zero-offset candidate. */
    const struct foc_calibration_offset_result *offset;
    /** Static identity expected by the target Board/Device/Sensor setup. */
    const struct foc_calibration_identity *expected_identity;
    /** Independent quality limits for this application boundary. */
    struct foc_calibration_admission_limits limits;
};

/** Immutable-by-convention configuration snapshot accepted before initialization. */
struct foc_calibration_snapshot {
    /** Static source identity used to produce all three candidates. */
    struct foc_calibration_identity source_identity;
    /** Accepted physical ADC zero offsets in raw counts. */
    float phase_a_offset_count;
    float phase_b_offset_count;
    float phase_c_offset_count;
    /** Direction fitted by CAL-01 before mapping into the Runtime sensor frame. */
    int32_t calibration_direction;
    /** Raw-code to Runtime mechanical direction after the coordinate conversion. */
    int32_t runtime_encoder_direction;
    /** Accepted positive electrical pole-pair count. */
    uint16_t pole_pairs;
    /** Accepted electrical angle at mechanical zero, normalized to [0, 2π). */
    float electrical_offset_rad;
    /** True only after an entire snapshot has been atomically committed. */
    bool accepted;
};

/* Public Functions ------------------------------------------------------------- */

/** Compare all identity fields relevant to safe result application. */
bool foc_calibration_identity_matches(const struct foc_calibration_identity *expected,
                                      const struct foc_calibration_identity *actual);

/**
 * Validate and atomically commit one complete calibration snapshot.
 *
 * The source direction in `expected_identity` describes the raw-code to
 * calibration-mechanical frame. The rotor candidate direction describes the
 * electrical sign relative to that frame. Existing FalconFOC Core maps the
 * Runtime mechanical frame with a positive pole-pair multiplier, so the
 * resolved sensor direction is their product. No candidate or caller-owned
 * running configuration is modified on failure.
 *
 * @param input Read-only candidates, expected identity, and quality limits.
 * @param snapshot Caller-owned destination, changed only after full success.
 * @param failure Optional detailed failure category.
 * @return FOC_CALIBRATION_ADMISSION_OK or a deterministic admission error.
 */
int foc_calibration_admit(const struct foc_calibration_admission_input *input,
                          struct foc_calibration_snapshot *snapshot,
                          enum foc_calibration_admission_failure *failure);

/** Validate a previously accepted snapshot before an explicit Board mapping. */
bool foc_calibration_snapshot_valid(const struct foc_calibration_snapshot *snapshot);

/* Current-zero estimation ----------------------------------------------------- */

int foc_calibration_adc_init(struct foc_calibration_adc_context *context,
                             const struct foc_calibration_adc_config *config);
int foc_calibration_adc_reset(struct foc_calibration_adc_context *context);
int foc_calibration_adc_collect(struct foc_calibration_adc_context *context,
                                const struct foc_calibration_adc_sample *sample);
int foc_calibration_adc_finalize(struct foc_calibration_adc_context *context,
                                 struct foc_calibration_adc_result *result);

/* Direction/pole-pair identification ------------------------------------------ */

int foc_calibration_rotor_init(struct foc_calibration_rotor_context *context,
                               const struct foc_calibration_rotor_config *config);
int foc_calibration_rotor_reset(struct foc_calibration_rotor_context *context);
int foc_calibration_rotor_collect(struct foc_calibration_rotor_context *context,
                                  const struct foc_calibration_angle_sample *sample);
int foc_calibration_rotor_finalize(struct foc_calibration_rotor_context *context,
                                   struct foc_calibration_rotor_result *result);

/* Electrical angle offset ----------------------------------------------------- */

int foc_calibration_offset_init(struct foc_calibration_offset_context *context,
                                const struct foc_calibration_offset_config *config);
int foc_calibration_offset_reset(struct foc_calibration_offset_context *context);
int foc_calibration_offset_collect(struct foc_calibration_offset_context *context,
                                   const struct foc_calibration_angle_sample *sample);
int foc_calibration_offset_finalize(struct foc_calibration_offset_context *context,
                                    struct foc_calibration_offset_result *result);

/* ---------------------------------------- end of file ----------------------------------------- */
