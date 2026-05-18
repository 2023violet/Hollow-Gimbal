/**
 * @file    soft_timer.c
 * @brief   OS-free software timer core with overflow-safe ordering
 * @version v1.0.0
 * @date    2026.04.24
 *
 * Implementation role
 * -------------------
 * - pairs with `soft_timer.h`
 * - owns sorted-list insertion, wraparound-safe expiry tests, and the
 *   detach-before-fire callback discipline
 *
 * Key collaborators
 * -----------------
 * - `soft_timer.h`: caller-facing lifecycle and callback-context contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "soft_timer.h"
#include <errno.h>

/* Private Helpers ------------------------------------------------------------------------------ */

/**
 * @brief  Return whether @p tick_a has reached or passed @p tick_b.
 *
 * The signed-subtraction idiom keeps the comparison correct across uint32_t
 * wraparound.
 *
 * @param  tick_a  Current tick value.
 * @param  tick_b  Target tick value.
 *
 * @return true if @p tick_a is at or beyond @p tick_b in modular time.
 */
static inline bool _tick_reached(uint32_t tick_a, uint32_t tick_b)
{
    return (int32_t)(tick_a - tick_b) >= 0;
}

/**
 * @brief  Compute the first periodic deadline strictly after @p current_tick.
 *
 * The calculation stays O(1) even when the core has fallen many periods
 * behind. `period_ticks < 2^31` is already enforced by soft_timer_start(),
 * which keeps the modular arithmetic well-defined for the signed-subtraction
 * expiry test used throughout this component.
 */
static uint32_t _periodic_next_deadline(uint32_t previous_deadline,
                                        uint32_t current_tick,
                                        uint32_t period_ticks)
{
    uint32_t elapsed_periods = (uint32_t)((current_tick - previous_deadline) / period_ticks);
    return previous_deadline + ((elapsed_periods + 1U) * period_ticks);
}

/**
 * @brief  Insert a timer into the active list in ascending timeout order.
 *
 * @pre `timer->timeout_tick` is already populated.
 * @pre `timer->is_active == false`.
 * @pre `timer->next == NULL`.
 */
static void _sorted_insert(struct soft_timer_core *core, struct soft_timer *timer)
{
    struct soft_timer **pp = &core->active_list;

    /*
     * Insert before the first timer whose deadline is strictly later than the
     * new timer. Equal deadlines are appended after existing nodes so FIFO
     * order among simultaneous timers is preserved.
     */
    while (*pp != NULL && (int32_t)((*pp)->timeout_tick - timer->timeout_tick) <= 0) {
        pp = &(*pp)->next;
    }

    timer->next = *pp;
    *pp         = timer;

    timer->is_active = true;
    timer->core      = core;
}

/**
 * @brief  Remove a timer from the active list.
 *
 * @param  core   Timer core that owns the list.
 * @param  timer  Timer to remove.
 *
 * @return `true` when the timer was found and removed; otherwise `false`.
 */
static bool _list_remove(struct soft_timer_core *core, struct soft_timer *timer)
{
    struct soft_timer **pp = &core->active_list;

    while (*pp != NULL) {
        if (*pp == timer) {
            *pp              = timer->next;
            timer->next      = NULL;
            timer->is_active = false;
            timer->core      = NULL;
            return true;
        }
        pp = &(*pp)->next;
    }

    return false;
}

/* Public Functions ----------------------------------------------------------------------------- */

/* Core Lifecycle -------------------------------------------------------------------------------- */

int soft_timer_core_init(struct soft_timer_core *core)
{
    if (core == NULL) {
        return -EINVAL;
    }

    core->current_tick = 0U;
    core->active_list  = NULL;

    return 0;
}

void soft_timer_core_update(struct soft_timer_core *core, uint32_t delta_ticks)
{
    if (core == NULL || delta_ticks == 0U) {
        return;
    }

    /* Intentional uint32_t wraparound keeps the tick base monotonic modulo 2^32. */
    core->current_tick += delta_ticks;

    /*
     * Drain expired timers from the head of the sorted list.
     *
     * The list is ordered by timeout, so once the head has not expired, no
     * later node can have expired either.
     *
     * The callback discipline is "detach before fire":
     * 1. Pop the head node from the list.
     * 2. Re-arm periodic timers before invoking the callback.
     * 3. Invoke the callback.
     * 4. Re-read `core->active_list` on the next loop iteration instead of
     *    trusting a cached next pointer.
     *
     * This keeps mutation from callbacks safe. A callback may stop the current
     * timer, restart it, or insert new timers without invalidating the outer
     * traversal.
     */
    for (;;) {
        struct soft_timer *node = core->active_list;
        bool               should_reinsert;
        uint32_t           next_periodic_deadline = 0U;

        if (node == NULL) {
            break;
        }
        if (!_tick_reached(core->current_tick, node->timeout_tick)) {
            break;
        }

        core->active_list = node->next;
        node->next        = NULL;
        node->is_active   = false;
        node->core        = NULL;
        node->is_firing   = true;
        node->suppress_rearm = false;

        should_reinsert = node->is_periodic;
        if (should_reinsert) {
            next_periodic_deadline =
                _periodic_next_deadline(node->timeout_tick, core->current_tick, node->period_ticks);
        }

        if (node->callback != NULL) {
            node->callback(node->user_data);
        }

        node->is_firing = false;

        if (should_reinsert && !node->is_active && !node->suppress_rearm) {
            node->timeout_tick = next_periodic_deadline;
            node->next         = NULL;
            _sorted_insert(core, node);
        }

        node->suppress_rearm = false;
    }
}

/* Timer Lifecycle ------------------------------------------------------------------------------ */

int soft_timer_init(struct soft_timer *timer, soft_timer_cb_t callback, void *user_data)
{
    if (timer == NULL || callback == NULL) {
        return -EINVAL;
    }

    timer->timeout_tick = 0U;
    timer->period_ticks = 0U;
    timer->is_periodic  = false;
    timer->is_active    = false;
    timer->is_firing    = false;
    timer->suppress_rearm = false;
    timer->callback     = callback;
    timer->user_data    = user_data;
    timer->core         = NULL;
    timer->next         = NULL;

    return 0;
}

int soft_timer_start(struct soft_timer_core *core,
                     struct soft_timer      *timer,
                     uint32_t                delay_ticks,
                     bool                    is_periodic)
{
    if (core == NULL || timer == NULL || delay_ticks == 0U || delay_ticks >= 0x80000000U) {
        return -EINVAL;
    }

    /*
     * Restart semantics are deliberate: if the timer is already active, remove
     * the old instance first so the new start is idempotent and does not leave
     * a stale node behind in the list.
     */
    if (timer->is_active) {
        _list_remove(timer->core, timer);
    }

    timer->timeout_tick = core->current_tick + delay_ticks;
    timer->period_ticks = delay_ticks;
    timer->is_periodic  = is_periodic;
    timer->next         = NULL;

    _sorted_insert(core, timer);

    return 0;
}

void soft_timer_stop(struct soft_timer *timer)
{
    if (timer == NULL) {
        return;
    }

    if (timer->is_firing) {
        timer->suppress_rearm = true;
        return;
    }

    if (!timer->is_active || timer->core == NULL) {
        return;
    }

    _list_remove(timer->core, timer);
}

/* ---------------------------------------- end of file ----------------------------------------- */
