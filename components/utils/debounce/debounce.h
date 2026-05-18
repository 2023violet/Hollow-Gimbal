/**
 * @file    debounce.h
 * @brief   Themis Asymmetric Saturating Counter / Leaky Integrator
 * @version v1.0.0
 * @date    2026.03.03
 *
 * Design overview
 * ---------------
 * `debounce` implements a caller-owned asymmetric leaky integrator for noisy
 * digital inputs. It keeps one saturating counter per signal and uses
 * cross-multiplied step sizes so `limit_on` and `limit_off` can differ
 * without floating-point math or division.
 *
 * Usage example
 * -------------
 * struct debounce sw = {0};
 *
 * debounce_init(&sw, 3U, 8U, false);
 * if (debounce_update(&sw, raw_level) > 0) {
 *     filtered_level = true;
 * }
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - The caller owns `struct debounce` storage and may place it in stack,
 *   static storage, or a larger device/app context.
 * - One instance represents one logical signal path.
 * - The same instance must be driven from one serialized execution context;
 *   this helper does not provide locking.
 * - @note Not thread-safe. Caller must ensure external serialization. Not
 *   ISR-safe unless one ISR or one task exclusively owns the instance.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no callback re-entry.
 * - `debounce_init()` and `debounce_update()` are O(1).
 *
 * Failure / recovery
 * ------------------
 * - Invalid context pointers fail with `-EINVAL`.
 * - Zero limits are clamped to `1`, so callers recover to a valid immediate
 *   transition contract instead of leaving a dead configuration behind.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief State debouncer control block.
 */
struct debounce {
    uint32_t counter;   /* Internal integration accumulator.               */
    uint32_t limit_on;  /* Consecutive true samples required to trigger.   */
    uint32_t limit_off; /* Consecutive false samples required to recover.  */
    bool     state;     /* Current filtered steady-state.                  */
};

/* Public Functions ----------------------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------------------------ */

/**
 * @brief  Initialize the debounce state machine.
 *
 * @param  ctx         Pointer to the debounce control block.
 * @param  limit_on    Number of ticks to validate a true state.
 * @param  limit_off   Number of ticks to validate a false state.
 * @param  init_state  Initial logical state.
 *
 * @retval 0         Initialization succeeded.
 * @retval -EINVAL   @p ctx is NULL.
 */
int debounce_init(struct debounce *ctx, uint32_t limit_on, uint32_t limit_off, bool init_state);

/* Runtime Step --------------------------------------------------------------------------------- */

/**
 * @brief  Feed one raw sample into the integrator and return the filtered state.
 *
 * @param  ctx        Pointer to the debounce control block.
 * @param  raw_input  Instantaneous unfiltered physical state.
 *
 * @return `1` when the filtered state is high, `0` when it is low, or
 *         negative `-EINVAL` when @p ctx is NULL.
 *
 * @note Must be called at a deterministic cadence chosen by the caller.
 */
int debounce_update(struct debounce *ctx, bool raw_input);

/* Snapshot ------------------------------------------------------------------------------------- */

/**
 * @brief  Query the current filtered state without advancing the integrator.
 *
 * @param  ctx  Pointer to the debounce control block.
 *
 * @return Current steady-state. Returns `false` when @p ctx is NULL.
 */
static inline bool debounce_get_state(const struct debounce *ctx)
{
    return (ctx != NULL) ? ctx->state : false;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
