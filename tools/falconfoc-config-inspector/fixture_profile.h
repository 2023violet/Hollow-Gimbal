/**
 * @file    fixture_profile.h
 * @brief   Trusted setup for the SOFT-03 Host demonstration image.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Bind an independently known setup to the SOFT-03 demonstration policy.
 *
 * Usage example
 * -------------
 * Initialize a fixture and pass its policy to the Config Store inspector.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns the entire fixture. Keep it at a stable address while
 * its policy borrows the Runtime and Pipeline configuration.
 *
 * Failure / recovery
 * ------------------
 * The initializer assigns a fixed setup. Config Store diagnostics report
 * identity or qualification failures when inspecting a snapshot.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */
#include "foc_config_store.h"
#include "foc_virtual_current.h"

/* Public Types ----------------------------------------------------------------- */
struct inspector_fixture_profile {
    struct foc_virtual_current_profile virtual_current;
    struct foc_pipeline_config pipeline;
    struct foc_config_store_policy policy;
};

/* Public Functions ------------------------------------------------------------- */
/** Use the independent setup of examples/parameters/config_store.c. */
void inspector_fixture_profile_init(struct inspector_fixture_profile *fixture);

/* ---------------------------------------- end of file ----------------------------------------- */
