/**
 * @file    tm_log.h
 * @brief   Themis Logging System - Zero Overhead & Standardized
 * @version v5.1.1
 * @date    2026.01.08
 *
 * Design overview
 * ---------------
 * `tm_log` is the shared formatted runtime log contract. It keeps the logging
 * frontend heap-free and deliberately small: call sites emit through macros,
 * the implementation formats into one bounded stack buffer, and the caller
 * installs the actual transport backend through a borrowed write callback.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - `tm_log_init()` copies runtime config by value and may prime runtime
 *   locking once the RTOS is available.
 * - The transport backend remains caller-owned and borrowed.
 * - Public logging APIs are task-context only; ISR calls are intentionally
 *   dropped before backend work starts.
 *
 * Blocking / failure
 * ------------------
 * - The shared contract is drop-on-busy by design; it prefers bounded stack
 *   and zero heap over lossless queuing.
 * - When locking is enabled, the runtime mutex only serializes the backend
 *   write phase. It does not turn the frontend into a queued transport layer.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>
#include "config/tm_core_config.h"
#include "tmdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/* Project-level logging configuration is owned by tm_core_config.h. */
#if !defined(TM_LOG_ENABLE) || !defined(TM_LOG_COLOR_ENABLE) || !defined(TM_LOG_LOCK_ENABLE) || \
    !defined(TM_LOG_TIMESTAMP_ENABLE) || !defined(TM_LOG_HEADER_ENABLE) ||                      \
    !defined(TM_LOG_BUFFER_SIZE) || !defined(TM_LOG_LEVEL_DEFAULT) ||                           \
    !defined(TM_LOG_LEVEL_COMPILED_MAX) || !defined(TM_ASSERT_ENABLE)
#error "Log configuration must be provided by tm_core_config.h"
#endif

/**
 * @brief  Define module tag
 * @usage  Put LOG_TAG("MAIN") at top of .c file
 */
#if TM_LOG_ENABLE || TM_ASSERT_ENABLE
#define LOG_TAG(_tag) static const char *const TAG = (_tag)
#else
/*
 * When both logging and asserts are compiled out, LOG_TAG() must still expand
 * to a valid file-scope declaration so existing modules do not need local
 * `#if` guards around their tag line.
 */
#define LOG_TAG(_tag)                                  \
    enum {                                             \
        TM_CONCAT(_tm_log_tag_disabled_, __LINE__) = 0 \
    }
#endif

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Log levels
 */
enum tm_log_level {
    TM_LOG_LEVEL_NONE = 0,
    TM_LOG_LEVEL_ERROR,
    TM_LOG_LEVEL_WARNING,
    TM_LOG_LEVEL_INFO,
    TM_LOG_LEVEL_DEBUG,
    TM_LOG_LEVEL_MAX
};

/**
 * @brief  Backend write function prototype
 *
 * Task-context only. ISR calls are silently dropped before reaching the
 * backend.
 *
 * Concurrency model - drop-on-busy:
 * The log frontend uses a short IRQ-safe gate (not a queuing mutex) to guard
 * the stack buffer and backend write for the duration of each output call.
 * While one task is actively formatting or writing, any concurrent call from
 * another task - or a recursive call triggered from within the backend - is
 * dropped silently. This design choice prioritises low stack usage and zero
 * heap over lossless delivery. The backend must therefore:
 *   - Never call log macros itself (would be dropped by the reentry gate).
 *   - Remain short and non-blocking so the gate is held for the minimum time.
 *
 * When TM_LOG_LOCK_ENABLE is enabled, a CMSIS-RTOS2 recursive mutex is
 * additionally used to serialise the backend write across task contexts once
 * the RTOS scheduler is running. The mutex is primed by tm_log_init() or
 * lazily created on the first task-context output after kernel init. The gate
 * still fires before the mutex acquisition, so the mutex only serialises the
 * write phase and does not queue callers blocked by formatting in progress.
 * If mutex acquisition fails, that log line is dropped instead of being
 * emitted outside the stated serialization contract.
 *
 * @param  data  Pointer to formatted data to send
 * @param  len   Length of data in bytes
 */
typedef void (*tm_log_write_fn)(const char *data, uint16_t len);

/**
 * @brief Configuration structure
 */
struct tm_log_config {
    /** Backend output function. */
    tm_log_write_fn write;
};

/* Public Functions ----------------------------------------------------------------------------- */

#if TM_LOG_ENABLE

/**
 * @brief  Initialize logging configuration and prime runtime locking
 *
 * @param  config Pointer to configuration copied by value
 */
void tm_log_init(const struct tm_log_config *config);

/**
 * @brief  Set runtime log level
 *
 * @param  level Target log level
 *
 * @note   Invalid values are clamped back to `TM_LOG_LEVEL_DEFAULT`.
 */
void tm_log_set_level(enum tm_log_level level);

/**
 * @brief  Get current log level
 *
 * @return Current log level
 */
enum tm_log_level tm_log_get_level(void);

/**
 * @brief  Internal output function (Do not call directly)
 *
 * Task-context only. ISR calls are dropped before any work is done.
 *
 * A concurrent or recursive call that arrives while a previous call is still
 * active on this CPU is also dropped (see drop-on-busy note in
 * tm_log_write_fn above). The formatted output for such a call is lost
 * silently - no error is returned and no queuing occurs.
 */
void _tm_log_output(const char *tag, uint32_t line, enum tm_log_level level, const char *fmt, ...);

/* Macro API - Zero overhead if level disabled */
#define _TM_LOG_LEVEL_IS_COMPILED(_level) ((int)(_level) <= (int)(TM_LOG_LEVEL_COMPILED_MAX))

#define _LOG_IMPL(level, ...)                                      \
    do {                                                           \
        if (_TM_LOG_LEVEL_IS_COMPILED(level)) {                    \
            if ((level) <= tm_log_get_level()) {                   \
                _tm_log_output(TAG, __LINE__, level, __VA_ARGS__); \
            }                                                      \
        }                                                          \
    } while (0)

#define log_error(...) _LOG_IMPL(TM_LOG_LEVEL_ERROR, __VA_ARGS__)
#define log_warn(...)  _LOG_IMPL(TM_LOG_LEVEL_WARNING, __VA_ARGS__)
#define log_info(...)  _LOG_IMPL(TM_LOG_LEVEL_INFO, __VA_ARGS__)
#define log_debug(...) _LOG_IMPL(TM_LOG_LEVEL_DEBUG, __VA_ARGS__)

#else /* TM_LOG_ENABLE */

static inline void _tm_log_disabled_sink(const char *fmt, ...)
{
    (void)fmt;
}

#define tm_log_init(cfg) \
    do {                 \
        (void)(cfg);     \
    } while (0)
#define tm_log_set_level(l) \
    do {                    \
        (void)(l);          \
    } while (0)
#define tm_log_get_level() TM_LOG_LEVEL_NONE
#define log_error(...)                          \
    do {                                        \
        if (0) {                                \
            _tm_log_disabled_sink(__VA_ARGS__); \
        }                                       \
    } while (0)
#define log_warn(...)                           \
    do {                                        \
        if (0) {                                \
            _tm_log_disabled_sink(__VA_ARGS__); \
        }                                       \
    } while (0)
#define log_info(...)                           \
    do {                                        \
        if (0) {                                \
            _tm_log_disabled_sink(__VA_ARGS__); \
        }                                       \
    } while (0)
#define log_debug(...)                          \
    do {                                        \
        if (0) {                                \
            _tm_log_disabled_sink(__VA_ARGS__); \
        }                                       \
    } while (0)

#endif /* TM_LOG_ENABLE */

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
