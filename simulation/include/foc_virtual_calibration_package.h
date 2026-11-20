/**
 * @file    foc_virtual_calibration_package.h
 * @brief   Fixed version Host package for reviewed virtual calibration.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Design overview
 * ---------------
 * Persist reviewed Host estimates and calibration configuration with identity
 * and integrity checks. Restore policy and output ownership from local code.
 *
 * Usage example
 * -------------
 * Save a reviewed candidate after control verification. Load it against the
 * selected fixture, then initialize fresh Runtime and Encoder storage.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Host Foreground owns the file lifecycle, review and fixed package storage.
 *
 * Failure / recovery
 * ------------------
 * Load publishes a complete package only after validation. Save creates an
 * exclusive new file, preserving prior packages.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration.h"

/* Public Types ----------------------------------------------------------------- */

/** The descriptor borrows offsets in this object. Preserve its address. */
struct foc_virtual_calibration_package {
    struct foc_cal_application_candidate candidate;
    uint32_t review_id;
    bool uses_legacy_policy;
    struct foc_encoder_lut lut;
    int16_t offsets[FOC_ENCODER_CAL_MAX_NODES];
    float flux_wb;
    float inertia_kg_m2;
    float damping_nm_s;
};

enum foc_virtual_calibration_package_status {
    FOC_VIRTUAL_PACKAGE_OK,
    FOC_VIRTUAL_PACKAGE_IO,
    FOC_VIRTUAL_PACKAGE_VERSION,
    FOC_VIRTUAL_PACKAGE_CHECKSUM,
    FOC_VIRTUAL_PACKAGE_IDENTITY,
    FOC_VIRTUAL_PACKAGE_RANGE,
};

/* Public Functions ------------------------------------------------------------- */

/** Compute the review fingerprint over the canonical Host payload. */
int foc_virtual_calibration_package_id(const struct foc_virtual_calibration *session,
                                       const struct foc_cal_application_candidate *candidate,
                                       uint32_t *review_id);

/** Write a new file only. Data are encoded field-by-field, with no pointers. */
int foc_virtual_calibration_package_save(const char *path,
                                         const struct foc_virtual_calibration *session,
                                         const struct foc_cal_application_candidate *candidate);

/** Validate and decode a complete package against the selected Host setup. */
int foc_virtual_calibration_package_load(
    const char *path,
    const struct foc_virtual_current_profile *profile,
    const struct foc_cal_workflow_identity *identity,
    const struct foc_cal_application_tuning_policy *policy,
    const struct foc_cal_application_tuning_policy *legacy_policy,
    struct foc_virtual_calibration_package *package);

const char *foc_virtual_calibration_package_reason(int status);

/* ---------------------------------------- end of file ----------------------------------------- */
