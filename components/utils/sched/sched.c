/**
 * @file    sched.c
 * @brief   Opaque cooperative periodic scheduler for bare-metal loops
 * @version v2.0.0
 * @date    2026.05.01
 *
 * Implementation role
 * -------------------
 * - pairs with `sched.h`
 * - owns the opaque storage layout and all task/core runtime fields
 * - enforces one concurrency model: caller-supplied task-context locks only
 *
 * Key collaborators
 * -----------------
 * - `sched.h`: caller-facing opaque handles and contracts
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "sched.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

/* Private Types -------------------------------------------------------------------------------- */

struct sched_core {
    uint64_t now_ticks;

    size_t task_count;
    size_t enabled_count;
    size_t ready_count;

    struct sched_task *task_list;
    struct sched_task *ready_head;

    sched_idle_fn_t idle_hook;
    void           *idle_user_data;

    struct sched_lock_iface lock_iface;
    bool                   lock_installed;
};

struct sched_task {
    uint64_t next_release_tick;
    uint64_t period_ticks;
    uint64_t phase_ticks;

    uint32_t run_count;
    uint32_t overrun_count;

    struct sched_core *core;
    struct sched_task *next;

    sched_task_fn_t handler;
    void           *user_data;
    const char     *name;

    uint8_t priority;

    bool pending;
    bool enabled;
    bool registered;
};

TM_STATIC_ASSERT(sizeof(struct sched_core) <= SCHED_CORE_STORAGE_SIZE,
                 "sched_core_storage_size_too_small");
TM_STATIC_ASSERT(sizeof(struct sched_task) <= SCHED_TASK_STORAGE_SIZE,
                 "sched_task_storage_size_too_small");

/* Private Helpers ------------------------------------------------------------------------------ */

static bool _sched_lock_iface_is_valid(const struct sched_lock_iface *lock_iface)
{
    if (lock_iface == NULL) {
        return true;
    }

    return (lock_iface->enter != NULL) && (lock_iface->exit != NULL);
}

static int _sched_storage_check(void *storage, size_t storage_size, size_t min_size, size_t align)
{
    if ((storage == NULL) || (storage_size < min_size) || !TM_IS_ALIGNED(storage, align)) {
        return -EINVAL;
    }

    return 0;
}

static void _sched_core_lock(struct sched_core *core)
{
    if ((core != NULL) && core->lock_installed) {
        core->lock_iface.enter(core->lock_iface.user_data);
    }
}

static void _sched_core_unlock(struct sched_core *core)
{
    if ((core != NULL) && core->lock_installed) {
        core->lock_iface.exit(core->lock_iface.user_data);
    }
}

static bool _sched_u64_add_overflow(uint64_t lhs, uint64_t rhs, uint64_t *result)
{
    if (lhs > (UINT64_MAX - rhs)) {
        return true;
    }

    if (result != NULL) {
        *result = lhs + rhs;
    }
    return false;
}

static bool _sched_u64_mul_overflow(uint64_t lhs, uint64_t rhs, uint64_t *result)
{
    if ((lhs == 0U) || (rhs == 0U)) {
        if (result != NULL) {
            *result = 0U;
        }
        return false;
    }

    if (lhs > (UINT64_MAX / rhs)) {
        return true;
    }

    if (result != NULL) {
        *result = lhs * rhs;
    }
    return false;
}

static int _sched_task_seed_from_now(struct sched_core *core,
                                     struct sched_task *task,
                                     uint64_t          *next_release_tick)
{
    uint64_t seeded_tick = 0U;

    if ((core == NULL) || (task == NULL)) {
        return -EINVAL;
    }

    if (_sched_u64_add_overflow(core->now_ticks, task->phase_ticks, &seeded_tick)) {
        return -ERANGE;
    }

    if (next_release_tick != NULL) {
        *next_release_tick = seeded_tick;
    } else {
        task->next_release_tick = seeded_tick;
    }
    return 0;
}

static int _sched_task_fast_forward_tick(uint64_t  now_ticks,
                                         uint64_t  period_ticks,
                                         uint64_t *next_release_tick,
                                         uint32_t *skipped_windows)
{
    uint64_t elapsed_ticks;
    uint64_t advance_periods;
    uint64_t delta_ticks;
    uint64_t new_next_tick = 0U;

    if ((next_release_tick == NULL) || (skipped_windows == NULL) || (period_ticks == 0U)) {
        return -EINVAL;
    }

    if (now_ticks < *next_release_tick) {
        *skipped_windows = 0U;
        return 0;
    }

    elapsed_ticks   = now_ticks - *next_release_tick;
    advance_periods = (elapsed_ticks / period_ticks) + 1U;

    if (_sched_u64_mul_overflow(advance_periods, period_ticks, &delta_ticks) ||
        _sched_u64_add_overflow(*next_release_tick, delta_ticks, &new_next_tick)) {
        return -ERANGE;
    }

    *next_release_tick = new_next_tick;
    *skipped_windows   = (advance_periods > 0U) ? (uint32_t)(advance_periods - 1U) : 0U;
    return 0;
}

static void _sched_task_pending_drop(struct sched_core *core, struct sched_task *task)
{
    if ((core == NULL) || (task == NULL) || !task->pending) {
        return;
    }

    if (core->ready_head == task) {
        struct sched_task *scan = task->next;

        core->ready_head = NULL;
        while (scan != NULL) {
            if (scan->enabled && scan->pending) {
                core->ready_head = scan;
                break;
            }
            scan = scan->next;
        }
    }

    task->pending = false;
    if (core->ready_count > 0U) {
        core->ready_count--;
    }
}

static void _sched_task_pending_take_locked(struct sched_core *core, struct sched_task *task)
{
    if ((core == NULL) || (task == NULL) || !task->pending) {
        return;
    }

    _sched_task_pending_drop(core, task);
}

static void _sched_task_info_fill_locked(const struct sched_task *task,
                                         uint64_t                 now_ticks,
                                         struct sched_task_info  *info)
{
    memset(info, 0, sizeof(*info));
    info->name              = task->name;
    info->period_ticks      = task->period_ticks;
    info->phase_ticks       = task->phase_ticks;
    info->next_release_tick = task->registered ? task->next_release_tick : 0U;
    info->run_count         = task->run_count;
    info->overrun_count     = task->overrun_count;
    info->priority          = task->priority;
    info->registered        = task->registered;
    info->enabled           = task->enabled;
    info->pending           = task->pending;

    if (!task->registered || !task->enabled || task->pending || (task->next_release_tick <= now_ticks)) {
        info->remaining_ticks = 0U;
    } else {
        info->remaining_ticks = task->next_release_tick - now_ticks;
    }
}

static struct sched_task *_sched_core_find_ready_task_locked(struct sched_core *core)
{
    if (core == NULL) {
        return NULL;
    }

    return core->ready_head;
}

static void _sched_core_insert_task_locked(struct sched_core *core, struct sched_task *task)
{
    struct sched_task **link = NULL;

    link = &core->task_list;
    while ((*link != NULL) && ((*link)->priority <= task->priority)) {
        link = &(*link)->next;
    }

    task->next = *link;
    *link      = task;
}

static bool _sched_core_remove_task_locked(struct sched_core *core, struct sched_task *task)
{
    struct sched_task **link = NULL;

    if ((core == NULL) || (task == NULL)) {
        return false;
    }

    link = &core->task_list;
    while (*link != NULL) {
        if (*link == task) {
            *link      = task->next;
            task->next = NULL;
            return true;
        }
        link = &(*link)->next;
    }

    return false;
}

/* Public Functions ----------------------------------------------------------------------------- */

int sched_core_init(void                        *storage,
                    size_t                       storage_size,
                    const struct sched_lock_iface *lock_iface,
                    struct sched_core          **out_core)
{
    struct sched_core *core = NULL;
    int                rc   = 0;

    if (out_core == NULL) {
        return -EINVAL;
    }

    rc = _sched_storage_check(
        storage, storage_size, SCHED_CORE_STORAGE_SIZE, SCHED_CORE_STORAGE_ALIGN);
    if (rc != 0) {
        return rc;
    }

    if (!_sched_lock_iface_is_valid(lock_iface)) {
        return -EINVAL;
    }

    core = (struct sched_core *)storage;
    memset(core, 0, sizeof(*core));
    if (lock_iface != NULL) {
        core->lock_iface     = *lock_iface;
        core->lock_installed = true;
    }

    *out_core = core;
    return 0;
}

int sched_core_set_idle_hook(struct sched_core *core, sched_idle_fn_t idle_hook, void *user_data)
{
    if (core == NULL) {
        return -EINVAL;
    }

    _sched_core_lock(core);
    core->idle_hook      = idle_hook;
    core->idle_user_data = user_data;
    _sched_core_unlock(core);
    return 0;
}

int sched_task_init(void               *storage,
                    size_t              storage_size,
                    const char         *name,
                    uint64_t            period_ticks,
                    uint64_t            phase_ticks,
                    uint8_t             priority,
                    sched_task_fn_t     handler,
                    void               *user_data,
                    struct sched_task **out_task)
{
    struct sched_task *task = NULL;
    int                rc   = 0;

    if ((handler == NULL) || (out_task == NULL) || (period_ticks == 0U) || (phase_ticks == 0U) ||
        (phase_ticks > period_ticks)) {
        return -EINVAL;
    }

    rc = _sched_storage_check(
        storage, storage_size, SCHED_TASK_STORAGE_SIZE, SCHED_TASK_STORAGE_ALIGN);
    if (rc != 0) {
        return rc;
    }

    task = (struct sched_task *)storage;
    memset(task, 0, sizeof(*task));
    task->period_ticks = period_ticks;
    task->phase_ticks  = phase_ticks;
    task->handler      = handler;
    task->user_data    = user_data;
    task->name         = name;
    task->priority     = priority;
    task->enabled      = true;

    *out_task = task;
    return 0;
}

int sched_core_register(struct sched_core *core, struct sched_task *task)
{
    int      rc          = 0;
    uint64_t next_release = 0U;

    if ((core == NULL) || (task == NULL)) {
        return -EINVAL;
    }

    if ((task->handler == NULL) || (task->period_ticks == 0U) || (task->phase_ticks == 0U) ||
        (task->phase_ticks > task->period_ticks)) {
        return -EINVAL;
    }

    if (task->registered) {
        return -EALREADY;
    }

    _sched_core_lock(core);
    if (task->registered) {
        _sched_core_unlock(core);
        return -EALREADY;
    }

    rc = _sched_task_seed_from_now(core, task, &next_release);
    if (rc != 0) {
        _sched_core_unlock(core);
        return rc;
    }

    task->core              = core;
    task->next_release_tick = next_release;
    task->pending           = false;
    task->registered        = true;
    task->next              = NULL;

    _sched_core_insert_task_locked(core, task);
    core->task_count++;
    if (task->enabled) {
        core->enabled_count++;
    }
    _sched_core_unlock(core);

    return 0;
}

int sched_core_unregister(struct sched_core *core, struct sched_task *task)
{
    if ((core == NULL) || (task == NULL)) {
        return -EINVAL;
    }

    _sched_core_lock(core);
    if (!task->registered || (task->core != core) || !_sched_core_remove_task_locked(core, task)) {
        _sched_core_unlock(core);
        return -ENOENT;
    }

    _sched_task_pending_drop(core, task);
    if (task->enabled && (core->enabled_count > 0U)) {
        core->enabled_count--;
    }
    if (core->task_count > 0U) {
        core->task_count--;
    }

    task->core              = NULL;
    task->registered        = false;
    task->pending           = false;
    task->next_release_tick = 0U;
    _sched_core_unlock(core);
    return 0;
}

int sched_task_set_phase(struct sched_task *task, uint64_t phase_ticks)
{
    struct sched_core *core         = NULL;
    uint64_t           next_release = 0U;
    uint64_t           old_phase    = 0U;
    int                rc           = 0;

    if ((task == NULL) || (phase_ticks == 0U) || (task->period_ticks == 0U) ||
        (phase_ticks > task->period_ticks)) {
        return -EINVAL;
    }

    if (!task->registered || (task->core == NULL)) {
        task->phase_ticks = phase_ticks;
        return 0;
    }

    core = task->core;
    _sched_core_lock(core);
    old_phase         = task->phase_ticks;
    task->phase_ticks = phase_ticks;
    rc                = _sched_task_seed_from_now(core, task, &next_release);
    if (rc == 0) {
        _sched_task_pending_drop(core, task);
        task->next_release_tick = next_release;
    } else {
        task->phase_ticks = old_phase;
    }
    _sched_core_unlock(core);
    return rc;
}

int sched_task_enable(struct sched_task *task)
{
    struct sched_core *core = NULL;
    uint64_t           next_release = 0U;
    uint32_t           skipped      = 0U;
    int                rc           = 0;

    if (task == NULL) {
        return -EINVAL;
    }

    if (!task->registered || (task->core == NULL)) {
        task->enabled = true;
        return 0;
    }

    core = task->core;
    _sched_core_lock(core);
    if (task->enabled) {
        _sched_core_unlock(core);
        return 0;
    }

    next_release = task->next_release_tick;
    if (next_release <= core->now_ticks) {
        rc = _sched_task_fast_forward_tick(
            core->now_ticks, task->period_ticks, &next_release, &skipped);
        if (rc != 0) {
            _sched_core_unlock(core);
            return rc;
        }
        (void)skipped;
    }

    task->next_release_tick = next_release;
    task->enabled           = true;
    task->pending           = false;
    core->enabled_count++;
    _sched_core_unlock(core);
    return 0;
}

int sched_task_disable(struct sched_task *task)
{
    struct sched_core *core = NULL;

    if (task == NULL) {
        return -EINVAL;
    }

    if (!task->registered || (task->core == NULL)) {
        task->pending = false;
        task->enabled = false;
        return 0;
    }

    core = task->core;
    _sched_core_lock(core);
    _sched_task_pending_drop(core, task);
    if (task->enabled) {
        task->enabled = false;
        if (core->enabled_count > 0U) {
            core->enabled_count--;
        }
    }
    _sched_core_unlock(core);
    return 0;
}

int sched_task_restart(struct sched_task *task)
{
    struct sched_core *core         = NULL;
    uint64_t           next_release = 0U;
    int                rc           = 0;

    if (task == NULL) {
        return -EINVAL;
    }

    if (!task->registered || (task->core == NULL)) {
        return -ENOENT;
    }

    core = task->core;
    _sched_core_lock(core);
    rc = _sched_task_seed_from_now(core, task, &next_release);
    if (rc == 0) {
        _sched_task_pending_drop(core, task);
        task->next_release_tick = next_release;
    }
    _sched_core_unlock(core);
    return rc;
}

int sched_task_clear_stats(struct sched_task *task)
{
    struct sched_core *core = NULL;

    if (task == NULL) {
        return -EINVAL;
    }

    core = task->registered ? task->core : NULL;
    if (core != NULL) {
        _sched_core_lock(core);
    }

    task->run_count     = 0U;
    task->overrun_count = 0U;

    if (core != NULL) {
        _sched_core_unlock(core);
    }
    return 0;
}

int sched_core_tick(struct sched_core *core, uint32_t delta_ticks)
{
    struct sched_task *task    = NULL;
    struct sched_task *ready_head = NULL;
    uint64_t           new_now = 0U;
    int                rc      = 0;

    if ((core == NULL) || (delta_ticks == 0U)) {
        return -EINVAL;
    }

    if (_sched_u64_add_overflow(core->now_ticks, (uint64_t)delta_ticks, &new_now)) {
        return -ERANGE;
    }
    core->now_ticks = new_now;

    task = core->task_list;
    while (task != NULL) {
        if (task->enabled && (core->now_ticks >= task->next_release_tick)) {
            uint64_t next_release = task->next_release_tick;
            uint32_t skipped      = 0U;

            rc = _sched_task_fast_forward_tick(
                core->now_ticks, task->period_ticks, &next_release, &skipped);
            if (rc != 0) {
                core->ready_head = ready_head;
                return rc;
            }

            if (!task->pending) {
                task->pending = true;
                core->ready_count++;
            } else if (task->overrun_count != UINT32_MAX) {
                task->overrun_count++;
            }

            if (skipped != 0U) {
                uint64_t sum = (uint64_t)task->overrun_count + (uint64_t)skipped;

                task->overrun_count = (sum > UINT32_MAX) ? UINT32_MAX : (uint32_t)sum;
            }

            task->next_release_tick = next_release;
        }

        if ((ready_head == NULL) && task->enabled && task->pending) {
            ready_head = task;
        }

        task = task->next;
    }

    core->ready_head = ready_head;
    return 0;
}

int sched_core_run(struct sched_core *core, size_t *run_count)
{
    return sched_core_run_budget(core, SIZE_MAX, run_count);
}

int sched_core_run_budget(struct sched_core *core, size_t budget, size_t *run_count)
{
    size_t dispatched = 0U;

    if (core == NULL) {
        return -EINVAL;
    }

    if (run_count != NULL) {
        *run_count = 0U;
    }

    if (budget == 0U) {
        return 0;
    }

    while (dispatched < budget) {
        struct sched_task *task    = NULL;
        sched_task_fn_t    handler = NULL;
        void              *ctx     = NULL;

        _sched_core_lock(core);
        task = _sched_core_find_ready_task_locked(core);
        if (task == NULL) {
            _sched_core_unlock(core);
            break;
        }

        _sched_task_pending_take_locked(core, task);
        if (task->run_count != UINT32_MAX) {
            task->run_count++;
        }
        handler = task->handler;
        ctx     = task->user_data;
        _sched_core_unlock(core);

        handler(ctx);
        dispatched++;
    }

    if (run_count != NULL) {
        *run_count = dispatched;
    }
    return 0;
}

int sched_core_idle(struct sched_core *core, bool *did_idle)
{
    sched_idle_fn_t idle_hook = NULL;
    void           *user_data = NULL;

    if (core == NULL) {
        return -EINVAL;
    }

    if (did_idle != NULL) {
        *did_idle = false;
    }

    _sched_core_lock(core);
    if ((core->ready_count == 0U) && (core->idle_hook != NULL)) {
        idle_hook = core->idle_hook;
        user_data = core->idle_user_data;
    }
    _sched_core_unlock(core);

    if (idle_hook == NULL) {
        return 0;
    }

    idle_hook(user_data);
    if (did_idle != NULL) {
        *did_idle = true;
    }
    return 0;
}

int sched_core_get_info(const struct sched_core *core, struct sched_core_info *info)
{
    struct sched_core *mutable_core = NULL;

    if ((core == NULL) || (info == NULL)) {
        return -EINVAL;
    }

    mutable_core = (struct sched_core *)core;
    _sched_core_lock(mutable_core);
    memset(info, 0, sizeof(*info));
    info->now_ticks      = core->now_ticks;
    info->task_count     = core->task_count;
    info->enabled_count  = core->enabled_count;
    info->ready_count    = core->ready_count;
    info->lock_installed = core->lock_installed;
    _sched_core_unlock(mutable_core);
    return 0;
}

int sched_core_peek_next_ready(const struct sched_core *core, struct sched_task_info *info)
{
    struct sched_core       *mutable_core = NULL;
    const struct sched_task *task         = NULL;

    if ((core == NULL) || (info == NULL)) {
        return -EINVAL;
    }

    mutable_core = (struct sched_core *)core;
    _sched_core_lock(mutable_core);
    task = core->ready_head;
    if (task == NULL) {
        _sched_core_unlock(mutable_core);
        return -ENOENT;
    }

    _sched_task_info_fill_locked(task, core->now_ticks, info);
    _sched_core_unlock(mutable_core);
    return 0;
}

int sched_task_get_info(const struct sched_task *task, struct sched_task_info *info)
{
    struct sched_core *core = NULL;

    if ((task == NULL) || (info == NULL)) {
        return -EINVAL;
    }

    core = task->registered ? task->core : NULL;
    if (core != NULL) {
        _sched_core_lock(core);
        _sched_task_info_fill_locked(task, core->now_ticks, info);
        _sched_core_unlock(core);
    } else {
        _sched_task_info_fill_locked(task, 0U, info);
    }

    return 0;
}

int sched_core_get_task_info_at(const struct sched_core *core,
                                size_t                   index,
                                struct sched_task_info  *info)
{
    struct sched_core       *mutable_core = NULL;
    const struct sched_task *task         = NULL;
    size_t                   i            = 0U;

    if ((core == NULL) || (info == NULL)) {
        return -EINVAL;
    }

    mutable_core = (struct sched_core *)core;
    _sched_core_lock(mutable_core);
    task = core->task_list;
    while ((task != NULL) && (i < index)) {
        task = task->next;
        i++;
    }

    if (task == NULL) {
        _sched_core_unlock(mutable_core);
        return -ENOENT;
    }

    _sched_task_info_fill_locked(task, core->now_ticks, info);
    _sched_core_unlock(mutable_core);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
