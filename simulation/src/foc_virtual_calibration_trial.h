/**
 * @file    foc_virtual_calibration_trial.h
 * @brief   Private Host acquisition variations for seeded quality experiments.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * Reuse the Host capture path with independently repeatable start
 * conditions and bounded instrument dither.
 *
 * Usage example
 * -------------
 * Supply the same trial to Bootstrap and the referenced Workflow polling.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The Foreground caller owns this immutable experiment and any observer.
 *
 * Failure / recovery
 * ------------------
 * Standard capture qualification and rejection remain in effect.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration_reference.h"

/* Public Types ----------------------------------------------------------------- */

struct foc_virtual_cal_trial {
    uint32_t seed;
    uint32_t started_us;
    double initial_position_rad;
    bool reverse_first;
    void (*sample)(void *argument,
                   enum foc_virtual_calibration_stage stage,
                   const struct foc_cal_workflow_sample *sample);
    void *argument;
};

/* Public Functions ------------------------------------------------------------- */

/** Bounded instrument dither keyed by independent seed, aperture and signal. */
static inline int foc_virtual_cal_trial_dither(uint32_t seed, uint32_t us, uint32_t signal)
{
    uint32_t word = seed ^ us ^ (signal * UINT32_C(0x9e3779b9));
    word = (word ^ (word >> 16U)) * UINT32_C(0x7feb352d);
    word = (word ^ (word >> 15U)) * UINT32_C(0x846ca68b);
    return (int)((word ^ (word >> 16U)) % 3U) - 1;
}

static inline struct foc_device_adc_config foc_virtual_cal_trial_adc(
    const struct foc_device_adc_config *source,
    uint32_t seed,
    uint32_t aperture_us
)
{
    struct foc_device_adc_config sensed = *source;
    sensed.phase_a_offset_count += (float)foc_virtual_cal_trial_dither(seed, aperture_us, 1U);
    sensed.phase_b_offset_count += (float)foc_virtual_cal_trial_dither(seed, aperture_us, 2U);
    sensed.phase_c_offset_count += (float)foc_virtual_cal_trial_dither(seed, aperture_us, 3U);
    return sensed;
}

int foc_virtual_cal_cli_bootstrap_sampled(const char *mode,
                                          const char *scenario,
                                          const char *trace,
                                          struct foc_virtual_current_profile *profile,
                                          struct foc_cal_workflow_request *request,
                                          uint32_t *signature,
                                          const struct foc_virtual_cal_trial *trial);

int foc_virtual_calibration_poll_sampled(struct foc_virtual_calibration *session,
                                         const struct foc_virtual_flux_options *options,
                                         struct foc_flux_id_result *evidence,
                                         const struct foc_virtual_cal_trial *trial);

/* ---------------------------------------- end of file ----------------------------------------- */
