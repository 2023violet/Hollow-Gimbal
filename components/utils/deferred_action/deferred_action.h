/**
 * @file    deferred_action.h
 * @brief   Portable pending/armed deferred-action helper
 * @version v1.0.0
 * @date    2026.04.23
 *
 * Design overview
 * ---------------
 * `deferred_action` models one tiny state machine for callers that need to:
 * - mark work pending immediately
 * - arm one delayed execution window later
 * - observe whether work is still pending or already armed
 * - advance one countdown without pulling RTOS or product policy into the
 *   caller
 *
 * The helper intentionally stays portability-first:
 * no scheduler ownership, no blocking, no product words, and no callback
 * surface. Callers still own what the action means and what to do once it is
 * ready.
 *
 * Usage example
 * -------------
 * struct deferred_action action;
 *
 * deferred_action_init(&action);
 * deferred_action_mark_pending(&action);
 * deferred_action_arm(&action, 250U);
 *
 * if (deferred_action_wait(&action, 50U)) {
 *     // the caller-owned action is now ready to execute
 * }
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns the `struct deferred_action` storage and its lifetime.
 * - The helper borrows no external resources and stores no global state.
 * - Task/thread/ISR safety is entirely caller-managed; this helper does not
 *   add locking.
 * - @note Not thread-safe. Caller must ensure external serialization. Not
 *   ISR-safe.
 *
 * Blocking / allocation
 * ---------------------
 * - All APIs are bounded and non-blocking.
 * - No heap, RTOS, callback, or device dependencies are introduced here.
 *
 * Failure / recovery
 * ------------------
 * - Invalid contexts fail with `-EINVAL` where the API returns a status code.
 * - Query helpers return the conservative idle/false/0 view for NULL input.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "countdown.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Current deferred-action state.
 */
enum deferred_action_state {
    DEFERRED_ACTION_STATE_IDLE = 0,
    DEFERRED_ACTION_STATE_PENDING,
    DEFERRED_ACTION_STATE_ARMED,
};

/**
 * @brief Caller-owned deferred-action instance.
 */
struct deferred_action {
    /** Countdown used while the action is armed. */
    struct countdown           timer;
    /** Current deferred-action state. */
    enum deferred_action_state state;
};

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief Initialize one caller-owned deferred action.
 *
 * @param  action  Instance to initialize.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int deferred_action_init(struct deferred_action *action);
/**
 * @brief Clear one deferred action back to the idle state.
 *
 * @param  action  Instance to clear.
 */
void deferred_action_clear(struct deferred_action *action);
/**
 * @brief Mark that caller-owned work exists but is not armed yet.
 *
 * @param  action  Instance to update.
 */
void deferred_action_mark_pending(struct deferred_action *action);
/**
 * @brief Arm one deferred action to become ready after `delay_ms`.
 *
 * @param  action    Instance to arm.
 * @param  delay_ms  Delay window in milliseconds.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int deferred_action_arm(struct deferred_action *action, uint32_t delay_ms);
/**
 * @brief Report whether the action still has pending or armed work.
 *
 * @param  action  Instance to inspect.
 *
 * @return `true` when the action is pending or armed.
 */
bool deferred_action_has_work(const struct deferred_action *action);
/**
 * @brief Report whether the action is pending but not yet armed.
 *
 * @param  action  Instance to inspect.
 *
 * @return `true` when the current state is pending.
 */
bool deferred_action_is_pending(const struct deferred_action *action);
/**
 * @brief Report whether the action is currently armed.
 *
 * @param  action  Instance to inspect.
 *
 * @return `true` when the current state is armed.
 */
bool deferred_action_is_armed(const struct deferred_action *action);
/**
 * @brief Advance the countdown and report whether the action is now ready.
 *
 * @param  action      Instance to advance.
 * @param  elapsed_ms  Milliseconds elapsed since the previous tick.
 *
 * @return `true` when the delay expired during this call.
 */
bool deferred_action_wait(struct deferred_action *action, uint32_t elapsed_ms);
/**
 * @brief Return the remaining armed delay in milliseconds.
 *
 * @param  action  Instance to inspect.
 *
 * @return Remaining milliseconds, or `0` when idle/pending/unarmed.
 */
uint32_t deferred_action_remaining(const struct deferred_action *action);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
