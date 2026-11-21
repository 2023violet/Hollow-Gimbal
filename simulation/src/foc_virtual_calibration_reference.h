/**
 * @file    foc_virtual_calibration_reference.h
 * @brief   Qualified count-derived phase for Host calibration experiments.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * Separate the established exact-reference comparison fixture and the
 * bootstrap path using the qualified production Encoder and mechanical clock.
 *
 * Usage example
 * -------------
 * Poll the reference-backed calibration after bootstrap supplied the poles
 * and direction. The Workflow still acquires and qualifies its own fine offset.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * A serialized Host caller owns a dedicated Plant and Workflow in Foreground.
 *
 * Failure / recovery
 * ------------------
 * Missing or inconsistent source observations cause full capture rejection.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration_flux.h"

/* Public Types ----------------------------------------------------------------- */

struct foc_virtual_cal_trial;

struct foc_virtual_phase_source {
    bool (*read)(const void *argument,
                 const struct foc_virtual_motor *motor,
                 uint32_t aperture_us,
                 double *theta,
                 double *shaft_speed);
    const void *argument;
    const struct foc_virtual_cal_trial *trial;
};

/* Public Functions ------------------------------------------------------------- */

int foc_virtual_calibration_poll_referenced(struct foc_virtual_calibration *session,
                                            const struct foc_virtual_flux_options *options,
                                            struct foc_flux_id_result *evidence);

int foc_virtual_calibrate_rl_observed(struct foc_cal_workflow *workflow,
                                      struct foc_virtual_motor *motor,
                                      const struct foc_virtual_cal_electrical_config *config,
                                      enum foc_motor_id_axis axis,
                                      uint32_t capture,
                                      uint32_t first_us,
                                      double aligned_electrical_rad,
                                      const struct foc_virtual_phase_source *reference);

int foc_virtual_calibrate_flux_referenced(struct foc_virtual_motor *motor,
                                          const struct foc_virtual_cal_electrical_config *config,
                                          const struct foc_cal_workflow_bundle *bundle,
                                          uint32_t capture,
                                          uint32_t first_us,
                                          struct foc_virtual_cal_flux_result *result,
                                          const struct foc_virtual_flux_options *options,
                                          struct foc_flux_id_result *evidence,
                                          const struct foc_virtual_phase_source *reference);

/* ---------------------------------------- end of file ----------------------------------------- */

