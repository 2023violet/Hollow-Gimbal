/**
 * @file    countdown.h
 * @brief   Portable elapsed-time countdown helper
 * @version v1.0.0
 * @date    2026.04.23
 *
 * Design overview
 * ---------------
 * countdown models one restartable "remaining milliseconds" state machine.
 * It does not own a clock source, does not allocate memory, and does not
 * invoke callbacks. The owner stores one struct countdown instance and feeds
 * elapsed time explicitly through countdown_tick().
 *
 * This shape keeps the helper portability-first:
 * - no RTOS dependency
 * - no board facts
 * - no hidden cadence or timer thread
 *
 * Usage example
 * -------------
 * @code
 * struct countdown retry_timer;
 *
 * countdown_init(&retry_timer);
 * countdown_start(&retry_timer, 250U);
 *
 * if (countdown_tick(&retry_timer, elapsed_ms)) {
 *     retry_render();
 * }
 * @endcode
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns the struct countdown storage.
 * - The helper keeps no external references.
 * - The same countdown instance is not thread-safe by itself; one execution
 *   context must own each instance, or callers must serialize access.
 * - ISR use is allowed only if the caller's own context rules allow plain
 *   struct reads/writes there; the helper itself performs no blocking work.
 * - @note Not thread-safe for a shared instance. Caller must ensure external
 *   serialization. ISR-safe only when one context owns the instance.
 *
 * Blocking / allocation
 * --------------------
 * - All APIs are bounded and non-blocking.
 * - Dynamic allocation: none.
 * - Callback re-entry: none.
 *
 * Failure / recovery
 * ------------------
 * - countdown_init() and countdown_start() return 0 / -errno.
 * - countdown_stop(), countdown_is_active(), and countdown_remaining() are
 *   defensive no-ops for NULL input.
 * - countdown_tick() returns true only on the transition from active to
 *   expired; otherwise it returns false.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief  Countdown instance state.
 *
 * Zero-initialised storage is a valid inactive state.
 */
struct countdown {
    uint32_t remaining_ms; /* Remaining duration until expiry. */
    bool     active;       /* true while the countdown is armed. */
};

/* Public function declarations ----------------------------------------------------------------- */

/**
 * @brief  Initialise a countdown instance to the inactive state.
 *
 * @param  timer  Countdown instance to initialise.
 *
 * @retval 0        Initialization succeeded.
 * @retval -EINVAL  @p timer is NULL.
 */
int countdown_init(struct countdown *timer);

/**
 * @brief  Start or restart a countdown.
 *
 * @param  timer         Countdown instance to arm.
 * @param  duration_ms   Initial remaining duration in milliseconds. Must be
 *                       greater than 0.
 *
 * @retval 0        Start succeeded.
 * @retval -EINVAL  @p timer is NULL or @p duration_ms is 0.
 */
int countdown_start(struct countdown *timer, uint32_t duration_ms);

/**
 * @brief  Stop a countdown and clear its remaining duration.
 *
 * @param  timer  Countdown instance to stop. NULL is silently ignored.
 */
void countdown_stop(struct countdown *timer);

/**
 * @brief  Advance one countdown by an elapsed duration.
 *
 * @param  timer        Countdown instance to advance.
 * @param  elapsed_ms   Elapsed milliseconds since the previous tick.
 *
 * @return `true` when the countdown expired during this call; otherwise
 *         `false`.
 */
bool countdown_tick(struct countdown *timer, uint32_t elapsed_ms);

/**
 * @brief  Report whether a countdown is currently armed.
 *
 * @param  timer  Countdown instance to query.
 *
 * @return `true` when the instance is active; otherwise `false`.
 */
static inline bool countdown_is_active(const struct countdown *timer)
{
    if (timer == NULL) {
        return false;
    }

    return timer->active;
}

/**
 * @brief  Return the remaining duration of a countdown.
 *
 * @param  timer  Countdown instance to query.
 *
 * @return Remaining milliseconds, or 0 when inactive or NULL.
 */
static inline uint32_t countdown_remaining(const struct countdown *timer)
{
    if ((timer == NULL) || !timer->active) {
        return 0U;
    }

    return timer->remaining_ms;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
