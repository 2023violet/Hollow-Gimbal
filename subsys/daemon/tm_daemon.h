/**
 * @file    tm_daemon.h
 * @brief   Themis software watchdog based on absolute deadlines
 * @version v3.2.0
 * @date    2026.03.28
 *
 * Design overview
 * ---------------
 * `tm_daemon` is a lightweight software-watchdog layer built on top of the
 * shared runtime tick source. Each daemon tracks an absolute deadline in
 * milliseconds; `tm_daemon_feed()` refreshes that deadline, and
 * `tm_daemon_process()` detects timeout transitions and dispatches the caller's
 * callback in task context.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns every `struct tm_daemon`, `struct tm_daemon_cfg`, and
 *   `struct tm_daemon_list`.
 * - `tm_daemon_init()` borrows `config`; the pointed configuration must stay
 *   valid for the entire registered lifetime of the daemon.
 * - `tm_daemon_feed()` is ISR/task safe and never calls the timeout callback.
 * - List operations and timeout processing are task-context APIs and rely on a
 *   live CMSIS-RTOS2 mutex.
 * - `tm_daemon_list_deinit()` is a final owner operation; the caller must stop
 *   concurrent register/unregister/process traffic before tearing a list down.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cmsis_os2.h"
#include "config/tm_core_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

#if !defined(TM_DAEMON_ENABLE) || !defined(TM_DAEMON_STATS_ENABLE)
#error "Daemon configuration must be provided by tm_core_config.h"
#endif

/**
 * @brief   Maximum number of daemons to process per call.
 * @details This is a trade-off between callback execution time and
 *          system responsiveness.
 */
#ifndef TM_DAEMON_MAX_PROCESS_PER_CALL
#define TM_DAEMON_MAX_PROCESS_PER_CALL 3U
#endif

/**
 * @brief   Maximum wrap-safe daemon timeout in milliseconds.
 * @details The daemon subsystem compares absolute deadlines with signed
 *          subtraction, so timeout windows must stay within INT32_MAX.
 */
#ifndef TM_DAEMON_MAX_TIMEOUT_MS
#define TM_DAEMON_MAX_TIMEOUT_MS ((uint32_t)INT32_MAX)
#endif

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Timeout callback signature.
 *
 * @param  owner  Borrowed opaque pointer from `struct tm_daemon_cfg`.
 */
typedef void (*tm_daemon_cb_t)(void *owner);

/**
 * @brief Caller-owned static/runtime configuration for one daemon.
 */
struct tm_daemon_cfg {
    /** Absolute timeout window in milliseconds. */
    uint32_t timeout_ms;
    /** Timeout callback run by `tm_daemon_process()`. */
    tm_daemon_cb_t callback;
    /** Borrowed callback context pointer. */
    void *owner;
};

/*
 * Stats contract:
 * - feed_count counts every accepted tm_daemon_feed()
 * - timeout_count counts online -> offline transitions detected in process()
 * - last/min remaining reflect how much budget was left before the last feed
 * - max_feed_late_ms records how late a feed arrived after its deadline
 * - last_timeout_tick_ms records the tick of the last timeout transition
 */
struct tm_daemon_stats {
    /** Accepted `tm_daemon_feed()` calls. */
    uint32_t feed_count;
    /** Offline transitions detected in `tm_daemon_process()`. */
    uint32_t timeout_count;
    /** Budget remaining before the last accepted feed. */
    uint32_t last_feed_remaining_ms;
    /** Minimum remaining budget ever observed. */
    uint32_t min_feed_remaining_ms;
    /** Worst lateness observed on a late feed. */
    uint32_t max_feed_late_ms;
    /** Tick timestamp of the last timeout transition. */
    uint32_t last_timeout_tick_ms;
};

/**
 * @brief Runtime daemon state owned by the caller and updated by the subsystem.
 */
struct tm_daemon {
    /** Borrowed configuration pointer. */
    const struct tm_daemon_cfg *config;
    /** Internal singly-linked list node managed by the list owner. */
    struct tm_daemon *next;
    /** Absolute timeout deadline in milliseconds. */
    volatile uint32_t deadline_ms;
    /** `true` once a timeout transition has fired. */
    volatile bool is_offline;
#if TM_DAEMON_STATS_ENABLE
    /** Optional observability snapshot storage. */
    struct tm_daemon_stats stats;
#endif
};

/*
 * List owner contract:
 * - register / unregister / process operate on a caller-owned list object.
 * - the list object owns the mutex that serializes list topology updates.
 * - daemon callbacks run after the list mutex is released.
 * - list init / register / unregister / process are task-context APIs and
 *   require CMSIS-RTOS2 to be ready.
 *
 * Zero-init requirement:
 * - A `struct tm_daemon_list` instance MUST be zero-initialized (e.g. via
 *   static storage, `= {0}`, or `memset`) before the first call to
 *   `tm_daemon_list_init()`. The re-init guard inside `tm_daemon_list_init()`
 *   checks `list->mutex != NULL` to detect an already-initialised list; if
 *   the struct is not zero-initialised, a garbage non-NULL `mutex` field will
 *   cause the init to incorrectly return `-EALREADY`.
 *
 * Example (static storage - preferred):
 *   static struct tm_daemon_list g_daemon_list;   // zero by default
 *   tm_daemon_list_init(&g_daemon_list);
 *
 * Example (local / heap storage - explicit zero required):
 *   struct tm_daemon_list list = {0};
 *   tm_daemon_list_init(&list);
 */
struct tm_daemon_list {
    /** Internal head of the registered daemon list. */
    struct tm_daemon *head;
    /** Caller-owned list mutex created at init. */
    osMutexId_t mutex;
};

/* Public Functions ----------------------------------------------------------------------------- */

#if TM_DAEMON_ENABLE

/* Lifecycle ------------------------------------------------------------------------------------ */

/*
 * `tm_daemon_list_init()` is a one-shot initializer for one list object.
 * Re-initializing an already live list returns `-EALREADY` so callers do not
 * silently leak the list mutex.
 *
 * `tm_daemon_init()` is expected to prepare a valid daemon instance before
 * register. Invalid configs are downgraded to an offline / inert daemon and
 * `tm_daemon_register()` will reject them with `-EINVAL`.
 */

int tm_daemon_list_init(struct tm_daemon_list *list);

/**
 * @brief  Tear down one daemon list after all users have quiesced.
 *
 * @param  list  Caller-owned list object previously initialized once.
 */
void tm_daemon_list_deinit(struct tm_daemon_list *list);

/**
 * @brief  Initialize one daemon instance from a caller-owned configuration.
 *
 * Invalid or out-of-range configurations are downgraded to an inert offline
 * daemon so later registration can reject them deterministically.
 */
void tm_daemon_init(struct tm_daemon *daemon, const struct tm_daemon_cfg *config);

/* Registration / topology ---------------------------------------------------------------------- */

/**
 * @brief  Attach one daemon to a caller-owned daemon list.
 */
int tm_daemon_register(struct tm_daemon_list *list, struct tm_daemon *daemon);

/**
 * @brief  Detach one daemon from its list without destroying the daemon object.
 */
int tm_daemon_unregister(struct tm_daemon_list *list, struct tm_daemon *daemon);

/* Runtime -------------------------------------------------------------------------------------- */

/**
 * @brief  Refresh one daemon deadline to `now + timeout_ms`.
 *
 * Safe in task or ISR context.
 */
void tm_daemon_feed(struct tm_daemon *daemon);

/**
 * @brief  Query whether the daemon is currently considered online.
 */
bool tm_daemon_is_online(const struct tm_daemon *daemon);

/**
 * @brief  Query the remaining time budget before timeout.
 */
uint32_t tm_daemon_timeout_remaining_ms(const struct tm_daemon *daemon);

/* Diagnostics ---------------------------------------------------------------------------------- */

/*
 * Stats APIs are task/ISR safe snapshots backed by the same short critical
 * section model as tm_daemon_feed(). When TM_DAEMON_STATS_ENABLE == 0 they
 * return -ENOSYS so call sites can detect that observability was compiled out.
 */
int tm_daemon_stats_get(const struct tm_daemon *daemon, struct tm_daemon_stats *stats);
int tm_daemon_stats_reset(struct tm_daemon *daemon);
/**
 * Process expired daemons.
 *
 * Notes:
 * - This processes at most TM_DAEMON_MAX_PROCESS_PER_CALL timeouts per call,
 *   so a burst of many expired daemons won't block the scheduler for an
 *   unbounded time.
 * - Callbacks must be short and non-blocking because they run with the list
 *   mutex released but in the same task context.
 * - The caller remains the owner of cadence; this function is a polling hook,
 *   not an internal timer thread.
 */
void tm_daemon_process(struct tm_daemon_list *list);

#else

#define _TM_DAEMON_DISABLED_ARG(_arg) ((void)sizeof(_arg))

#define tm_daemon_list_init(l) (_TM_DAEMON_DISABLED_ARG(l), (int)(-ENOSYS))
#define tm_daemon_list_deinit(l) \
    do {                         \
        _TM_DAEMON_DISABLED_ARG(l); \
    } while (0)
#define tm_daemon_init(d, c) \
    do {                     \
        _TM_DAEMON_DISABLED_ARG(d); \
        _TM_DAEMON_DISABLED_ARG(c); \
    } while (0)
#define tm_daemon_register(l, d) \
    (_TM_DAEMON_DISABLED_ARG(l), _TM_DAEMON_DISABLED_ARG(d), (int)(-ENOSYS))
#define tm_daemon_unregister(l, d) \
    (_TM_DAEMON_DISABLED_ARG(l), _TM_DAEMON_DISABLED_ARG(d), (int)(-ENOSYS))
#define tm_daemon_feed(d) \
    do {                  \
        _TM_DAEMON_DISABLED_ARG(d); \
    } while (0)
#define tm_daemon_is_online(d)            (_TM_DAEMON_DISABLED_ARG(d), false)
#define tm_daemon_timeout_remaining_ms(d) (_TM_DAEMON_DISABLED_ARG(d), 0U)
#define tm_daemon_stats_get(d, s) \
    (_TM_DAEMON_DISABLED_ARG(d), _TM_DAEMON_DISABLED_ARG(s), (int)(-ENOSYS))
#define tm_daemon_stats_reset(d)          (_TM_DAEMON_DISABLED_ARG(d), (int)(-ENOSYS))
#define tm_daemon_process(l) \
    do {                     \
        _TM_DAEMON_DISABLED_ARG(l); \
    } while (0)

#endif /* TM_DAEMON_ENABLE */

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
