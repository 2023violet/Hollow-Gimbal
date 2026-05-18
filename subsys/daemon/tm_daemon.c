/**
 * @file    tm_daemon.c
 * @brief   Themis software watchdog based on absolute deadlines
 * @version v3.2.0
 * @date    2026.03.28
 *
 * Implementation role
 * -------------------
 * The daemon core intentionally splits fast-path heartbeat refresh from
 * timeout-callback dispatch:
 * - `tm_daemon_feed()` only touches deadline/offline state under a short IRQ
 *   critical section, so high-frequency producers can call it cheaply
 * - `tm_daemon_process()` runs in task context, walks the list under a mutex,
 *   and invokes callbacks only after the mutex is released
 *
 * Key collaborators
 * -----------------
 * - `tm_daemon.h`: public watchdog/list contract
 * - `tm_runtime`: tick source plus IRQ/task context helpers
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_daemon.h"
#include <assert.h>
#include "tm_runtime.h"

#if TM_DAEMON_ENABLE

/* Private Constants ---------------------------------------------------------------------------- */

static const osMutexAttr_t _tm_daemon_list_mutex_attr = {
    .attr_bits = osMutexRecursive | osMutexPrioInherit,
};

/* Private Helpers ------------------------------------------------------------------------------ */

static int  _tm_daemon_list_lock(struct tm_daemon_list *list);
static void _tm_daemon_list_unlock(struct tm_daemon_list *list);
static bool _tm_daemon_timeout_valid(uint32_t timeout_ms);
static bool _tm_daemon_config_valid(const struct tm_daemon *daemon);
static void _tm_daemon_stats_clear(struct tm_daemon *daemon);

/* Public Functions ----------------------------------------------------------------------------- */

int tm_daemon_list_init(struct tm_daemon_list *list)
{
    if (list == NULL) {
        return -EINVAL;
    }
    if (tm_is_in_isr()) {
        return -EPERM;
    }
    if (list->mutex != NULL) {
        return -EALREADY;
    }

    list->head  = NULL;
    list->mutex = osMutexNew(&_tm_daemon_list_mutex_attr);
    if (list->mutex == NULL) {
        return -ENOMEM;
    }

    return 0;
}

void tm_daemon_list_deinit(struct tm_daemon_list *list)
{
    struct tm_daemon *current;

    assert(list != NULL);
    if (list == NULL) {
        return;
    }

    assert(list->mutex != NULL);
    if (list->mutex == NULL) {
        return;
    }

    assert(!tm_is_in_isr());
    if (tm_is_in_isr()) {
        return;
    }

    if (_tm_daemon_list_lock(list) != 0) {
        assert(!"tm_daemon_list_deinit lock failed");
        return;
    }

    /* Final owner teardown: detach the whole topology before deleting the mutex. */
    current    = list->head;
    list->head = NULL;
    while (current != NULL) {
        struct tm_daemon *next = current->next;
        current->next          = NULL;
        current                = next;
    }

    _tm_daemon_list_unlock(list);
    osMutexDelete(list->mutex);
    list->mutex = NULL;
}

void tm_daemon_init(struct tm_daemon *daemon, const struct tm_daemon_cfg *config)
{
    assert(daemon != NULL);
    if (daemon == NULL) {
        return;
    }

    _tm_daemon_stats_clear(daemon);

    assert(config != NULL);
    assert((config != NULL) && _tm_daemon_timeout_valid(config->timeout_ms));
    if ((config == NULL) || !_tm_daemon_timeout_valid(config->timeout_ms)) {
        daemon->config      = NULL;
        daemon->next        = NULL;
        daemon->deadline_ms = 0U;
        daemon->is_offline  = true;
        return;
    }

    daemon->config      = config;
    daemon->next        = NULL;
    daemon->deadline_ms = tm_tick_get_ms() + config->timeout_ms;
    daemon->is_offline  = false;
}

int tm_daemon_register(struct tm_daemon_list *list, struct tm_daemon *daemon)
{
    struct tm_daemon *current;

    if ((list == NULL) || (daemon == NULL)) {
        return -EINVAL;
    }
    if (tm_is_in_isr()) {
        return -EPERM;
    }
    if (!_tm_daemon_config_valid(daemon)) {
        return -EINVAL;
    }

    if (_tm_daemon_list_lock(list) != 0) {
        return -ENODEV;
    }

    current = list->head;

    while (current != NULL) {
        if (current == daemon) {
            _tm_daemon_list_unlock(list);
            return 0;
        }
        current = current->next;
    }

    daemon->next = list->head;
    list->head   = daemon;

    _tm_daemon_list_unlock(list);
    return 0;
}

int tm_daemon_unregister(struct tm_daemon_list *list, struct tm_daemon *daemon)
{
    struct tm_daemon **current;

    if ((list == NULL) || (daemon == NULL)) {
        return -EINVAL;
    }
    if (tm_is_in_isr()) {
        return -EPERM;
    }

    if (_tm_daemon_list_lock(list) != 0) {
        return -ENODEV;
    }

    current = &list->head;

    while (*current != NULL) {
        if (*current == daemon) {
            *current     = daemon->next;
            daemon->next = NULL;
            _tm_daemon_list_unlock(list);
            return 0;
        }
        current = &(*current)->next;
    }

    _tm_daemon_list_unlock(list);
    return -ENOENT;
}

void tm_daemon_feed(struct tm_daemon *daemon)
{
    uint32_t now;
    uint32_t level;

    /* tm_daemon_feed is designed to be ISR/task safe via irq critical section.
     * It does not take daemon list mutex so it is suitable for high-frequency
     * heartbeat updates. Callback execution still happens in tm_daemon_process
     * context and callbacks must be short and non-blocking.
     */
    if (!_tm_daemon_config_valid(daemon)) {
        return;
    }

    level = tm_irq_critical_enter();
    now   = tm_tick_get_ms();

#if TM_DAEMON_STATS_ENABLE
    daemon->stats.feed_count++;
    if ((int32_t)(daemon->deadline_ms - now) > 0) {
        uint32_t remaining_ms = (uint32_t)(daemon->deadline_ms - now);

        daemon->stats.last_feed_remaining_ms = remaining_ms;
        if (remaining_ms < daemon->stats.min_feed_remaining_ms) {
            daemon->stats.min_feed_remaining_ms = remaining_ms;
        }
    } else {
        uint32_t late_ms = (uint32_t)(now - daemon->deadline_ms);

        daemon->stats.last_feed_remaining_ms = 0U;
        if (late_ms > daemon->stats.max_feed_late_ms) {
            daemon->stats.max_feed_late_ms = late_ms;
        }
    }
#endif

    daemon->deadline_ms = now + daemon->config->timeout_ms;
    daemon->is_offline  = false;
    tm_irq_critical_exit(level);
}

bool tm_daemon_is_online(const struct tm_daemon *daemon)
{
    bool     is_online = false;
    uint32_t level;

    if (daemon == NULL) {
        return false;
    }

    level     = tm_irq_critical_enter();
    is_online = !daemon->is_offline;
    tm_irq_critical_exit(level);

    return is_online;
}

uint32_t tm_daemon_timeout_remaining_ms(const struct tm_daemon *daemon)
{
    uint32_t deadline_ms;
    bool     is_offline;
    uint32_t level;
    uint32_t now;
    int32_t  remaining;

    if (!_tm_daemon_config_valid(daemon)) {
        return 0U;
    }

    level       = tm_irq_critical_enter();
    deadline_ms = daemon->deadline_ms;
    is_offline  = daemon->is_offline;
    tm_irq_critical_exit(level);

    if (is_offline) {
        return 0U;
    }

    now       = tm_tick_get_ms();
    remaining = (int32_t)(deadline_ms - now);

    return (remaining > 0) ? (uint32_t)remaining : 0U;
}

int tm_daemon_stats_get(const struct tm_daemon *daemon, struct tm_daemon_stats *stats)
{
    if ((daemon == NULL) || (stats == NULL)) {
        return -EINVAL;
    }
    if (!_tm_daemon_config_valid(daemon)) {
        return -EINVAL;
    }

#if !TM_DAEMON_STATS_ENABLE
    return -ENOSYS;
#else
    uint32_t level;

    level  = tm_irq_critical_enter();
    *stats = daemon->stats;
    tm_irq_critical_exit(level);

    if (stats->min_feed_remaining_ms == UINT32_MAX) {
        stats->min_feed_remaining_ms = 0U;
    }

    return 0;
#endif
}

int tm_daemon_stats_reset(struct tm_daemon *daemon)
{
    if (!_tm_daemon_config_valid(daemon)) {
        return -EINVAL;
    }

#if !TM_DAEMON_STATS_ENABLE
    return -ENOSYS;
#else
    uint32_t level;

    level = tm_irq_critical_enter();
    _tm_daemon_stats_clear(daemon);
    tm_irq_critical_exit(level);

    return 0;
#endif
}

void tm_daemon_process(struct tm_daemon_list *list)
{
    if ((list == NULL) || tm_is_in_isr()) {
        return;
    }

    uint32_t expired_count = 0;

    while (expired_count < TM_DAEMON_MAX_PROCESS_PER_CALL) {
        struct tm_daemon *current;
        tm_daemon_cb_t    callback = NULL;
        void             *owner    = NULL;
        uint32_t          now;

        if (_tm_daemon_list_lock(list) != 0) {
            return;
        }

        now     = tm_tick_get_ms();
        current = list->head;
        while (current != NULL) {
            if (_tm_daemon_config_valid(current)) {
                uint32_t level = tm_irq_critical_enter();

                if (!current->is_offline && ((int32_t)(now - current->deadline_ms) >= 0)) {
                    current->is_offline = true;
                    callback            = current->config->callback;
                    owner               = current->config->owner;
#if TM_DAEMON_STATS_ENABLE
                    current->stats.timeout_count++;
                    current->stats.last_timeout_tick_ms = now;
#endif
                }
                tm_irq_critical_exit(level);
            }

            if (callback != NULL) {
                break;
            }

            current = current->next;
        }
        _tm_daemon_list_unlock(list);

        if (callback == NULL) {
            return;
        }

        /* Callbacks intentionally run outside the list mutex to avoid lock inversion. */
        callback(owner);
        expired_count++;
    }

    /* If more expired daemons exist, keep polling to handle them later. */
}

/* Private Helpers ------------------------------------------------------------------------------ */

static int _tm_daemon_list_lock(struct tm_daemon_list *list)
{
    if ((list == NULL) || (list->mutex == NULL)) {
        return -ENODEV;
    }

    return (osMutexAcquire(list->mutex, osWaitForever) == osOK) ? 0 : -EIO;
}

static void _tm_daemon_list_unlock(struct tm_daemon_list *list)
{
    if ((list != NULL) && (list->mutex != NULL)) {
        (void)osMutexRelease(list->mutex);
    }
}

static bool _tm_daemon_timeout_valid(uint32_t timeout_ms)
{
    return ((timeout_ms > 0U) && (timeout_ms <= TM_DAEMON_MAX_TIMEOUT_MS));
}

static bool _tm_daemon_config_valid(const struct tm_daemon *daemon)
{
    return ((daemon != NULL) && (daemon->config != NULL) &&
            _tm_daemon_timeout_valid(daemon->config->timeout_ms));
}

static void _tm_daemon_stats_clear(struct tm_daemon *daemon)
{
#if TM_DAEMON_STATS_ENABLE
    if (daemon == NULL) {
        return;
    }

    daemon->stats.feed_count             = 0U;
    daemon->stats.timeout_count          = 0U;
    daemon->stats.last_feed_remaining_ms = 0U;
    daemon->stats.min_feed_remaining_ms  = UINT32_MAX;
    daemon->stats.max_feed_late_ms       = 0U;
    daemon->stats.last_timeout_tick_ms   = 0U;
#else
    (void)daemon;
#endif
}

#endif /* TM_DAEMON_ENABLE */

/* ---------------------------------------- end of file ----------------------------------------- */
