/**
 * @file    tm_task_budget_probe.h
 * @brief   Shared RTOS heap/stack budget probe helper
 * @version v1.0.0
 * @date    2026.04.14
 *
 * Design overview
 * ---------------
 * `tm_task_budget_probe` is a shared runtime helper for coarse RTOS
 * heap/stack diagnostics. Task owners register their thread handle and stack
 * budget once, while shell or UART diagnostic surfaces consume one consistent
 * read-only snapshot contract.
 *
 * Usage example
 * -------------
 * tm_task_budget_probe_register("input", input_thread, input_stack_bytes);
 *
 * if (tm_task_budget_probe_snapshot(&snapshot)) {
 *     consume_budget_snapshot(&snapshot);
 * }
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Registration is performed by task owners after thread creation.
 * - Snapshot APIs are read-only helpers for shell/diag surfaces.
 * - This helper owns no product policy and must not grow product vocabulary.
 *
 * Blocking / resources / diagnostics
 * ----------------------------------
 * - Registration and snapshot capture stay bounded and heap-free.
 * - Snapshot helpers are read-only views over one shared static registration
 *   table.
 * - Evidence is intentionally compact: `bool` / `size_t` returns plus the
 *   copied snapshot/format buffer contents.
 *
 * Failure / recovery
 * ------------------
 * - Invalid registration inputs are ignored instead of creating partial table
 *   state; duplicate task/name registration updates the existing slot.
 * - Invalid snapshot requests fail with `false`; this helper owns no retry
 *   loop or transport recovery.
 * - Registration capacity pressure is bounded by
 *   `TM_TASK_BUDGET_PROBE_MAX_TASKS`; caller task owners remain responsible
 *   for keeping the observed task set intentional.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cmsis_os2.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum registered task owners captured in one shared budget snapshot. */
#define TM_TASK_BUDGET_PROBE_MAX_TASKS (8U)

struct tm_task_budget_snapshot_task {
    /** Borrowed stable task label copied from registration. */
    const char *name;
    /** Caller-declared total stack budget for the registered task. */
    uint32_t    stack_size_bytes;
    /** Current free high-water mark reported for that task stack. */
    uint32_t    stack_high_water_free_bytes;
};

struct tm_task_budget_snapshot {
    /** Minimum-ever free heap observed by the active RTOS heap backend. */
    size_t                              heap_min_ever_free_bytes;
    /** Number of valid entries currently present in `tasks[]`. */
    uint8_t                             task_count;
    /** Per-task stack budget evidence in registration order. */
    struct tm_task_budget_snapshot_task tasks[TM_TASK_BUDGET_PROBE_MAX_TASKS];
};

/**
 * @brief Register or refresh one task owner entry for later budget reporting.
 *
 * @param  name              Borrowed stable task label, or `NULL` to ignore
 *                           the request.
 * @param  thread_id         RTOS thread handle owned by the registering task.
 * @param  stack_size_bytes  Declared total stack budget for that task.
 *
 * @note Duplicate registrations update the existing slot instead of creating a
 *       second entry.
 */
void   tm_task_budget_probe_register(const char  *name,
                                     osThreadId_t thread_id,
                                     uint32_t     stack_size_bytes);

/**
 * @brief Copy one read-only heap/stack budget snapshot across registered tasks.
 *
 * @param  snapshot  Caller-owned output structure.
 *
 * @return `true` when the snapshot was captured successfully; otherwise
 *         `false`.
 */
bool   tm_task_budget_probe_snapshot(struct tm_task_budget_snapshot *snapshot);
#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
