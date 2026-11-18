/**
 * @file    foc_encoder_calibration.h
 * @brief   Offline periodic encoder LUT fit and independent holdout admission.
 * @version v0.1.0
 * @date    2026.11.27
 *
 * Design overview
 * ---------------
 * Fit reference-minus-raw corrections in raw encoder coordinates. Freeze the
 * knots before checking an independent capture with the production quantizer.
 *
 * Usage example
 * -------------
 * Initialize, push training pairs, freeze, begin holdout, push holdout pairs,
 * then finish. Copy the valid result with export before resetting the context.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller serializes each instance, keeps its address stable until reset,
 * and treats fields as read-only. Exported results own their knots. Keep the
 * exported result alive and immutable while a sensor uses its LUT descriptor.
 *
 * Failure / recovery
 * ------------------
 * A bad observation invalidates the session. Diagnostic counts and residuals
 * remain available, knots are cleared, and reset starts a new capture.
 */
#pragma once

#include "foc_calibration.h"
#include "foc_encoder.h"

/* Public Macros ---------------------------------------------------------------- */

#define FOC_ENCODER_CAL_MAX_NODES 64U

/* Public Types ----------------------------------------------------------------- */

enum foc_encoder_cal_source {
    FOC_ENCODER_CAL_SOURCE_VIRTUAL = 1,
    FOC_ENCODER_CAL_SOURCE_EXTERNAL,
};

enum foc_encoder_cal_status {
    FOC_ENCODER_CAL_OK = 0,
    FOC_ENCODER_CAL_ERR_NULL = -1,
    FOC_ENCODER_CAL_ERR_CONFIG = -2,
    FOC_ENCODER_CAL_ERR_STATE = -3,
    FOC_ENCODER_CAL_ERR_SAMPLE = -4,
    FOC_ENCODER_CAL_ERR_QUALITY = -5,
};

/** Fixed acquisition setup, shared by training and holdout. */
struct foc_encoder_cal_setup {
    uint32_t encoder_identity;
    uint32_t reference_identity;
    uint32_t counts_per_revolution;
    enum foc_encoder_direction direction;
    enum foc_encoder_cal_source source;
};

struct foc_encoder_cal_config {
    /** Production policy used to admit and evaluate the frozen table. */
    struct foc_encoder_config encoder;
    uint32_t encoder_identity;
    uint32_t reference_identity;
    enum foc_encoder_cal_source source;
    uint32_t nodes;
    uint32_t min_samples_per_segment;
    /** Known mechanical reference reading at the declared encoder zero, [0, 2*pi). */
    double reference_zero_rad;
    double max_zero_bias_rad;
    double max_training_segment_rms_rad;
    double max_holdout_rms_rad;
    double max_holdout_peak_rad;
};

struct foc_encoder_cal_sample {
    struct foc_encoder_cal_setup setup;
    uint32_t capture_id;
    uint32_t sequence;
    uint32_t encoder_us;
    uint32_t reference_us;
    uint32_t raw_code;
    /** Independent mechanical position in [0, 2*pi) at the raw-code aperture. */
    double reference_rad;
    bool encoder_valid;
    bool reference_valid;
    /** Acquisition owner attests independent reference and calibrated alignment. */
    bool reference_confirmed;
};

struct foc_encoder_cal_capture {
    uint32_t capture_id;
    uint32_t samples;
    uint32_t first_us;
    uint32_t last_us;
    uint32_t last_sequence;
    uint16_t segment_samples[FOC_ENCODER_CAL_MAX_NODES];
    /** Four quarter-bin bits per segment, from low to high raw code. */
    uint8_t coverage[FOC_ENCODER_CAL_MAX_NODES];
};

struct foc_encoder_cal_result {
    struct foc_encoder_cal_setup setup;
    enum foc_calibration_state state;
    enum foc_calibration_failure failure;
    /** Sensor admission status, when applicable. */
    int sensor_status;
    uint32_t nodes;
    double reference_zero_rad;
    double zero_bias_rad;
    double training_rms_rad;
    double training_max_segment_rms_rad;
    double holdout_raw_rms_rad;
    double holdout_rms_rad;
    double holdout_peak_rad;
    struct foc_encoder_cal_capture training;
    struct foc_encoder_cal_capture holdout;
    int16_t offsets[FOC_ENCODER_CAL_MAX_NODES];
};

/** Streaming training moments for local linear regression. */
struct foc_encoder_cal_segment {
    double fraction;
    double fraction2;
    double correction;
    double fraction_correction;
    double correction2;
};

struct foc_encoder_cal_context {
    struct foc_encoder_cal_config config;
    struct foc_encoder_cal_result result;
    struct foc_encoder_cal_segment segments[FOC_ENCODER_CAL_MAX_NODES];
    struct foc_encoder_context evaluator;
    double radians_per_count;
    double holdout_sse;
    double holdout_raw_sse;
};

/* Public Functions ------------------------------------------------------------- */

/** Invalid init preserves an existing context. */
int foc_encoder_cal_init(struct foc_encoder_cal_context *context,
                         const struct foc_encoder_cal_config *config,
                         uint32_t training_capture_id);

/** Reset all accumulation and diagnostics, retaining the acquisition policy. */
int foc_encoder_cal_reset(struct foc_encoder_cal_context *context, uint32_t training_capture_id);

/** Accept one complete timestamped pair into the active capture only. */
int foc_encoder_cal_push(struct foc_encoder_cal_context *context,
                         const struct foc_encoder_cal_sample *sample);

/** Freeze a training result. At this stage it is not usable. */
int foc_encoder_cal_freeze(struct foc_encoder_cal_context *context);

/**
 * Name a distinct capture after freeze. Its first timestamp must be strictly
 * after training on the shared modular clock. Independence of the samples
 * belongs to the acquisition owner, beyond chronology and ID checks here.
 */
int foc_encoder_cal_begin_holdout(struct foc_encoder_cal_context *context, uint32_t capture_id);

/** Check complete holdout coverage and exact post-quantization error. */
int foc_encoder_cal_finish(struct foc_encoder_cal_context *context);

/** Copy a validated result. Failure leaves result untouched. */
int foc_encoder_cal_export(const struct foc_encoder_cal_context *context,
                           struct foc_encoder_cal_result *result);

/** Borrow a descriptor from a valid, immutable result. Failure leaves LUT untouched. */
int foc_encoder_cal_make_lut(const struct foc_encoder_cal_result *result,
                             struct foc_encoder_lut *lut);

/* ---------------------------------------- end of file ----------------------------------------- */
