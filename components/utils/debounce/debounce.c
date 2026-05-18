/**
 * @file    debounce.c
 * @brief   Themis Asymmetric Saturating Counter / Leaky Integrator
 * @version v1.0.0
 * @date    2026.03.03
 *
 * Implementation role
 * -------------------
 * - pairs with `debounce.h`
 * - owns the saturating counter update path and limit clamping details
 * - keeps the hot path branch-light and allocation-free
 *
 * Key collaborators
 * -----------------
 * - `debounce.h`: caller-facing contract and usage entry
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "debounce.h"
#include <errno.h>

/* Public Functions ----------------------------------------------------------------------------- */

int debounce_init(struct debounce *ctx, uint32_t limit_on, uint32_t limit_off, bool init_state)
{
    uint64_t max_val64;

    if (ctx == NULL) {
        return -EINVAL;
    }

    /* Clamp zero limits to the minimal valid one-sample contract. */
    ctx->limit_on  = (limit_on > 0U) ? limit_on : 1U;
    ctx->limit_off = (limit_off > 0U) ? limit_off : 1U;
    ctx->state     = init_state;

    /* Prime the accumulator to the requested logical starting point. */
    if (init_state) {
        max_val64    = (uint64_t)ctx->limit_on * (uint64_t)ctx->limit_off;
        ctx->counter = (max_val64 > UINT32_MAX) ? UINT32_MAX : (uint32_t)max_val64;
    } else {
        ctx->counter = 0U;
    }

    return 0;
}

int debounce_update(struct debounce *ctx, bool raw_input)
{
    uint64_t max_val64;
    uint32_t max_val;

    if (ctx == NULL) {
        return -EINVAL;
    }

    /* Cross-multiplied state space keeps asymmetric thresholds division-free. */
    uint32_t step_up   = ctx->limit_off;
    uint32_t step_down = ctx->limit_on;

    max_val64 = (uint64_t)step_up * (uint64_t)step_down;
    max_val   = (max_val64 > UINT32_MAX) ? UINT32_MAX : (uint32_t)max_val64;

    if (raw_input) {
        /* Integrate high samples upward and saturate at the trigger boundary. */
        if (ctx->counter < max_val) {
            uint32_t space = max_val - ctx->counter;
            if (space <= step_up) {
                ctx->counter = max_val;
                ctx->state   = true;
            } else {
                ctx->counter += step_up;
            }
        }
    } else {
        /* Integrate low samples downward and saturate at the release boundary. */
        if (ctx->counter > 0U) {
            if (ctx->counter <= step_down) {
                ctx->counter = 0U;
                ctx->state   = false;
            } else {
                ctx->counter -= step_down;
            }
        }
    }

    return ctx->state;
}

/* ---------------------------------------- end of file ----------------------------------------- */
