/**
 * @file    foc_virtual_calibration_flux.h
 * @brief   Private link between virtual acquisition and portable flux evidence.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * Carry original dependency revisions into the estimator and optionally export
 * each accepted interval. The legacy Host capture result retains its layout.
 *
 * Usage example
 * -------------
 * Pass the reviewed revisions when replaying selected stages. Omit options
 * when all prerequisites were captured in the current workflow session.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * A serialized Host caller owns the options and borrows interval rows.
 *
 * Failure / recovery
 * ------------------
 * The capture and its Kt are usable only after the enclosing session and
 * generic quality gates pass. Cancelled sessions revoke application.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_flux_id.h"
#include "foc_virtual_calibration.h"

/* Public Types ----------------------------------------------------------------- */

struct foc_virtual_flux_options {
    uint32_t rl_revision;
    uint32_t poles_revision;
    uint32_t current_revision;
    uint32_t voltage_chain;
    uint32_t angle_reference;
    void (*interval)(void *argument, const struct foc_flux_id_interval *sample);
    void *argument;
};

/* Public Functions ------------------------------------------------------------- */

int foc_virtual_calibrate_flux_observed(struct foc_virtual_motor *motor,
                                        const struct foc_virtual_cal_electrical_config *config,
                                        const struct foc_cal_workflow_bundle *bundle,
                                        uint32_t capture,
                                        uint32_t first_us,
                                        struct foc_virtual_cal_flux_result *legacy,
                                        const struct foc_virtual_flux_options *options,
                                        struct foc_flux_id_result *evidence);

int foc_virtual_calibration_poll_observed(struct foc_virtual_calibration *session,
                                          const struct foc_virtual_flux_options *options,
                                          struct foc_flux_id_result *evidence);

/** Project only the valid measured-Flux/derived-Kt pair, never an independent-torque claim. */
bool foc_virtual_flux_agrees(const struct foc_virtual_calibration *session,
                             const struct foc_flux_id_result *evidence);

/* ---------------------------------------- end of file ----------------------------------------- */

