/**
 * @file    foc_bootstrap.h
 * @brief   Cold-path bootstrap qualifications and coarse electrical reference.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * The acquisition owner provides synchronous encoder, open-circuit phase
 * voltage, and independently signed shaft speed readings. Observation
 * identity includes the verified phase order and mechanical coordinate frame.
 *
 * Usage example
 * -------------
 * Call qualify after verifying the measurement chain and safety state. A
 * reviewed reference can proceed directly. Otherwise, acquire four alternating
 * sweeps and call fit. A coarse reference requires an independent final
 * capture, existing calibration quality checks, and review before application.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Run in Foreground. Inputs remain caller-owned and immutable for each call.
 *
 * Failure / recovery
 * ------------------
 * An unsuccessful fit clears the mapping and records its rejection status.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_workflow.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros ---------------------------------------------------------------- */

#define FOC_BOOTSTRAP_SWEEPS         4U
#define FOC_BOOTSTRAP_MAX_POLE_PAIRS 14U

/* Public Types ---------------------------------------------------------------- */

enum foc_bootstrap_status {
    FOC_BOOTSTRAP_MEASUREMENT_INVALID = 0,
    FOC_BOOTSTRAP_READY_WITH_TRUSTED_REFERENCE,
    FOC_BOOTSTRAP_REFERENCE_ACQUISITION_REQUIRED,
    FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION,
    FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE,
    FOC_BOOTSTRAP_IDENTITY_MISMATCH,
    FOC_BOOTSTRAP_SAFETY_BLOCKED,
    FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED,
};

enum foc_bootstrap_quality {
    FOC_BOOTSTRAP_NO_REFERENCE = 0,
    FOC_BOOTSTRAP_COARSE_ELECTRICAL_REFERENCE,
};

struct foc_bootstrap_prerequisites {
    bool current_mapping_and_zero_verified;
    bool phase_and_direction_verified;
    bool encoder_and_clock_verified;
    bool bus_and_limits_verified;
    bool acquisition_verified;
    bool safety_permitted;
};

struct foc_bootstrap_trusted_reference {
    /** Mechanical frame identifies the reviewed angle mapping including any LUT. */
    struct foc_cal_workflow_identity identity;
    uint16_t pole_pairs;
    int32_t encoder_direction;
    float electrical_offset_rad;
    bool reviewed;
};

struct foc_bootstrap_config {
    struct foc_cal_workflow_identity identity;
    uint32_t capture;
    enum foc_cal_workflow_source source;
    uint16_t minimum_pole_pairs;
    uint16_t maximum_pole_pairs;
    /** Optional independently trusted constraints. Zero means unknown. */
    uint16_t expected_pole_pairs;
    int32_t expected_encoder_direction;
    uint32_t minimum_samples_per_sweep;
    uint32_t maximum_interval_us;
    float minimum_mechanical_span_rad;
    float maximum_residual_rad;
    float minimum_candidate_separation_rad;
    float maximum_offset_disagreement_rad;
    float minimum_emf_v;
    float minimum_abs_speed_rad_s;
    /** Acquisition-enforced shaft speed limit, including between apertures. */
    float maximum_abs_speed_rad_s;
    /** Allowed error beyond one count for trapezoidal shaft displacement. */
    float maximum_motion_error_rad;
};

struct foc_bootstrap_sample {
    struct foc_cal_workflow_identity identity;
    uint32_t capture;
    uint32_t captured_us;
    uint32_t sequence;
    /** Four contiguous sweeps numbered 0 to 3, alternating shaft direction. */
    uint32_t sweep;
    uint32_t raw_code;
    float shaft_rad_s;
    float emf_alpha_v;
    float emf_beta_v;
    /** The producer asserts aperture coherence, sensor validity and no clipping. */
    bool valid;
    /** An accepted open-circuit witness is required for every aperture. */
    bool output_inhibited;
};

struct foc_bootstrap_result {
    enum foc_bootstrap_status status;
    enum foc_bootstrap_quality quality;
    struct foc_cal_workflow_identity identity;
    uint32_t capture;
    enum foc_cal_workflow_source source;
    uint16_t pole_pairs;
    int32_t encoder_direction;
    /** Angle at raw encoder zero, normalized to [0, 2pi). */
    float electrical_offset_rad;
    /** Worst and weakest quality across the four independent fits. */
    float maximum_residual_rad;
    float minimum_candidate_separation_rad;
    float minimum_mechanical_span_rad;
    float maximum_offset_disagreement_rad;
};

/* Public Functions ------------------------------------------------------------ */

/** Inspect preconditions and any reviewed commutation reference without motion. */
enum foc_bootstrap_status foc_bootstrap_qualify(
    const struct foc_cal_workflow_identity *expected_identity,
    const struct foc_cal_workflow_identity *observed_identity,
    const struct foc_bootstrap_prerequisites *prerequisites,
    const struct foc_bootstrap_trusted_reference *trusted_reference);

/** Decode the magnetic axis of an already-qualified aperture. Return NAN for unusable input. */
float foc_bootstrap_reference_angle(const struct foc_bootstrap_sample *sample);

/** Fit four qualified sweeps. Results contain a coarse reference only on success. */
enum foc_bootstrap_status foc_bootstrap_fit(const struct foc_bootstrap_config *config,
                                            const struct foc_bootstrap_sample *samples,
                                            uint32_t sample_count,
                                            struct foc_bootstrap_result *result);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
