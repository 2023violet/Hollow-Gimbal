/**
 * @file    tm_log.c
 * @brief   Themis Logging System Implementation - Lightweight & Standardized
 * @version v5.1.1
 * @date    2026.01.08
 *
 * Implementation role
 * -------------------
 * This translation unit pairs with `tm_log.h` and owns the bounded stack
 * formatter, drop-on-busy gate, and optional runtime mutex used to keep the
 * shared logging surface deterministic without introducing a queue or heap.
 *
 * Key collaborators
 * -----------------
 * - `tm_format`: bounded formatting backend
 * - `tm_runtime`: IRQ/task critical-section helpers for the reentry gate
 * - caller-installed `tm_log_write_fn`: final transport backend
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_log.h"
#include <stdarg.h>
#include "cmsis_os2.h"
#include "tm_assert.h"
#include "tm_format.h"
#include "tm_runtime.h"

#if TM_LOG_ENABLE

/* ANSI Colors - Only compiled if enabled */
#if TM_LOG_COLOR_ENABLE
#define COLOR_RED    "\033[31m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE   "\033[34m"
#define COLOR_RESET  "\033[0m"
#else
#define COLOR_RED    ""
#define COLOR_GREEN  ""
#define COLOR_YELLOW ""
#define COLOR_BLUE   ""
#define COLOR_RESET  ""
#endif

#define _LOG_LEVEL_ERROR_STR   "E"
#define _LOG_LEVEL_WARNING_STR "W"
#define _LOG_LEVEL_INFO_STR    "I"
#define _LOG_LEVEL_DEBUG_STR   "D"

/* Private State ------------------------------------------------------------------------------- */
/* Why: Config copy allows user to pass stack variable in init (safer) */
static struct tm_log_config       _log_cfg       = {0};
static volatile enum tm_log_level _current_level = TM_LOG_LEVEL_DEFAULT;

/*
 * Drop-on-busy gate:
 * This boolean is set for the entire duration of a log output call (from
 * buffer formatting through backend write). Its purpose is twofold:
 *
 *   1. Prevent recursive logging: if the registered backend write function
 *      somehow triggers another log macro call, the gate ensures the re-entrant
 *      call is dropped immediately without corrupting the stack buffer.
 *
 *   2. Prevent concurrent calls from other tasks from racing on the same stack
 *      buffer before the RTOS mutex has been primed.
 *
 * A side effect of (2) is that, while the mutex is NULL (before kernel start
 * or if mutex creation fails), only one log output can proceed at a time
 * system-wide - concurrent callers are dropped rather than queued.
 * Once the mutex is live (TM_LOG_LOCK_ENABLE), the mutex serialises the
 * write phase across tasks; the gate's role then narrows back to (1) only,
 * because the mutex queues concurrent task callers rather than dropping them.
 * However, the gate still fires before mutex acquisition, so a task that
 * arrives while another is in the formatting phase (before mutex acquire) is
 * still dropped in that window.
 *
 * This is an intentional embedded trade-off: zero heap, bounded stack, no
 * output queue, lossless-only within a single task's call chain.
 */
static volatile bool _log_output_active = false;

#if TM_LOG_LOCK_ENABLE
static osMutexId_t         _log_mutex      = NULL;
static const osMutexAttr_t _log_mutex_attr = {
    .attr_bits = osMutexRecursive | osMutexPrioInherit,
};
#endif

/* Private Helpers ----------------------------------------------------------------------------- */
static const char *_get_color(enum tm_log_level level);
#if TM_LOG_HEADER_ENABLE
static const char *_get_level_char(enum tm_log_level level);
#endif
#if TM_LOG_LOCK_ENABLE
static void _log_mutex_init_once(void);
#endif
static size_t _log_format_capacity(size_t offset);
static char  *_log_format_cursor(char *buffer, size_t offset);
static void   _log_append(char *buffer, size_t *offset, const char *fmt, ...);
static size_t _log_output_len(size_t offset);
static bool   _log_output_enter(void);
static void   _log_output_leave(void);

/* Public Functions ----------------------------------------------------------------------------- */

void tm_log_init(const struct tm_log_config *config)
{
    if (config) {
        _log_cfg = *config;
    }

#if TM_LOG_LOCK_ENABLE
    _log_mutex_init_once();
#endif
}

void tm_log_set_level(enum tm_log_level level)
{
    if (level >= TM_LOG_LEVEL_MAX) {
        level = TM_LOG_LEVEL_DEFAULT;
    }

    _current_level = level;
}

enum tm_log_level tm_log_get_level(void)
{
    if (_current_level >= TM_LOG_LEVEL_MAX) {
        return TM_LOG_LEVEL_DEFAULT;
    }

    return _current_level;
}

#if TM_LOG_LOCK_ENABLE
static void _log_mutex_init_once(void)
{
    osMutexId_t new_mutex;

    if ((_log_mutex != NULL) || (osKernelGetState() == osKernelInactive) || tm_is_in_isr()) {
        return;
    }

    new_mutex = osMutexNew(&_log_mutex_attr);
    if (new_mutex == NULL) {
        return;
    }

    /*
     * Double-check under a task critical section (vTaskSuspendAll) to
     * handle the case where two tasks call _log_mutex_init_once concurrently
     * after the kernel starts. The loser deletes its redundant mutex.
     */
    tm_task_critical_enter();
    if (_log_mutex == NULL) {
        _log_mutex = new_mutex;
        new_mutex  = NULL;
    }
    tm_task_critical_exit();

    if (new_mutex != NULL) {
        (void)osMutexDelete(new_mutex);
    }
}
#endif

void _tm_log_output(const char *tag, uint32_t line, enum tm_log_level level, const char *fmt, ...)
{
    char   buf[TM_LOG_BUFFER_SIZE];
    bool   lock_held = false;
    size_t offset    = 0U;

    if ((_log_cfg.write == NULL) || (fmt == NULL) || tm_is_in_isr()) {
        return;
    }

    /*
     * Acquire the drop-on-busy gate. See the module-level comment on
     * _log_output_active for the full concurrency contract.
     */
    if (!_log_output_enter()) {
        return;
    }

    /* Serialise backend writes across tasks once the RTOS mutex is live. */
#if TM_LOG_LOCK_ENABLE
    _log_mutex_init_once();
    if (_log_mutex != NULL) {
        lock_held = (osMutexAcquire(_log_mutex, osWaitForever) == osOK);
        if (!lock_held) {
            _log_output_leave();
            return;
        }
    }
#endif

    const char *color = _get_color(level);

#if TM_LOG_HEADER_ENABLE
    const char *lvl_char = _get_level_char(level);
    const char *safe_tag = (tag != NULL) ? tag : "SYS";

#if TM_LOG_TIMESTAMP_ENABLE
    uint32_t timestamp = tm_tick_get_ms();

    /* Format: [L/Tag L:Line T:Time]
     * Example: [E/VOFA L:42 T:12345] Error occurred
     * Why: 'ms' is standard engineering unit. 'L:' helps quick jump in IDE.
     */
    _log_append(buf,
                &offset,
                "%s[%s/%s L:%u T:%u] ",
                color,
                lvl_char,
                safe_tag,
                (unsigned int)line,
                (unsigned int)timestamp);
#else
    /* Format: [L/Tag L:Line]
     * Example: [E/VOFA L:42] Error occurred
     */
    _log_append(buf, &offset, "%s[%s/%s L:%u] ", color, lvl_char, safe_tag, (unsigned int)line);
#endif
#else
    (void)tag;
    (void)line;
    _log_append(buf, &offset, "%s", color);
#endif

    /* Content */
    {
        va_list args;
        size_t  written;

        va_start(args, fmt);
        written = tm_format_vsnprintf(_log_format_cursor(buf, offset),
                                      _log_format_capacity(offset),
                                      fmt,
                                      args);
        va_end(args);
        offset += written;
    }

#if TM_LOG_COLOR_ENABLE
    _log_append(buf, &offset, "%s\r\n", COLOR_RESET);
#else
    _log_append(buf, &offset, "\r\n");
#endif

    /* Output via registered backend */
    _log_cfg.write(buf, (uint16_t)_log_output_len(offset));

#if TM_LOG_LOCK_ENABLE
    if (lock_held && (_log_mutex != NULL)) {
        (void)osMutexRelease(_log_mutex);
    }
#endif
    _log_output_leave();
}

/* Private functions ---------------------------------------------------------------------------- */

static const char *_get_color(enum tm_log_level level)
{
#if TM_LOG_COLOR_ENABLE
    switch (level) {
    case TM_LOG_LEVEL_ERROR:
        return COLOR_RED;
    case TM_LOG_LEVEL_WARNING:
        return COLOR_YELLOW;
    case TM_LOG_LEVEL_INFO:
        return COLOR_BLUE;
    case TM_LOG_LEVEL_DEBUG:
        return COLOR_GREEN;
    default:
        return "";
    }
#else
    (void)level;
    return "";
#endif
}

static size_t _log_format_capacity(size_t offset)
{
    if (offset >= (size_t)TM_LOG_BUFFER_SIZE) {
        return 0U;
    }

    return (size_t)TM_LOG_BUFFER_SIZE - offset;
}

static char *_log_format_cursor(char *buffer, size_t offset)
{
    if ((buffer == NULL) || (offset >= (size_t)TM_LOG_BUFFER_SIZE)) {
        return NULL;
    }

    return buffer + offset;
}

static void _log_append(char *buffer, size_t *offset, const char *fmt, ...)
{
    va_list args;
    size_t  written;

    if ((offset == NULL) || (fmt == NULL)) {
        return;
    }

    va_start(args, fmt);
    written = tm_format_vsnprintf(_log_format_cursor(buffer, *offset),
                                  _log_format_capacity(*offset),
                                  fmt,
                                  args);
    va_end(args);

    *offset += written;
}

static size_t _log_output_len(size_t offset)
{
    if (offset >= (size_t)TM_LOG_BUFFER_SIZE) {
        return (size_t)TM_LOG_BUFFER_SIZE - 1U;
    }

    return offset;
}

static bool _log_output_enter(void)
{
    uint32_t level;
    bool     entered = false;

    level = tm_irq_critical_enter();
    if (!_log_output_active) {
        _log_output_active = true;
        entered            = true;
    }
    tm_irq_critical_exit(level);

    return entered;
}

static void _log_output_leave(void)
{
    uint32_t level;

    level              = tm_irq_critical_enter();
    _log_output_active = false;
    tm_irq_critical_exit(level);
}

#if TM_LOG_HEADER_ENABLE
static const char *_get_level_char(enum tm_log_level level)
{
    switch (level) {
    case TM_LOG_LEVEL_ERROR:
        return _LOG_LEVEL_ERROR_STR;
    case TM_LOG_LEVEL_WARNING:
        return _LOG_LEVEL_WARNING_STR;
    case TM_LOG_LEVEL_INFO:
        return _LOG_LEVEL_INFO_STR;
    case TM_LOG_LEVEL_DEBUG:
        return _LOG_LEVEL_DEBUG_STR;
    default:
        return "?";
    }
}
#endif

#endif /* TM_LOG_ENABLE */

/* ---------------------------------------- end of file ----------------------------------------- */
