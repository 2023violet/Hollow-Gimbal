/**
 * @file    tm_deferred_save_runtime.c
 * @brief   Shared deferred-save bookkeeping helper above deferred_action
 * @version v1.0.0
 * @date    2026.04.26
 *
 * Implementation role
 * -------------------
 * - owns deferred dirty/armed bookkeeping built on `deferred_action`
 * - tracks bounded save timing and retry counters without product semantics
 * - leaves persistence backend calls and higher-level policy to the caller
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_deferred_save_runtime.h"
#include <assert.h>
#include <errno.h>

/* Public Functions ----------------------------------------------------------------------------- */

void tm_deferred_save_runtime_reset(struct tm_deferred_save_runtime *runtime)
{
    assert(runtime != NULL);
    if (runtime == NULL) {
        return;
    }

    *runtime = (const struct tm_deferred_save_runtime){
        .action =
            {
                .timer = {0},
                .state = DEFERRED_ACTION_STATE_IDLE,
            },
        .dirty_count       = 0U,
        .request_count     = 0U,
        .attempt_count     = 0U,
        .success_count     = 0U,
        .defer_count       = 0U,
        .last_duration_ms  = 0U,
        .last_result       = -EAGAIN,
    };
    (void)deferred_action_init(&runtime->action);
}

void tm_deferred_save_runtime_clear(struct tm_deferred_save_runtime *runtime)
{
    assert(runtime != NULL);
    if (runtime == NULL) {
        return;
    }

    deferred_action_clear(&runtime->action);
}

void tm_deferred_save_runtime_mark_dirty(struct tm_deferred_save_runtime *runtime)
{
    assert(runtime != NULL);
    if (runtime == NULL) {
        return;
    }

    deferred_action_mark_pending(&runtime->action);
    runtime->dirty_count++;
}

void tm_deferred_save_runtime_request(struct tm_deferred_save_runtime *runtime, uint32_t delay_ms)
{
    assert(runtime != NULL);
    if (runtime == NULL) {
        return;
    }

    if (!deferred_action_is_pending(&runtime->action)) {
        return;
    }

    (void)deferred_action_arm(&runtime->action, delay_ms);
    runtime->request_count++;
}

bool tm_deferred_save_runtime_wait(struct tm_deferred_save_runtime *runtime, uint32_t elapsed_ms)
{
    if ((runtime == NULL) || !deferred_action_is_armed(&runtime->action)) {
        return false;
    }

    if (!deferred_action_wait(&runtime->action, elapsed_ms)) {
        runtime->defer_count++;
        return false;
    }

    return true;
}

void tm_deferred_save_runtime_note_attempt(struct tm_deferred_save_runtime *runtime)
{
    assert(runtime != NULL);
    if (runtime == NULL) {
        return;
    }

    runtime->attempt_count++;
}

int tm_deferred_save_runtime_note_blocked(struct tm_deferred_save_runtime *runtime,
                                          int                              result,
                                          bool                             retry_on_failure,
                                          uint32_t                         retry_delay_ms)
{
    if (runtime == NULL) {
        return -EINVAL;
    }

    runtime->last_duration_ms = 0U;
    runtime->last_result      = result;
    if (retry_on_failure) {
        (void)deferred_action_arm(&runtime->action, retry_delay_ms);
    } else {
        deferred_action_mark_pending(&runtime->action);
    }

    return result;
}

int tm_deferred_save_runtime_finish(struct tm_deferred_save_runtime *runtime,
                                    int                              result,
                                    uint32_t                         started_ms,
                                    uint32_t                         finished_ms,
                                    bool                             retry_on_failure,
                                    uint32_t                         retry_delay_ms)
{
    if (runtime == NULL) {
        return -EINVAL;
    }

    runtime->last_duration_ms = finished_ms - started_ms;
    runtime->last_result      = result;
    if (result == 0) {
        deferred_action_clear(&runtime->action);
        runtime->success_count++;
        return 0;
    }

    if (retry_on_failure) {
        (void)deferred_action_arm(&runtime->action, retry_delay_ms);
    } else {
        deferred_action_mark_pending(&runtime->action);
    }

    return result;
}

bool tm_deferred_save_runtime_diag_get(const struct tm_deferred_save_runtime *runtime,
                                       struct tm_deferred_save_runtime_diag  *diag)
{
    if ((runtime == NULL) || (diag == NULL)) {
        return false;
    }

    *diag = (const struct tm_deferred_save_runtime_diag){
        .dirty              = deferred_action_has_work(&runtime->action),
        .save_requested     = deferred_action_is_armed(&runtime->action),
        .dirty_count        = runtime->dirty_count,
        .request_count      = runtime->request_count,
        .attempt_count      = runtime->attempt_count,
        .success_count      = runtime->success_count,
        .defer_remaining_ms = deferred_action_remaining(&runtime->action),
        .defer_count        = runtime->defer_count,
        .last_duration_ms   = runtime->last_duration_ms,
        .last_result        = runtime->last_result,
    };
    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
