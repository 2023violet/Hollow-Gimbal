/**
 * @file    tm_runtime.h
 * @brief   Themis runtime context, tick, heap, and critical-section bridge
 * @version v1.2.1
 * @date    2026.03.31
 *
 * Design overview
 * ---------------
 * This module is the thin runtime adapter shared by upper framework layers.
 * It exposes a small, stable contract for:
 *
 * - current execution context detection (ISR vs task)
 * - scheduler/tick queries
 * - task-level and IRQ-level critical sections
 * - heap allocation wrappers that refuse ISR usage
 *
 * The goal is not to hide FreeRTOS completely, but to stop `subsys/`,
 * `device/`, and `drivers/` from open-coding scheduler-state and critical
 * section checks in every module.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - This module owns no dynamic state and requires no explicit init.
 * - Tick helpers return `0` before the scheduler starts, so pre-kernel code can
 *   probe them safely without accidentally touching RTOS APIs in an invalid
 *   state.
 * - Heap helpers are task-context only. ISR callers get `NULL` or a no-op so
 *   the project never touches the FreeRTOS heap from interrupt context.
 * - `tm_task_critical_*()` only gates task scheduling; it is a no-op from ISR
 *   or before the scheduler starts.
 * - `tm_irq_critical_enter()` / `tm_irq_critical_exit()` are the symmetric
 *   cross-context interrupt critical-section pair and must be used as a matched
 *   enter/exit sequence on the same call path.
 *
 * Usage example
 * -------------
 * uint32_t last_tick = tm_tick_get();
 *
 * for (;;) {
 *     uint32_t elapsed_ms = tm_tick_elapsed_ms_update(&last_tick);
 *
 *     service_process(elapsed_ms);
 * }
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "config/tm_core_config.h"
#include "tmdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Detect whether the current CPU context is an interrupt handler.
 *
 * @return `true` when the caller is running in ISR/exception context;
 *         otherwise `false`.
 */
bool tm_is_in_isr(void);

/**
 * @brief  Report whether the FreeRTOS scheduler has started.
 *
 * @return `true` when the scheduler is initialized enough to serve task APIs;
 *         otherwise `false`.
 */
bool tm_scheduler_is_running(void);

/**
 * @brief  Get the current RTOS tick count.
 *
 * Uses the ISR-safe or task-safe backend automatically based on the current
 * context. Returns `0` before the scheduler starts.
 */
uint32_t tm_tick_get(void);

/**
 * @brief  Get the current uptime tick converted to milliseconds.
 *
 * Returns `0` before the scheduler starts. Conversion uses the active RTOS tick
 * period and is intentionally integer-only.
 */
uint32_t tm_tick_get_ms(void);

/**
 * @brief  Report the configured RTOS tick frequency in hertz.
 *
 * This is the single runtime-owned truth that bridges `TM_TICK_PER_SECOND`
 * with the active RTOS tick source.
 */
uint32_t tm_tick_hz_get(void);

/**
 * @brief  Convert milliseconds to RTOS ticks using ceiling rounding.
 *
 * Zero milliseconds returns zero ticks. Non-zero input returns at least one
 * tick when the configured tick source is valid.
 */
uint32_t tm_ms_to_ticks_ceil(uint32_t milliseconds);

/**
 * @brief  Convert RTOS ticks to milliseconds using integer-only scaling.
 *
 * Zero ticks returns zero milliseconds. Non-zero ticks return at least one
 * millisecond so elapsed time does not silently collapse to zero on fast tick
 * configurations.
 */
uint32_t tm_ticks_to_ms(uint32_t ticks);

/**
 * @brief  Convert tick delta since `*last_tick` to elapsed milliseconds and refresh the baseline.
 *
 * This helper centralizes the common "current tick - previous tick" pattern
 * used by owner tasks that need a monotonic elapsed-ms budget for service
 * processing. When a non-zero tick delta would otherwise round down to zero
 * milliseconds, the helper returns `1` to preserve forward progress.
 *
 * @param  last_tick  In/out baseline tick pointer.
 * @return Elapsed milliseconds since the previous baseline, or `0` on NULL input.
 */
uint32_t tm_tick_elapsed_ms_update(uint32_t *last_tick);

/**
 * @brief  Enter a task-level critical section by suspending scheduling.
 *
 * This helper is for short task-context regions that must not be preempted by
 * other tasks but do not need to mask interrupts.
 *
 * No-op in ISR context or before the scheduler starts.
 */
void tm_task_critical_enter(void);

/**
 * @brief  Exit a task-level critical section entered by tm_task_critical_enter().
 *
 * No-op in ISR context or before the scheduler starts.
 */
void tm_task_critical_exit(void);

/**
 * @brief  Enter an interrupt critical section in either task or ISR context.
 *
 * @return Context token required by tm_irq_critical_exit(). The token is only
 *         meaningful when the call entered from ISR context; task-context calls
 *         return `0`.
 */
uint32_t tm_irq_critical_enter(void);

/**
 * @brief  Exit a critical section previously entered by tm_irq_critical_enter().
 *
 * @param  level  Token returned by tm_irq_critical_enter().
 */
void tm_irq_critical_exit(uint32_t level);

/**
 * @brief  Allocate one heap block from the FreeRTOS heap.
 *
 * Task-context only. ISR callers receive `NULL`.
 *
 * @param  size  Allocation size in bytes.
 * @return Heap pointer on success, otherwise `NULL`.
 */
void *tm_malloc(uint32_t size);

/**
 * @brief  Free one heap block previously returned by tm_malloc()/tm_calloc().
 *
 * Task-context only. Passing `NULL` is allowed. ISR callers are ignored to
 * preserve the "never free from interrupt context" contract.
 */
void tm_free(void *memory);

/**
 * @brief  Allocate and zero-fill an array from the FreeRTOS heap.
 *
 * Task-context only. Returns `NULL` on zero arguments, overflow, allocation
 * failure, or ISR usage.
 */
void *tm_calloc(uint32_t n, uint32_t size);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
