/**
 * @file    countdown.c
 * @brief   Portable elapsed-time countdown helper
 * @version v1.0.0
 * @date    2026.04.23
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "countdown.h"

int countdown_init(struct countdown *timer)
{
    if (timer == NULL) {
        return -EINVAL;
    }

    timer->remaining_ms = 0U;
    timer->active       = false;
    return 0;
}

int countdown_start(struct countdown *timer, uint32_t duration_ms)
{
    if ((timer == NULL) || (duration_ms == 0U)) {
        return -EINVAL;
    }

    timer->remaining_ms = duration_ms;
    timer->active       = true;
    return 0;
}

void countdown_stop(struct countdown *timer)
{
    if (timer == NULL) {
        return;
    }

    timer->remaining_ms = 0U;
    timer->active       = false;
}

bool countdown_tick(struct countdown *timer, uint32_t elapsed_ms)
{
    if ((timer == NULL) || !timer->active || (elapsed_ms == 0U)) {
        return false;
    }

    /*
     * The helper stores remaining time instead of an absolute deadline, so the
     * caller can feed any cadence without sharing a clock base with the helper.
     */
    if (elapsed_ms >= timer->remaining_ms) {
        countdown_stop(timer);
        return true;
    }

    timer->remaining_ms -= elapsed_ms;
    return false;
}

/* ---------------------------------------- end of file ----------------------------------------- */
