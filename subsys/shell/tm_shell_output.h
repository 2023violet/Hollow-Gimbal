/**
 * @file    tm_shell_output.h
 * @brief   Shared shell output helpers for Themis summary/detail formatting
 * @version v1.0.0
 * @date    2026.04.14
 *
 * Design overview
 * ---------------
 * This header centralizes the shared upper-layer shell output shape:
 * - one summary line in `[topic] key=value ...` form
 * - zero or more detail rows in `label : key=value ...` form
 * - stable on/off vocabulary for switch-like fields
 *
 * It stays header-only so command surfaces can share one formatting contract
 * without growing a second runtime module or extra host-test link edges.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Upper-layer shell command modules include this header through their local
 *   compatibility wrappers or directly from `subsys/shell/`.
 * - `TM_SHELL_SUMMARY()`, `TM_SHELL_ROW()`, and `TM_SHELL_PROMPT()` are
 *   producer-side formatting helpers only; runtime shell transport still
 *   belongs to the shell task.
 * - Those helpers inherit the shell core contract: task-only,
 *   ISR-forbidden, no extra heap allocation, and no independent transport
 *   ownership.
 * - `tm_shell_onoff_name()` is a read-only vocabulary helper with no mutable
 *   state, no blocking, and no allocation.
 * - Contract growth should happen here instead of reintroducing per-product
 *   formatting helpers.
 *
 * Blocking / resources / diagnostics
 * ----------------------------------
 * - This header stays header-only and adds no queue, heap, or thread owner.
 * - Formatting helpers execute inline on the caller's shell path, so any
 *   transport latency still belongs to the installed shell backend.
 * - Diagnostic evidence for failed writes remains with the shell runtime or
 *   transport owner, not with these macros.
 *
 * Failure / recovery
 * ------------------
 * - These helpers intentionally do not own retry or recovery policy.
 * - If shell transport or prompt emission fails, callers must rely on the
 *   surrounding shell/task diagnostics rather than inventing a second formatter
 *   owner.
 * - Shared vocabulary growth should land here so product wrappers do not drift
 *   into incompatible output dialects.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include "tm_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Emit one shared summary line through the installed shell backend.
 *
 * @note Role: producer-side formatting helper.
 * @note Context: task-only / ISR-forbidden because `shell_printf()` follows
 *        the shared shell runtime contract.
 * @note Blocking/resources: this macro adds no heap use or persistent state;
 *        transport latency remains owned by the installed shell backend.
 * @note Errors/evidence: no direct return code is available here; transport
 *        failures must still be diagnosed through the caller's shell path.
 */
#define TM_SHELL_SUMMARY(topic, fmt, ...) shell_printf("\r\n[%s] " fmt "\r\n", topic, ##__VA_ARGS__)
/**
 * @brief Emit one shared detail row through the installed shell backend.
 *
 * @note Role: producer-side formatting helper.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: no heap allocation or hidden state is added by
 *        this helper; transport cost stays on the shell backend.
 */
#define TM_SHELL_ROW(label, fmt, ...)     shell_printf("  %-7s: " fmt "\r\n", label, ##__VA_ARGS__)
/**
 * @brief Re-emit the shared shell prompt.
 *
 * @note Role: producer-side formatting helper.
 * @note Context: task-only / ISR-forbidden.
 * @note Errors/evidence: prompt emission is fire-and-forget and relies on the
 *        current shell transport contract for any downstream evidence.
 */
#define TM_SHELL_PROMPT()                 shell_print(SHELL_PROMPT)

/**
 * @brief Return the shared on/off vocabulary used by APP shell surfaces.
 *
 * @note Role: state-only vocabulary helper.
 * @note Context: task/ISR agnostic and non-blocking.
 * @note Resources: no allocation and no mutable state.
 */
static inline const char *tm_shell_onoff_name(bool enabled)
{
    return enabled ? "on" : "off";
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
