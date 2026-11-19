/**
 * @file    diagnostics_output.h
 * @brief   Terminal and versioned JSON adapters for the C11 diagnostics API.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Render reports produced by the C11 diagnostics with their source scope.
 *
 * Usage example
 * -------------
 * Pass a completed pair inspection to inspector_print_diagnostics().
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns the reports and diff entries through the output call.
 * Call from the Host CLI with standard output open.
 *
 * Failure / recovery
 * ------------------
 * An unknown explanation code returns false before producing output.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */
#include "foc_config_diagnostics.h"

/* Public Types ----------------------------------------------------------------- */
struct inspector_diff_source {
    enum foc_diagnostic_source source;
    enum foc_config_store_slot slot;
    const char *reason;
    struct foc_config_diff_input input;
};

/* Public Functions ------------------------------------------------------------- */
/** Return stable Host display codes for the observed slot and restore reasons. */
const char *inspector_slot_reason(const struct foc_config_slot_diagnostic *slot);
const char *inspector_restore_reason(const struct foc_config_diagnostic *report);

/** Write a single terminal or JSON result to standard output. */
void inspector_print_diagnostics(const struct foc_config_diagnostic *report,
                                 const char *profile,
                                 bool json);
void inspector_print_diff(const struct inspector_diff_source *left,
                          const struct inspector_diff_source *right,
                          const struct foc_config_diff_entry *fields,
                          size_t count,
                          const char *profile,
                          bool json);

/** Write the explanation for a known display code. */
bool inspector_explain_reason(const char *code, bool json);

/* ---------------------------------------- end of file ----------------------------------------- */
