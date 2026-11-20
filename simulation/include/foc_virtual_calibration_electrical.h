/**
 * @file    foc_virtual_calibration_electrical.h
 * @brief   Bounded Host Virtual Motor capture for R/L and back-EMF identification.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Design overview
 * ---------------
 * Constrain the shaft for separate d and q excitations and drive it in both
 * directions to fit magnet flux from the q-axis voltage equation.
 *
 * Usage example
 * -------------
 * Configure the ADC acquisition and qualified decoder. Measure d then q with
 * distinct capture IDs, and use the paired model with a qualified rotor result.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * A serialized Foreground caller supplies a dedicated plant, ADC acquisition
 * and decoder settings, a workflow session, and a shared capture clock in us.
 * A successful experiment leaves zero terminal voltage and releases the fixture.
 * These experiments exercise the Host plant boundary only.
 *
 * Failure / recovery
 * ------------------
 * The supplied motor remains unchanged when an experiment is rejected. Close
 * or cancel the workflow according to its result before starting a new session.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_workflow.h"
#include "foc_virtual_motor.h"

/* Public Types ---------------------------------------------------------------- */

/** Measurement chain and excitation bounds for the supported Virtual Motor profiles. */
struct foc_virtual_cal_electrical_config {
    /** Sensor transfer settings from the virtual acquisition profile. */
    struct foc_device_adc_config sensor_adc;
    /** Decoder settings with offsets obtained from qualified ADC acquisition. */
    struct foc_device_adc_config decoded_adc;
    float vbus_v;
    float rl_amplitude_v;
    float max_current_a;
    /** Basic magnitude of the two signed flux speeds, in electrical rad/s. */
    float flux_electrical_speed_rad_s;
    /** Optional stop and deadline check before each proposed aperture. */
    bool (*permit)(void *argument, uint32_t aperture_us);
    void *permit_argument;
    /** Optional synchronous recording of accepted R/L rows. Sample is borrowed for the call. */
    void (*rl_endpoint)(void *argument,
                        enum foc_motor_id_axis axis,
                        const struct foc_motor_id_sample *sample);
    void *rl_endpoint_argument;
};

/** Quality, provenance, and SI results of a distinct back-EMF capture. */
struct foc_virtual_cal_flux_result {
    uint32_t capture;
    uint32_t first_us;
    uint32_t last_us;
    uint32_t samples;
    enum foc_calibration_failure failure;
    bool qualified;
    float flux_wb;
    /** Open-circuit phase-to-neutral peak voltage per mechanical rad/s. */
    float ke_phase_peak_v_per_mechanical_rad_s;
    /** Permanent-magnet contribution at Id=0, per peak q-axis ampere. */
    float kt_nm_per_peak_q_amp;
    double residual_rms_v;
    double omega_e_rms_rad_s;
    /** Workflow Kt derived from fitted back-EMF, independent of J/B acquisition. */
    struct foc_cal_workflow_kt kt;
};

/* Public Functions ------------------------------------------------------------- */

/** Return the nominal Virtual Motor measurement and excitation policy. */
struct foc_virtual_cal_electrical_config foc_virtual_cal_electrical_default_config(void);

/**
 * Measure one constrained-shaft axis and submit its raw-aperture-converted
 * endpoints and last applied interval voltages to the existing workflow.
 * `aligned_electrical_rad` is the independent electrical reference at the
 * plant's current, stationary position. The q capture follows d on the same
 * clock. Returns workflow status, including PARTIAL after d in dual-axis mode.
 * The endpoint callback must not re-enter or mutate the workflow, config, or plant.
 */
int foc_virtual_calibrate_rl_axis(struct foc_cal_workflow *workflow,
                                  struct foc_virtual_motor *motor,
                                  const struct foc_virtual_cal_electrical_config *config,
                                  enum foc_motor_id_axis axis,
                                  uint32_t capture,
                                  uint32_t first_us,
                                  double aligned_electrical_rad);

/**
 * Fit lambda from signed, observed electrical motion and terminal measurements
 * using the workflow's qualified R/L model and pole-pair result. Return a
 * separate Host Virtual capture. Capture ID must be unused in the session.
 * Ke = p*lambda and Kt = 1.5*p*lambda in the declared conventions.
 */
int foc_virtual_calibrate_flux(struct foc_virtual_motor *motor,
                               const struct foc_virtual_cal_electrical_config *config,
                               const struct foc_cal_workflow_bundle *bundle,
                               uint32_t capture,
                               uint32_t first_us,
                               struct foc_virtual_cal_flux_result *result);

/* ---------------------------------------- end of file ----------------------------------------- */
