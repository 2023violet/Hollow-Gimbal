/**
 * @file    sched.c
 * @brief   Opaque cooperative periodic scheduler implementation
 * @version v2.1.1
 * @date    2026.07.24
 *
 * Implementation role
 * -------------------
 * - pairs with `sched.h` and exclusively owns the opaque core/task layouts
 * - maintains priority/FIFO task ordering, pending-release state, and diagnostics
 * - keeps release bookkeeping separate from callback execution
 *
 * Key invariants
 * --------------
 * - every registered task appears exactly once in its owning core list
 * - `ready_head` is either `NULL` or the first enabled pending task in list order
 * - tick advancement validates all deadline arithmetic before committing any state
 * - handlers execute only after the caller lock has been released
 */

/* Includes ------------------------------------------------------------------------------------- */

#include "sched.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

/* Private Macros ------------------------------------------------------------------------------- */

#define SCHED_CORE_MAGIC UINT32_C(0x53434843) /* SCHC */
#define SCHED_TASK_MAGIC UINT32_C(0x53434854) /* SCHT */

/* Alignment query portable to AC5 (offsetof trick, C11 6.7.2.1). */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define SCHED_ALIGNOF(type) _Alignof(type)
#else
#define SCHED_ALIGNOF(type) \
    (offsetof(struct { char _sched_align_c; type _sched_align_m; }, _sched_align_m))
#endif

/* Private Types -------------------------------------------------------------------------------- */

struct sched_core {
    /* 8-byte group ----------------------------------------------------------------------------- */
    uint64_t now_ticks;
    uint64_t successful_tick_count;
    uint64_t completed_dispatch_count;

    /* Pointer / 4-byte group ------------------------------------------------------------------- */
    size_t task_count;
    size_t enabled_count;
    size_t ready_count;

    struct sched_task *task_list;
    struct sched_task *ready_head;

    sched_idle_fn_t idle_hook;
    void *idle_user_data;

    struct sched_lock_iface lock_iface;

    uint32_t magic;
    uint32_t abi_version;

    /* 1-byte group + reserved padding ---------------------------------------------------------- */
    bool lock_installed;
    bool dispatching;
    uint8_t reserved[6];
};

struct sched_task {
    /* 8-byte group ----------------------------------------------------------------------------- */
    uint64_t next_release_tick;
    uint64_t period_ticks;
    uint64_t phase_ticks;

    /* Pointer / 4-byte group ------------------------------------------------------------------- */
    struct sched_core *core;
    struct sched_task *next;
    sched_task_fn_t handler;
    void *user_data;
    const char *name;

    uint32_t magic;
    uint32_t abi_version;
    uint32_t run_count;
    uint32_t overrun_count;

    /* 1-byte group + reserved padding ---------------------------------------------------------- */
    uint8_t priority;
    bool pending;
    bool enabled;
    bool registered;
    uint8_t reserved[4];
};

struct sched_dispatch_context {
    struct sched_task *task;
    sched_task_fn_t handler;
    void *user_data;
};

TM_STATIC_ASSERT(
    sizeof(struct sched_core) <= SCHED_CORE_STORAGE_SIZE,
    "sched_core_storage_size_too_small"
);
TM_STATIC_ASSERT(
    sizeof(struct sched_task) <= SCHED_TASK_STORAGE_SIZE,
    "sched_task_storage_size_too_small"
);
TM_STATIC_ASSERT(
    SCHED_ALIGNOF(struct sched_core) <= SCHED_CORE_STORAGE_ALIGN,
    "sched_core_storage_alignment_too_small"
);
TM_STATIC_ASSERT(
    SCHED_ALIGNOF(struct sched_task) <= SCHED_TASK_STORAGE_ALIGN,
    "sched_task_storage_alignment_too_small"
);

/* Private Helpers ------------------------------------------------------------------------------ */

/* Probe raw storage through `memcpy` so initialization never dereferences uninitialized bytes as
 * a live private structure. The explicit offset keeps repeated-init detection independent of the
 * private layout order used for Cortex-M padding control. */
static bool
_sched_storage_header_matches(const void *storage, size_t header_offset, uint32_t expected_magic)
{
    const uint8_t *bytes = storage;
    uint32_t header[2] = {0U, 0U};

    if (storage == NULL) {
        return false;
    }
    memcpy(header, &bytes[header_offset], sizeof(header));
    return (header[0] == expected_magic) && (header[1] == SCHED_ABI_VERSION);
}

static bool _sched_core_is_valid(const struct sched_core *core)
{
    return (core != NULL) && (core->magic == SCHED_CORE_MAGIC) &&
        (core->abi_version == SCHED_ABI_VERSION);
}

static bool _sched_task_is_valid(const struct sched_task *task)
{
    return (task != NULL) && (task->magic == SCHED_TASK_MAGIC) &&
        (task->abi_version == SCHED_ABI_VERSION) && (task->handler != NULL) &&
        (task->period_ticks != 0U) && (task->phase_ticks != 0U) &&
        (task->phase_ticks <= task->period_ticks);
}

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
    if (core->lock_installed) {
        core->lock_iface.enter(core->lock_iface.user_data);
    }
}

static void _sched_core_unlock(struct sched_core *core)
{
    if (core->lock_installed) {
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

static uint32_t _sched_u32_sat_add(uint32_t lhs, uint32_t rhs)
{
    if (lhs > (UINT32_MAX - rhs)) {
        return UINT32_MAX;
    }
    return lhs + rhs;
}

static int _sched_task_seed_from_now(
    const struct sched_core *core,
    const struct sched_task *task,
    uint64_t *next_release_tick
)
{
    if ((core == NULL) || (task == NULL) || (next_release_tick == NULL)) {
        return -EINVAL;
    }
    if (_sched_u64_add_overflow(core->now_ticks, task->phase_ticks, next_release_tick)) {
        return -ERANGE;
    }
    return 0;
}

static int _sched_task_fast_forward_tick(
    uint64_t now_ticks,
    uint64_t period_ticks,
    uint64_t current_release_tick,
    uint64_t *next_release_tick,
    uint32_t *skipped_windows
)
{
    uint64_t elapsed_ticks;
    uint64_t advance_periods;
    uint64_t delta_ticks;
    uint64_t missed_windows;

    if ((next_release_tick == NULL) || (skipped_windows == NULL) || (period_ticks == 0U)) {
        return -EINVAL;
    }

    if (now_ticks < current_release_tick) {
        *next_release_tick = current_release_tick;
        *skipped_windows = 0U;
        return 0;
    }

    elapsed_ticks = now_ticks - current_release_tick;
    advance_periods = elapsed_ticks / period_ticks;
    if (advance_periods == UINT64_MAX) {
        return -ERANGE;
    }
    advance_periods++;

    if (_sched_u64_mul_overflow(advance_periods, period_ticks, &delta_ticks) ||
        _sched_u64_add_overflow(current_release_tick, delta_ticks, next_release_tick)) {
        return -ERANGE;
    }

    missed_windows = advance_periods - 1U;
    *skipped_windows = (missed_windows > UINT32_MAX) ? UINT32_MAX : (uint32_t)missed_windows;
    return 0;
}

static void _sched_task_pending_drop(struct sched_core *core, struct sched_task *task)
{
    if (!task->pending) {
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

static void _sched_task_info_fill(
    const struct sched_task *task,
    uint64_t now_ticks,
    struct sched_task_info *info
)
{
    memset(info, 0, sizeof(*info));
    info->period_ticks = task->period_ticks;
    info->phase_ticks = task->phase_ticks;
    info->next_release_tick = task->registered ? task->next_release_tick : 0U;
    info->name = task->name;
    info->run_count = task->run_count;
    info->overrun_count = task->overrun_count;
    info->priority = task->priority;
    info->registered = task->registered;
    info->enabled = task->enabled;
    info->pending = task->pending;

    if (task->registered && task->enabled && !task->pending &&
        (task->next_release_tick > now_ticks)) {
        info->remaining_ticks = task->next_release_tick - now_ticks;
    }
}

static void _sched_core_insert_task(struct sched_core *core, struct sched_task *task)
{
    struct sched_task **link = &core->task_list;
    while ((*link != NULL) && ((*link)->priority <= task->priority)) {
        link = &(*link)->next;
    }
    task->next = *link;
    *link = task;
}

static bool _sched_core_contains_task(const struct sched_core *core, const struct sched_task *task)
{
    const struct sched_task *scan = core->task_list;
    while (scan != NULL) {
        if (scan == task) {
            return true;
        }
        scan = scan->next;
    }
    return false;
}

static bool _sched_core_remove_task(struct sched_core *core, struct sched_task *task)
{
    struct sched_task **link = &core->task_list;
    while (*link != NULL) {
        if (*link == task) {
            *link = task->next;
            task->next = NULL;
            return true;
        }
        link = &(*link)->next;
    }
    return false;
}

static bool _sched_list_has_cycle(const struct sched_task *head)
{
    const struct sched_task *slow = head;
    const struct sched_task *fast = head;

    while ((fast != NULL) && (fast->next != NULL)) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return true;
        }
    }
    return false;
}

static int _sched_core_tick_preflight(const struct sched_core *core, uint64_t new_now)
{
    const struct sched_task *task;

    if (_sched_list_has_cycle(core->task_list)) {
        return -EFAULT;
    }

    task = core->task_list;
    while (task != NULL) {
        uint64_t next_release_tick;
        uint32_t skipped_windows;

        if (!_sched_task_is_valid(task) || !task->registered || (task->core != core)) {
            return -EFAULT;
        }
        if (task->enabled && (new_now >= task->next_release_tick)) {
            int rc = _sched_task_fast_forward_tick(
                new_now,
                task->period_ticks,
                task->next_release_tick,
                &next_release_tick,
                &skipped_windows
            );
            if (rc != 0) {
                return rc;
            }
        }
        task = task->next;
    }
    return 0;
}

static int _sched_core_tick_commit(struct sched_core *core, uint64_t new_now)
{
    struct sched_task *task = core->task_list;
    struct sched_task *ready_head = NULL;

    core->now_ticks = new_now;
    if (core->successful_tick_count != UINT64_MAX) {
        core->successful_tick_count++;
    }

    while (task != NULL) {
        if (task->enabled && (new_now >= task->next_release_tick)) {
            uint64_t next_release_tick = task->next_release_tick;
            uint32_t skipped_windows = 0U;
            uint32_t missed_window = task->pending ? 1U : 0U;
            int rc = _sched_task_fast_forward_tick(
                new_now,
                task->period_ticks,
                task->next_release_tick,
                &next_release_tick,
                &skipped_windows
            );

            /* Preflight already proved this arithmetic. A failure here means unsupported
             * concurrent mutation or corrupted private state, not an ordinary time overflow. */
            if (rc != 0) {
                return -EFAULT;
            }

            if (!task->pending) {
                task->pending = true;
                core->ready_count++;
            }
            task->overrun_count = _sched_u32_sat_add(task->overrun_count, missed_window);
            task->overrun_count = _sched_u32_sat_add(task->overrun_count, skipped_windows);
            task->next_release_tick = next_release_tick;
        }

        if ((ready_head == NULL) && task->enabled && task->pending) {
            ready_head = task;
        }
        task = task->next;
    }

    core->ready_head = ready_head;
    return 0;
}

static int
_sched_dispatch_take_locked(struct sched_core *core, struct sched_dispatch_context *dispatch)
{
    struct sched_task *task = core->ready_head;

    if (task == NULL) {
        return -ENOENT;
    }
    if (!_sched_task_is_valid(task) || !task->registered || (task->core != core) ||
        !task->enabled || !task->pending) {
        return -EFAULT;
    }

    _sched_task_pending_drop(core, task);
    if (task->run_count != UINT32_MAX) {
        task->run_count++;
    }

    dispatch->task = task;
    dispatch->handler = task->handler;
    dispatch->user_data = task->user_data;
    return 0;
}

static void _sched_dispatch_complete_locked(struct sched_core *core)
{
    if (core->completed_dispatch_count != UINT64_MAX) {
        core->completed_dispatch_count++;
    }
}

/* Public Functions ----------------------------------------------------------------------------- */
/* Lifecycle ------------------------------------------------------------------------------------ */

int sched_core_init(
    void *storage,
    size_t storage_size,
    const struct sched_lock_iface *lock_iface,
    struct sched_core **out_core
)
{
    struct sched_core *core;
    int rc;

    if (out_core == NULL) {
        return -EINVAL;
    }
    *out_core = NULL;

    rc = _sched_storage_check(
        storage,
        storage_size,
        SCHED_CORE_STORAGE_SIZE,
        SCHED_CORE_STORAGE_ALIGN
    );
    if (rc != 0) {
        return rc;
    }
    if (!_sched_lock_iface_is_valid(lock_iface)) {
        return -EINVAL;
    }

    core = (struct sched_core *)storage;
    if (_sched_storage_header_matches(
            storage,
            offsetof(struct sched_core, magic),
            SCHED_CORE_MAGIC
        ) &&
        ((core->task_count != 0U) || core->dispatching)) {
        return -EBUSY;
    }

    memset(core, 0, sizeof(*core));
    core->magic = SCHED_CORE_MAGIC;
    core->abi_version = SCHED_ABI_VERSION;
    if (lock_iface != NULL) {
        core->lock_iface = *lock_iface;
        core->lock_installed = true;
    }

    *out_core = core;
    return 0;
}

int sched_core_set_idle_hook(struct sched_core *core, sched_idle_fn_t idle_hook, void *user_data)
{
    if (!_sched_core_is_valid(core)) {
        return -EINVAL;
    }

    _sched_core_lock(core);
    core->idle_hook = idle_hook;
    core->idle_user_data = user_data;
    _sched_core_unlock(core);
    return 0;
}

int sched_task_init(
    void *storage,
    size_t storage_size,
    const char *name,
    uint64_t period_ticks,
    uint64_t phase_ticks,
    uint8_t priority,
    sched_task_fn_t handler,
    void *user_data,
    struct sched_task **out_task
)
{
    struct sched_task *task;
    int rc;

    if (out_task == NULL) {
        return -EINVAL;
    }
    *out_task = NULL;

    if ((handler == NULL) || (period_ticks == 0U) || (phase_ticks == 0U) ||
        (phase_ticks > period_ticks)) {
        return -EINVAL;
    }

    rc = _sched_storage_check(
        storage,
        storage_size,
        SCHED_TASK_STORAGE_SIZE,
        SCHED_TASK_STORAGE_ALIGN
    );
    if (rc != 0) {
        return rc;
    }

    task = (struct sched_task *)storage;
    if (_sched_storage_header_matches(
            storage,
            offsetof(struct sched_task, magic),
            SCHED_TASK_MAGIC
        ) &&
        task->registered) {
        return -EBUSY;
    }

    memset(task, 0, sizeof(*task));
    task->period_ticks = period_ticks;
    task->phase_ticks = phase_ticks;
    task->handler = handler;
    task->user_data = user_data;
    task->name = name;
    task->magic = SCHED_TASK_MAGIC;
    task->abi_version = SCHED_ABI_VERSION;
    task->priority = priority;
    task->enabled = true;

    *out_task = task;
    return 0;
}

int sched_core_register(struct sched_core *core, struct sched_task *task)
{
    uint64_t next_release;
    int rc;

    if (!_sched_core_is_valid(core) || !_sched_task_is_valid(task)) {
        return -EINVAL;
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

    task->core = core;
    task->next_release_tick = next_release;
    task->pending = false;
    task->registered = true;
    task->next = NULL;
    _sched_core_insert_task(core, task);
    core->task_count++;
    if (task->enabled) {
        core->enabled_count++;
    }
    _sched_core_unlock(core);
    return 0;
}

int sched_core_unregister(struct sched_core *core, struct sched_task *task)
{
    if (!_sched_core_is_valid(core) || !_sched_task_is_valid(task)) {
        return -EINVAL;
    }

    _sched_core_lock(core);
    if (!task->registered || (task->core != core) || !_sched_core_contains_task(core, task)) {
        _sched_core_unlock(core);
        return -ENOENT;
    }

    /* Drop pending before unlinking so ready-head recovery can follow task->next. */
    _sched_task_pending_drop(core, task);
    (void)_sched_core_remove_task(core, task);
    if (task->enabled && (core->enabled_count > 0U)) {
        core->enabled_count--;
    }
    if (core->task_count > 0U) {
        core->task_count--;
    }

    task->core = NULL;
    task->registered = false;
    task->pending = false;
    task->next_release_tick = 0U;
    _sched_core_unlock(core);
    return 0;
}

/* Task Control --------------------------------------------------------------------------------- */
int sched_task_set_phase(struct sched_task *task, uint64_t phase_ticks)
{
    struct sched_core *core;
    uint64_t next_release;

    if (!_sched_task_is_valid(task) || (phase_ticks == 0U) || (phase_ticks > task->period_ticks)) {
        return -EINVAL;
    }

    if (!task->registered) {
        task->phase_ticks = phase_ticks;
        return 0;
    }

    core = task->core;
    if (!_sched_core_is_valid(core)) {
        return -EFAULT;
    }

    _sched_core_lock(core);
    if (_sched_u64_add_overflow(core->now_ticks, phase_ticks, &next_release)) {
        _sched_core_unlock(core);
        return -ERANGE;
    }

    _sched_task_pending_drop(core, task);
    task->phase_ticks = phase_ticks;
    task->next_release_tick = next_release;
    _sched_core_unlock(core);
    return 0;
}

int sched_task_enable(struct sched_task *task)
{
    struct sched_core *core;
    uint64_t next_release;
    uint32_t skipped;
    int rc;

    if (!_sched_task_is_valid(task)) {
        return -EINVAL;
    }

    if (!task->registered) {
        task->pending = false;
        task->enabled = true;
        return 0;
    }

    core = task->core;
    if (!_sched_core_is_valid(core)) {
        return -EFAULT;
    }

    _sched_core_lock(core);
    if (task->enabled) {
        _sched_core_unlock(core);
        return 0;
    }

    next_release = task->next_release_tick;
    if (next_release <= core->now_ticks) {
        rc = _sched_task_fast_forward_tick(
            core->now_ticks,
            task->period_ticks,
            next_release,
            &next_release,
            &skipped
        );
        if (rc != 0) {
            _sched_core_unlock(core);
            return rc;
        }
    }

    task->next_release_tick = next_release;
    task->enabled = true;
    task->pending = false;
    core->enabled_count++;
    _sched_core_unlock(core);
    return 0;
}

int sched_task_disable(struct sched_task *task)
{
    struct sched_core *core;

    if (!_sched_task_is_valid(task)) {
        return -EINVAL;
    }

    if (!task->registered) {
        task->pending = false;
        task->enabled = false;
        return 0;
    }

    core = task->core;
    if (!_sched_core_is_valid(core)) {
        return -EFAULT;
    }

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
    struct sched_core *core;
    uint64_t next_release;
    int rc;

    if (!_sched_task_is_valid(task)) {
        return -EINVAL;
    }
    if (!task->registered) {
        return -ENOENT;
    }

    core = task->core;
    if (!_sched_core_is_valid(core)) {
        return -EFAULT;
    }

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

    if (!_sched_task_is_valid(task)) {
        return -EINVAL;
    }

    if (task->registered) {
        core = task->core;
        if (!_sched_core_is_valid(core)) {
            return -EFAULT;
        }
        _sched_core_lock(core);
    }

    task->run_count = 0U;
    task->overrun_count = 0U;

    if (core != NULL) {
        _sched_core_unlock(core);
    }
    return 0;
}

/* Runtime Step --------------------------------------------------------------------------------- */
int sched_core_tick(struct sched_core *core, uint32_t delta_ticks)
{
    return sched_core_tick64(core, (uint64_t)delta_ticks);
}

int sched_core_tick64(struct sched_core *core, uint64_t delta_ticks)
{
    uint64_t new_now;
    int rc;

    if (!_sched_core_is_valid(core) || (delta_ticks == 0U)) {
        return -EINVAL;
    }
    if (_sched_u64_add_overflow(core->now_ticks, delta_ticks, &new_now)) {
        return -ERANGE;
    }

    /* Preflight proves every due-task calculation before any externally visible state changes.
     * A reported error therefore leaves the complete scheduler state unchanged. */
    rc = _sched_core_tick_preflight(core, new_now);
    if (rc != 0) {
        return rc;
    }
    return _sched_core_tick_commit(core, new_now);
}

int sched_core_run(struct sched_core *core, size_t *run_count)
{
    return sched_core_run_budget(core, SIZE_MAX, run_count);
}

int sched_core_run_budget(struct sched_core *core, size_t budget, size_t *run_count)
{
    size_t dispatched = 0U;
    int result = 0;

    if (!_sched_core_is_valid(core)) {
        return -EINVAL;
    }
    if (run_count != NULL) {
        *run_count = 0U;
    }
    if (budget == 0U) {
        return 0;
    }

    _sched_core_lock(core);
    if (core->dispatching) {
        _sched_core_unlock(core);
        return -EBUSY;
    }
    core->dispatching = true;
    _sched_core_unlock(core);

    while (dispatched < budget) {
        struct sched_dispatch_context dispatch = {0};
        int rc;

        _sched_core_lock(core);
        rc = _sched_dispatch_take_locked(core, &dispatch);
        _sched_core_unlock(core);
        if (rc == -ENOENT) {
            break;
        }
        if (rc != 0) {
            result = rc;
            break;
        }

        /* External code runs after unlock so callbacks cannot extend the critical section or
         * deadlock by calling task-control APIs on this core. */
        dispatch.handler(dispatch.user_data);
        dispatched++;

        _sched_core_lock(core);
        _sched_dispatch_complete_locked(core);
        _sched_core_unlock(core);
    }

    _sched_core_lock(core);
    core->dispatching = false;
    _sched_core_unlock(core);

    if (run_count != NULL) {
        *run_count = dispatched;
    }
    return result;
}

/* Idle Integration ----------------------------------------------------------------------------- */
int sched_core_idle(struct sched_core *core, bool *did_idle)
{
    sched_idle_fn_t hook = NULL;
    void *user_data = NULL;

    if (!_sched_core_is_valid(core)) {
        return -EINVAL;
    }
    if (did_idle != NULL) {
        *did_idle = false;
    }

    _sched_core_lock(core);
    if ((core->ready_count == 0U) && (core->idle_hook != NULL)) {
        hook = core->idle_hook;
        user_data = core->idle_user_data;
    }
    _sched_core_unlock(core);

    if (hook != NULL) {
        hook(user_data);
        if (did_idle != NULL) {
            *did_idle = true;
        }
    }
    return 0;
}

int sched_core_idle_atomic(struct sched_core *core, bool *did_idle)
{
    /* Compatibility only: Themis forbids running external callbacks while the component lock is
     * held. Platform-specific atomic sleep entry belongs to the BSP/platform owner. */
    return sched_core_idle(core, did_idle);
}

/* Snapshot / Diagnostics ----------------------------------------------------------------------- */
int sched_core_get_info(struct sched_core *core, struct sched_core_info *info)
{
    if (!_sched_core_is_valid(core) || (info == NULL)) {
        return -EINVAL;
    }

    _sched_core_lock(core);
    memset(info, 0, sizeof(*info));
    info->now_ticks = core->now_ticks;
    info->successful_tick_count = core->successful_tick_count;
    info->completed_dispatch_count = core->completed_dispatch_count;
    info->task_count = core->task_count;
    info->enabled_count = core->enabled_count;
    info->ready_count = core->ready_count;
    info->abi_version = core->abi_version;
    info->lock_installed = core->lock_installed;
    info->dispatching = core->dispatching;
    _sched_core_unlock(core);
    return 0;
}

int sched_task_get_info(const struct sched_task *task, struct sched_task_info *info)
{
    struct sched_core *core = NULL;

    if (!_sched_task_is_valid(task) || (info == NULL)) {
        return -EINVAL;
    }

    if (task->registered) {
        core = task->core;
        if (!_sched_core_is_valid(core)) {
            return -EFAULT;
        }
        _sched_core_lock(core);
        _sched_task_info_fill(task, core->now_ticks, info);
        _sched_core_unlock(core);
    } else {
        _sched_task_info_fill(task, 0U, info);
    }
    return 0;
}

int sched_core_peek_next_ready(struct sched_core *core, struct sched_task_info *info)
{
    struct sched_task *task;

    if (!_sched_core_is_valid(core) || (info == NULL)) {
        return -EINVAL;
    }

    _sched_core_lock(core);
    task = core->ready_head;
    if (task == NULL) {
        _sched_core_unlock(core);
        return -ENOENT;
    }
    if (!_sched_task_is_valid(task)) {
        _sched_core_unlock(core);
        return -EFAULT;
    }
    _sched_task_info_fill(task, core->now_ticks, info);
    _sched_core_unlock(core);
    return 0;
}

int sched_core_get_task_info_at(struct sched_core *core, size_t index, struct sched_task_info *info)
{
    struct sched_task *task;
    size_t i = 0U;

    if (!_sched_core_is_valid(core) || (info == NULL)) {
        return -EINVAL;
    }

    _sched_core_lock(core);
    task = core->task_list;
    while ((task != NULL) && (i < index)) {
        task = task->next;
        i++;
    }
    if (task == NULL) {
        _sched_core_unlock(core);
        return -ENOENT;
    }
    if (!_sched_task_is_valid(task)) {
        _sched_core_unlock(core);
        return -EFAULT;
    }
    _sched_task_info_fill(task, core->now_ticks, info);
    _sched_core_unlock(core);
    return 0;
}

int sched_core_validate(struct sched_core *core)
{
    const struct sched_task *task;
    const struct sched_task *expected_ready = NULL;
    size_t task_count = 0U;
    size_t enabled_count = 0U;
    size_t ready_count = 0U;
    uint8_t previous_priority = 0U;
    bool have_previous = false;
    int result = 0;

    if (!_sched_core_is_valid(core)) {
        return -EINVAL;
    }

    _sched_core_lock(core);
    if (_sched_list_has_cycle(core->task_list)) {
        result = -EFAULT;
        goto done;
    }

    task = core->task_list;
    while (task != NULL) {
        if (!_sched_task_is_valid(task) || !task->registered || (task->core != core)) {
            result = -EFAULT;
            goto done;
        }
        if (have_previous && (task->priority < previous_priority)) {
            result = -EFAULT;
            goto done;
        }
        previous_priority = task->priority;
        have_previous = true;
        task_count++;

        if (task->enabled) {
            enabled_count++;
        }
        if (task->pending) {
            if (!task->enabled) {
                result = -EFAULT;
                goto done;
            }
            ready_count++;
            if (expected_ready == NULL) {
                expected_ready = task;
            }
        }
        task = task->next;
    }

    if ((task_count != core->task_count) || (enabled_count != core->enabled_count) ||
        (ready_count != core->ready_count) || (expected_ready != core->ready_head)) {
        result = -EFAULT;
    }

done:
    _sched_core_unlock(core);
    return result;
}

/* ---------------------------------------- end of file ----------------------------------------- */