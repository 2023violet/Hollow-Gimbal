/**
 * @file    soft_timer.h
 * @brief   Themis caller-driven software timer core
 * @version v1.0.0
 * @date    2026.03.02
 *
 * Design overview
 * ---------------
 * `soft_timer` provides a caller-driven software timer core with no owned
 * clock source. Callers feed elapsed ticks explicitly through
 * `soft_timer_core_update()`, while the core maintains a sorted active list.
 *
 * Usage example
 * -------------
 * struct soft_timer_core g_timer_core;
 * struct soft_timer      blink_timer;
 *
 * soft_timer_core_init(&g_timer_core);
 * soft_timer_init(&blink_timer, on_blink, NULL);
 * soft_timer_start(&g_timer_core, &blink_timer, 500U, true);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - Callers own both timer nodes and the timer core.
 * - Callbacks run in whatever context calls `soft_timer_core_update()`.
 * - The callback may stop or restart timers safely because the core detaches
 *   expired timers before firing callbacks.
 * - @note Not thread-safe for a shared core or timer set. Caller must ensure
 *   external serialization. ISR-safe only when the entire timer core is owned
 *   by that ISR context.
 *
 * Blocking / allocation
 * ---------------------
 * - No allocation and no OS dependency.
 * - Tick expiry uses the signed-subtraction wraparound-safe idiom.
 * - Update is O(1) in the common case because the active list stays sorted.
 *
 * Failure / recovery
 * ------------------
 * - Invalid lifecycle inputs fail with `-EINVAL`.
 * - Delays must remain below `2^31` ticks so wrap-safe signed-subtraction
 *   comparisons stay valid for the full active lifetime.
 * - Stopping an inactive timer is a safe no-op.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward Declarations ------------------------------------------------------------------------- */

struct soft_timer_core;

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief  Callback type invoked when a timer expires.
 *
 * @param[in] user_data  Opaque pointer provided at `soft_timer_init()` time.
 *
 * @note   The callback runs in whatever context calls
 *         `soft_timer_core_update()`, typically an ISR or a periodic task.
 *         Keep callbacks short. Calling `soft_timer_stop()` on the timer that
 *         fired, or `soft_timer_start()` on any timer, is explicitly safe.
 */
typedef void (*soft_timer_cb_t)(void *user_data);

/**
 * @brief  Software timer node.
 *
 * @note   Embed one of these in your module state or declare it statically.
 *         Initialize it with `soft_timer_init()` before first use, and do not
 *         modify any field directly after initialization.
 */
struct soft_timer {
    /** Absolute tick at which this timer expires. */
    uint32_t timeout_tick;
    /** Reload interval for periodic timers, in ticks. */
    uint32_t period_ticks;

    /** `true` when the timer auto-reloads after expiry. */
    bool is_periodic;
    /** `true` while the timer is linked into one core's active list. */
    bool is_active;
    /** Internal callback-state flag used to make self-stop/self-restart safe. */
    bool is_firing;
    /** Internal one-shot override set when a firing periodic timer is stopped. */
    bool suppress_rearm;

    /** Callback invoked when the timer expires. */
    soft_timer_cb_t callback;
    /** Opaque pointer forwarded to `callback`. */
    void           *user_data;

    /** Core that currently owns this timer, or `NULL` when inactive. */
    struct soft_timer_core *core;
    /** Next node in the core's sorted active list. */
    struct soft_timer      *next;
};

/**
 * @brief  Software timer manager core.
 *
 * @note   Use one instance per execution context, for example one for a
 *         1 kHz task and another for a 10 kHz ISR. Timers are bound to a
 *         specific core at start time.
 */
struct soft_timer_core {
    /** Monotonically increasing tick counter owned by the caller. */
    uint32_t           current_tick;
    /** Head of the sorted active timer list. */
    struct soft_timer *active_list;
};

/* Public Functions ----------------------------------------------------------------------------- */

/* Core Lifecycle -------------------------------------------------------------------------------- */

/**
 * @brief  Initialize one timer core.
 *
 * @param[out] core  Core to initialize. It must remain valid for the lifetime
 *                   of all timers attached to it.
 *
 * @retval 0          Initialization succeeded.
 * @retval -EINVAL    `core` is `NULL`.
 */
int soft_timer_core_init(struct soft_timer_core *core);

/**
 * @brief  Advance one core clock and fire all expired timers.
 *
 * Call this function at a fixed rate, for example every millisecond from a
 * SysTick handler or periodic task. It is safe to call with `delta_ticks > 1`
 * if ticks were missed, such as after a sleep period, though callbacks will
 * all fire in the same update call.
 *
 * Mutation immunity guarantee:
 * Before invoking any callback the expired timer is detached from the active
 * list. Periodic timers compute their next deadline from the previous expiry
 * point, then re-insert only after the callback returns unless the callback
 * explicitly restarted or stopped that timer itself.
 * Consequently the callback may call `soft_timer_stop()` or
 * `soft_timer_start()` on any timer, including the one that just fired,
 * without corrupting the iterator or causing a double-remove.
 *
 * Overflow safety:
 * Expiry is tested with `(int32_t)(current_tick - timeout_tick) >= 0`.
 * This arithmetic stays correct across `uint32_t` wrap-around provided the
 * delay is less than `2^31` ticks.
 *
 * @param[in,out] core         Initialized timer core.
 * @param[in]     delta_ticks  Number of ticks elapsed since the previous call.
 *                             Passing `0` is a no-op.
 */
void soft_timer_core_update(struct soft_timer_core *core, uint32_t delta_ticks);

/* Timer Lifecycle ------------------------------------------------------------------------------ */

/**
 * @brief  Initialize one timer node without starting it.
 *
 * @param[out] timer      Timer to initialize.
 * @param[in]  callback   Callback invoked on expiry. Must not be `NULL`.
 * @param[in]  user_data  Opaque argument forwarded to `callback`.
 *
 * @retval 0          Initialization succeeded.
 * @retval -EINVAL    `timer` or `callback` is `NULL`.
 */
int soft_timer_init(struct soft_timer *timer, soft_timer_cb_t callback, void *user_data);

/**
 * @brief  Start or restart one timer.
 *
 * The timer is inserted into `core`'s active list in sorted order. If the
 * timer is already active it is stopped and re-inserted with the new
 * parameters, allowing restartable-watchdog style usage.
 *
 * @param[in,out] core         Core that will own the timer.
 * @param[in,out] timer        Timer previously initialized with
 *                             `soft_timer_init()`.
 * @param[in]     delay_ticks  Number of ticks until first expiry. Must be in
 *                             the range `[1, 2^31 - 1]`. Larger values violate
 *                             the signed-subtraction overflow-safety contract.
 * @param[in]     is_periodic  `true` for auto-reload, `false` for one-shot.
 *
 * @retval 0          Start succeeded.
 * @retval -EINVAL    Any pointer is `NULL` or `delay_ticks` is zero.
 */
int soft_timer_start(struct soft_timer_core *core,
                     struct soft_timer      *timer,
                     uint32_t                delay_ticks,
                     bool                    is_periodic);

/**
 * @brief  Stop one active timer.
 *
 * Removing an already inactive timer is a safe no-op. This helper is also
 * safe to call from within the timer's own callback.
 *
 * @param[in,out] timer  Timer to stop. `NULL` is silently ignored.
 */
void soft_timer_stop(struct soft_timer *timer);

/* Status Queries -------------------------------------------------------------------------------- */

/**
 * @brief  Check whether one timer is currently active.
 *
 * @param[in] timer  Timer to query.
 *
 * @return `true` when the timer is active; otherwise `false`.
 */
static inline bool soft_timer_is_active(const struct soft_timer *timer)
{
    if (timer == NULL) {
        return false;
    }

    return timer->is_active;
}

/**
 * @brief  Return the number of ticks remaining until one timer fires.
 *
 * @param[in] timer  Active timer to query.
 *
 * @return Remaining ticks, or `0` if the timer is expired, inactive, or
 *         `timer` is `NULL`.
 *
 * @note   This is an O(1) read of the timer's cached deadline field and does
 *         not traverse the list.
 */
static inline uint32_t soft_timer_remaining(const struct soft_timer *timer)
{
    uint32_t current;
    uint32_t target;

    if ((timer == NULL) || !timer->is_active || (timer->core == NULL)) {
        return 0U;
    }

    current = timer->core->current_tick;
    target  = timer->timeout_tick;

    if ((int32_t)(current - target) >= 0) {
        return 0U;
    }

    return target - current;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
