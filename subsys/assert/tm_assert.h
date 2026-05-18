/**
 * @file    tm_assert.h
 * @brief   Themis assertion reporting and fatal-fault escalation contract
 * @version v5.1.0
 * @date    2026.03.28
 *
 * Design overview
 * ---------------
 * Themis assertions are intentionally lightweight:
 *
 * - frontend macros capture file-local tag, line, function, condition, and
 *   optional message/id at the call site
 * - the backend output path streams small string fragments directly to the
 *   registered writer instead of assembling a large heap/stack buffer
 * - fatal asserts hand off to a caller-provided halt/reset hook and never
 *   return to the normal control path
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - `tm_assert_init()` copies `struct tm_assert_config` by value.
 * - `tm_assert_register_map()` borrows the caller-owned error-map table; it is
 *   not copied and must outlive future assert reports.
 * - `_tm_assert_failed()` may execute in crash-oriented or degraded contexts,
 *   so backend hooks must stay short, avoid heap allocation, and avoid
 *   indefinitely blocking waits.
 * - Assertion macros use the same file-local `TAG` symbol convention as the log
 *   macros. Translation units using `tm_assert(...)` should therefore provide a
 *   tag symbol, typically via `LOG_TAG("mod")`.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config/tm_core_config.h"

/*
 * cmsis_os2.h is intentionally NOT included here.
 * The IRQ critical section helpers used by the implementation are pulled in
 * from tm_runtime.h in the .c file only; they are an implementation detail
 * and must not leak into the public assert contract.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/* Project-level assert configuration is owned by tm_core_config.h. */
#if !defined(TM_ASSERT_ENABLE) || !defined(TM_ASSERT_USE_ERROR_MAP)
#error "Assert configuration must be provided by tm_core_config.h"
#endif

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Assertion severity levels.
 */
enum tm_assert_level {
    /** System halts after reporting the fault. */
    TM_ASSERT_LEVEL_FATAL = 0,
    /** Error is reported and execution continues. */
    TM_ASSERT_LEVEL_ERROR,
    /** Warning is reported and execution continues. */
    TM_ASSERT_LEVEL_WARNING,
    TM_ASSERT_LEVEL_MAX
};

/**
 * @brief One error-id to string mapping entry.
 */
struct tm_assert_error_map {
    /** Numeric error identifier used by `tm_assert_id()`. */
    uint16_t error_id;
    /** Borrowed NUL-terminated text shown in diagnostics. */
    const char *error_str;
};

/**
 * @brief Backend output hook used by the assert formatter.
 *
 * @note  Must be safe to call from fatal/error-reporting context.
 *        The backend should stay short and must not block indefinitely.
 */
typedef void (*tm_assert_write_fn)(const char *data, uint16_t len);

/**
 * @brief System halt hook invoked when a fatal assert fires.
 *
 * @note  The preferred implementation resets the board or enters a final panic
 *        loop. If the handler returns, the assertion core falls back to an
 *        infinite loop so control never silently resumes.
 */
typedef void (*tm_assert_handler_fn)(void);

/**
 * @brief Runtime configuration copied by tm_assert_init().
 */
struct tm_assert_config {
    /** Optional streaming output backend. */
    tm_assert_write_fn write;
    /** Optional fatal halt/reset hook. */
    tm_assert_handler_fn handler;
};

/* Public Functions ----------------------------------------------------------------------------- */

#if TM_ASSERT_ENABLE

/**
 * @brief  Install the runtime assertion backend configuration.
 *
 * @param  config  Optional configuration pointer copied by value.
 */
void tm_assert_init(const struct tm_assert_config *config);

#if TM_ASSERT_USE_ERROR_MAP
/**
 * @brief  Install one borrowed error-id mapping table.
 *
 * @param  map    Caller-owned array of mapping entries.
 * @param  count  Number of valid entries in @p map.
 */
void tm_assert_register_map(const struct tm_assert_error_map *map, size_t count);
#endif

/**
 * @brief  Low-level assertion backend invoked by the frontend macros.
 *
 * @note   Do not call directly from application code. Use the macro frontend so
 *         line numbers, conditions, and file-local tags stay correct.
 */
void _tm_assert_failed(const char          *tag,
                       uint32_t             line,
                       enum tm_assert_level level,
                       const char          *func,
                       uint32_t             id,
                       const char          *msg,
                       const char          *cond);

/* Macro Frontend ------------------------------------------------------------------------------- */

#define _ASSERT_IMPL(cond, level, id, msg)                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            _tm_assert_failed(TAG, __LINE__, level, __func__, id, msg, #cond); \
        }                                                                      \
    } while (0)

/** @brief Fatal assertion with no explicit message or error-id. */
#define tm_assert(cond)            _ASSERT_IMPL(cond, TM_ASSERT_LEVEL_FATAL, 0, NULL)

/** @brief Assertion with an explicit severity level. */
#define tm_assert_level(cond, lvl) _ASSERT_IMPL(cond, lvl, 0, NULL)

/** @brief Fatal assertion carrying one error-id looked up through the error map. */
#define tm_assert_id(cond, id)     _ASSERT_IMPL(cond, TM_ASSERT_LEVEL_FATAL, id, NULL)

/** @brief Fatal assertion carrying one caller-supplied message string. */
#define tm_assert_msg(cond, msg)   _ASSERT_IMPL(cond, TM_ASSERT_LEVEL_FATAL, 0, msg)

/** @brief Convenience fatal assertion for pointer-not-NULL checks. */
#define tm_assert_ptr(ptr)         _ASSERT_IMPL((ptr) != NULL, TM_ASSERT_LEVEL_FATAL, 0, #ptr " != NULL")

#else /* TM_ASSERT_ENABLE */

#define _TM_ASSERT_DISABLED_USE(_expr) \
    do {                               \
        if (0) {                       \
            (void)(_expr);             \
        }                              \
    } while (0)

#define tm_assert_init(cfg) \
    do {                    \
        (void)(cfg);        \
    } while (0)
#define tm_assert_register_map(m, c) \
    do {                             \
        (void)(m);                   \
        (void)(c);                   \
    } while (0)
#define tm_assert(cond) _TM_ASSERT_DISABLED_USE(cond)
#define tm_assert_level(cond, lvl)     \
    do {                               \
        _TM_ASSERT_DISABLED_USE(cond); \
        (void)(lvl);                   \
    } while (0)
#define tm_assert_id(cond, id)         \
    do {                               \
        _TM_ASSERT_DISABLED_USE(cond); \
        (void)(id);                    \
    } while (0)
#define tm_assert_msg(cond, msg)       \
    do {                               \
        _TM_ASSERT_DISABLED_USE(cond); \
        (void)(msg);                   \
    } while (0)
#define tm_assert_ptr(ptr) _TM_ASSERT_DISABLED_USE((ptr) != NULL)

#endif /* TM_ASSERT_ENABLE */

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
