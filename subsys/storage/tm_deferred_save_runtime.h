/**
 * @file    tm_deferred_save_runtime.h
 * @brief   Shared deferred-save bookkeeping helper above deferred_action
 * @version v1.0.0
 * @date    2026.04.26
 *
 * Design overview
 * ---------------
 * `tm_deferred_save_runtime` is a caller-owned mechanism helper for
 * "mark dirty now, persist later" flows:
 * - keeps deferred-save dirty/armed bookkeeping above `deferred_action`
 * - tracks bounded timing and retry diagnostics without owning save semantics
 * - stays backend-agnostic: callers still own what "save" means and when to
 *   invoke the real persistence path
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns one `struct tm_deferred_save_runtime` instance.
 * - The caller decides what counts as dirty state, when one save may be
 *   requested, and which backend actually performs the persistence work.
 * - This helper must not become a second owner of page semantics, product
 *   settings meaning, flash topology, or RTOS cadence policy.
 * - Public entry points are task-context helpers only; callers remain
 *   responsible for any higher-level serialization around the shared state
 *   they persist.
 *
 * Blocking / resources / diagnostics
 * ----------------------------------
 * - The helper allocates no heap, queue, semaphore, or owner thread.
 * - All state transitions are bounded and inline on the caller path.
 * - `tm_deferred_save_runtime_diag` is the formal read-only surface for dirty
 *   truth, armed-save truth, counters, and the most recent save result.
 *
 * Failure / recovery
 * ------------------
 * - The helper never touches the real persistence backend, so it cannot hide
 *   backend errors; callers must pass explicit negative `-errno` style results
 *   into `note_blocked()` or `finish()`.
 * - Retry arming remains caller-directed through the explicit
 *   `retry_on_failure` and `retry_delay_ms` arguments.
 * - `clear()` and `reset()` only affect local bookkeeping and never imply one
 *   storage erase, load, or commit.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "deferred_action.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types --------------------------------------------------------------------------------- */

struct tm_deferred_save_runtime {
    struct deferred_action action;
    uint32_t               dirty_count;
    uint32_t               request_count;
    uint32_t               attempt_count;
    uint32_t               success_count;
    uint32_t               defer_count;
    uint32_t               last_duration_ms;
    int                    last_result;
};

struct tm_deferred_save_runtime_diag {
    bool     dirty;
    bool     save_requested;
    uint32_t dirty_count;
    uint32_t request_count;
    uint32_t attempt_count;
    uint32_t success_count;
    uint32_t defer_remaining_ms;
    uint32_t defer_count;
    uint32_t last_duration_ms;
    int      last_result;
};

/* Public Functions ----------------------------------------------------------------------------- */

void tm_deferred_save_runtime_reset(struct tm_deferred_save_runtime *runtime);
void tm_deferred_save_runtime_clear(struct tm_deferred_save_runtime *runtime);
void tm_deferred_save_runtime_mark_dirty(struct tm_deferred_save_runtime *runtime);
void tm_deferred_save_runtime_request(struct tm_deferred_save_runtime *runtime, uint32_t delay_ms);
bool tm_deferred_save_runtime_wait(struct tm_deferred_save_runtime *runtime, uint32_t elapsed_ms);
void tm_deferred_save_runtime_note_attempt(struct tm_deferred_save_runtime *runtime);
int  tm_deferred_save_runtime_note_blocked(struct tm_deferred_save_runtime *runtime,
                                           int                              result,
                                           bool                             retry_on_failure,
                                           uint32_t                         retry_delay_ms);
int  tm_deferred_save_runtime_finish(struct tm_deferred_save_runtime *runtime,
                                     int                              result,
                                     uint32_t                         started_ms,
                                     uint32_t                         finished_ms,
                                     bool                             retry_on_failure,
                                     uint32_t                         retry_delay_ms);
bool tm_deferred_save_runtime_diag_get(const struct tm_deferred_save_runtime *runtime,
                                       struct tm_deferred_save_runtime_diag  *diag);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
