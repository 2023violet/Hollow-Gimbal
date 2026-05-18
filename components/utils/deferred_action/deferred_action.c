/**
 * @file    deferred_action.c
 * @brief   Portable pending/armed deferred-action helper
 * @version v1.0.0
 * @date    2026.04.23
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "deferred_action.h"
#include <errno.h>

/* Public Functions ----------------------------------------------------------------------------- */

int deferred_action_init(struct deferred_action *action)
{
    if (action == NULL) {
        return -EINVAL;
    }

    *action = (const struct deferred_action){
        .timer = {0},
        .state = DEFERRED_ACTION_STATE_IDLE,
    };
    return countdown_init(&action->timer);
}

void deferred_action_clear(struct deferred_action *action)
{
    if (action == NULL) {
        return;
    }

    action->state = DEFERRED_ACTION_STATE_IDLE;
    countdown_stop(&action->timer);
}

void deferred_action_mark_pending(struct deferred_action *action)
{
    if (action == NULL) {
        return;
    }

    action->state = DEFERRED_ACTION_STATE_PENDING;
    countdown_stop(&action->timer);
}

int deferred_action_arm(struct deferred_action *action, uint32_t delay_ms)
{
    if (action == NULL) {
        return -EINVAL;
    }

    action->state = DEFERRED_ACTION_STATE_ARMED;
    if (delay_ms == 0U) {
        countdown_stop(&action->timer);
        return 0;
    }

    return countdown_start(&action->timer, delay_ms);
}

bool deferred_action_has_work(const struct deferred_action *action)
{
    return (action != NULL) && (action->state != DEFERRED_ACTION_STATE_IDLE);
}

bool deferred_action_is_pending(const struct deferred_action *action)
{
    return (action != NULL) && (action->state == DEFERRED_ACTION_STATE_PENDING);
}

bool deferred_action_is_armed(const struct deferred_action *action)
{
    return (action != NULL) && (action->state == DEFERRED_ACTION_STATE_ARMED);
}

bool deferred_action_wait(struct deferred_action *action, uint32_t elapsed_ms)
{
    if (!deferred_action_is_armed(action)) {
        return false;
    }

    if (!countdown_is_active(&action->timer)) {
        return true;
    }

    (void)countdown_tick(&action->timer, elapsed_ms);
    return !countdown_is_active(&action->timer);
}

uint32_t deferred_action_remaining(const struct deferred_action *action)
{
    if (action == NULL) {
        return 0U;
    }

    return countdown_remaining(&action->timer);
}

/* ---------------------------------------- end of file ----------------------------------------- */
