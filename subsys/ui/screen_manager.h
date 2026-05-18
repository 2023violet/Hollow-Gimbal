/**
 * @file    screen_manager.h
 * @brief   Shared Screen Stack navigation core
 * @version v1.0.0
 * @date    2026.04.14
 *
 * Design overview
 * ---------------
 * `screen_manager` provides the small shared navigation core behind Screen
 * Stack. It only manages stack topology and lifecycle callbacks; it does not
 * know page semantics, display transport details, or RTOS concerns.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The manager borrows caller-owned static `struct screen` descriptors.
 * - It owns only stack ordering and callback sequencing.
 * - Product pages remain responsible for local state and page-specific
 *   semantics.
 * - Public APIs are task-only / ISR-forbidden and rely on one external owner
 *   path supplied by the caller; the manager has no internal mutex or queue.
 *
 * Blocking / resources / diagnostics
 * ----------------------------------
 * - The manager itself never allocates and never blocks on RTOS primitives.
 * - Lifecycle, key, draw, and wakeup callbacks execute inline, so any
 *   blocking cost belongs to the caller-owned page hook rather than to the
 *   shared manager.
 * - Failure evidence is intentionally small and explicit:
 *   `-errno` for contract violations / missing active page, `SCREEN_*`
 *   pass-through for key dispatch, and `UINT32_MAX` for "no timed wakeup".
 *
 * Failure / recovery
 * ------------------
 * - Contract violations are surfaced immediately through explicit return
 *   values; the manager does not hide retries or fallback navigation policy.
 * - Lifecycle or page-local recovery remains owned by the caller-provided
 *   screen hooks and their surrounding services.
 * - Resetting navigation state is an explicit owner action via
 *   `screen_manager_init()` / `screen_manager_reset()`, not an implicit
 *   self-healing side effect.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "screen.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

#if !defined(SCREEN_STACK_DEPTH)
#define SCREEN_STACK_DEPTH (8U)
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief Reset the manager to an empty stack and clear navigation diagnostics.
 *
 * @note Role: init-only / owner-only.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: heap-free and non-blocking.
 */
int screen_manager_init(void);

/**
 * @brief Push one caller-borrowed screen onto the navigation stack.
 *
 * @note Role: owner-only.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: heap-free and non-blocking in the manager itself;
 *        `on_enter()` executes inline on the caller path.
 * @retval 0         Screen was pushed and `on_enter()` was sequenced.
 * @retval -EINVAL   @p screen was `NULL`.
 * @retval -ENOSPC   Stack depth already reached `SCREEN_STACK_DEPTH`.
 */
int screen_manager_push(struct screen *screen);

/**
 * @brief Pop the current screen when the stack depth is greater than one.
 *
 * @note Role: owner-only.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: heap-free; `on_exit()` and parent `on_resume()`
 *        execute inline on the caller path.
 * @return `0` when the pop completed, or when the request was a bounded no-op
 *         on a root-only stack.
 */
int screen_manager_pop(void);

/**
 * @brief Replace the current top screen without disturbing lower history.
 *
 * @note Role: owner-only.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: heap-free; lifecycle callbacks execute inline.
 * @retval 0         Replacement succeeded, or the target was already current.
 * @retval -EINVAL   @p screen was `NULL`.
 */
int screen_manager_replace(struct screen *screen);

/**
 * @brief Collapse the whole stack to one root screen.
 *
 * @note Role: owner-only.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: heap-free; each stacked screen exits inline before
 *        the new root enters.
 * @retval 0         Reset succeeded, or the requested root was already the
 *         sole active page.
 * @retval -EINVAL   @p screen was `NULL`.
 */
int screen_manager_reset(struct screen *screen);

/**
 * @brief Forward one physical key event to the current top screen.
 *
 * @note Role: owner-only input dispatch.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: heap-free in the manager; `on_key()` executes
 *        inline on the caller path.
 * @retval SCREEN_HANDLED  Event was consumed without stack pop, including
 *         bounded root-only pop requests.
 * @retval SCREEN_POP      Event requested pop and the manager already applied it.
 * @retval SCREEN_IGNORED  Active page declined the event.
 * @retval -EINVAL         @p event was `NULL`.
 * @retval -ENODEV         No active page or current page has no key hook.
 */
int screen_manager_dispatch_key(const struct screen_event *event);

/**
 * @brief Ask the current top screen to render into the caller-owned canvas.
 *
 * @note Role: owner-only render dispatch.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: heap-free in the manager; `on_draw()` executes
 *        inline on the caller-owned render path.
 * @retval 0         Draw callback was invoked.
 * @retval -EINVAL   @p canvas was `NULL`.
 * @retval -ENODEV   No active page or current page has no draw hook.
 */
int screen_manager_draw(struct mono_canvas *canvas);

/**
 * @brief Read the current page's desired timed-wakeup interval.
 *
 * @note Role: state-only timing query.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: read-only, bounded, and allocation-free.
 * @return Milliseconds until owner-side wakeup, or `UINT32_MAX` when the
 *         active page has no timed wakeup contract.
 */
uint32_t screen_manager_next_wakeup_ms(void);

/**
 * @brief Let the current screen consume one owner-side wakeup tick.
 *
 * @note Role: owner-only timed dispatch.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: heap-free in the manager; `on_wakeup()` executes
 *        inline on the caller-owned timed path.
 * @return `0` when no active timed callback exists or when the callback
 *         returns 0; negative `-errno` when the page wakeup hook fails; or a
 *         positive page-defined result forwarded from the wakeup hook.
 */
int screen_manager_process_wakeup(void);

/**
 * @brief Borrow the current top screen descriptor.
 *
 * @note Role: state-only.
 * @note Context: task-only / ISR-forbidden.
 * @note Ownership: returned pointer is borrowed static storage and must not be
 *        freed or mutated as if it were privately owned.
 * @return Current top screen, or `NULL` when the stack is empty.
 */
struct screen *screen_manager_current(void);

/**
 * @brief Read the monotonic navigation generation counter.
 *
 * @note Role: diag-only / state-only.
 * @note Context: task-only / ISR-forbidden.
 * @note Resources: bounded read with no allocation or locking.
 * @return Generation value bumped after each successful stack mutation.
 */
uint32_t screen_manager_nav_generation_get(void);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
