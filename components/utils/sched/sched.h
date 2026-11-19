/**
 * @file    sched.h
 * @brief   Opaque cooperative periodic scheduler for bare-metal main loops
 * @version v2.1.1
 * @date    2026.07.24
 *
 * Design overview
 * ---------------
 * `sched` releases periodic tasks from a caller-driven 64-bit monotonic time domain and
 * dispatches them cooperatively from task context. The component owns no hardware clock,
 * performs no allocation, and has no RTOS or board dependency.
 *
 * Tasks are stored in a priority-ordered intrusive list. Lower numeric priorities run first,
 * and equal-priority tasks retain FIFO registration order. Each task can hold at most one
 * pending release; additional release windows are coalesced and counted as overruns.
 *
 * Usage example
 * -------------
 * @code
 * SCHED_CORE_STORAGE(core_storage) = {0};
 * SCHED_TASK_STORAGE(task_storage) = {0};
 * struct sched_core *core = NULL;
 * struct sched_task *task = NULL;
 *
 * (void)sched_core_init(core_storage, sizeof(core_storage), NULL, &core);
 * (void)sched_task_init(task_storage,
 *                       sizeof(task_storage),
 *                       "poll",
 *                       10U,
 *                       10U,
 *                       0U,
 *                       poll_handler,
 *                       NULL,
 *                       &task);

 * (void)sched_core_register(core, task);
 *
 * for (;;) {
 *     (void)sched_core_tick(core, 1U);
 *     (void)sched_core_run(core, NULL);
 * }
 * @endcode
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns every storage buffer, task name, callback, and callback context.
 * - Names and callback contexts are borrowed for the lifetime of the initialized object.
 * - A task must be initialized before registration and unregistered before its storage is
 *   reused.
 * - One core supports one task-context control path plus one serialized tick context.
 * - When tick runs in an ISR, install lock hooks that prevent that ISR from observing
 *   task-context mutations. Tick APIs intentionally do not call those hooks.
 * - Task handlers execute outside the scheduler lock and may control or unregister tasks.
 * - Except for the documented tick split, APIs are not generally thread-safe or reentrant on
 *   the same core. Distinct core instances are independent.
 *
 * Blocking / allocation
 * ---------------------
 * - No API allocates memory or waits on an OS primitive.
 * - Lock-hook latency is entirely owned by the caller.
 * - Task handlers and the compatibility idle hook may block only if the caller deliberately
 *   provides blocking callbacks; doing so is discouraged for cooperative main loops.
 *
 * Failure / recovery
 * ------------------
 * - Status APIs return `0` on success or a negative `-errno` value on failure.
 * - Invalid pointers, storage spans, alignments, periods, or phases fail with `-EINVAL`.
 * - Live-object reinitialization fails with `-EBUSY`.
 * - Missing membership fails with `-ENOENT`; duplicate registration fails with `-EALREADY`.
 * - Time-domain overflow fails with `-ERANGE` without partially committing tick state.
 * - Detected internal ownership, list, or lifecycle corruption fails with `-EFAULT`.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */

#include "tmdef.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Version / Feature Macros -------------------------------------------------------------- */

/** @brief Scheduler semantic-version major component. */
#define SCHED_VERSION_MAJOR 2U

/** @brief Scheduler semantic-version minor component. */
#define SCHED_VERSION_MINOR 1U

/** @brief Scheduler semantic-version patch component. */
#define SCHED_VERSION_PATCH 1U

/** @brief Public opaque-storage ABI identifier returned by diagnostic snapshots. */
#define SCHED_ABI_VERSION UINT32_C(0x00020100)

/** @brief Required byte alignment for caller-owned scheduler-core storage. */
#define SCHED_CORE_STORAGE_ALIGN 8U

/** @brief Required byte alignment for caller-owned scheduler-task storage. */
#define SCHED_TASK_STORAGE_ALIGN 8U

/** @brief Reserved byte capacity for one opaque scheduler core. */
#define SCHED_CORE_STORAGE_SIZE 128U

/** @brief Reserved byte capacity for one opaque scheduler task. */
#define SCHED_TASK_STORAGE_SIZE 128U

/**
 * @brief Declare aligned caller-owned storage for one scheduler core.
 *
 * @param[in] name Identifier of the generated `uint8_t` array.
 *
 * @note The array must remain valid until the core and all registered tasks are no longer used.
 */
#define SCHED_CORE_STORAGE(name) \
    TM_ALIGN(SCHED_CORE_STORAGE_ALIGN) uint8_t name[SCHED_CORE_STORAGE_SIZE]

/**
 * @brief Declare aligned caller-owned storage for one scheduler task.
 *
 * @param[in] name Identifier of the generated `uint8_t` array.
 *
 * @note The array must remain valid until the task is unregistered and no API uses its handle.
 */
#define SCHED_TASK_STORAGE(name) \
    TM_ALIGN(SCHED_TASK_STORAGE_ALIGN) uint8_t name[SCHED_TASK_STORAGE_SIZE]

/* Public Forward Declarations ------------------------------------------------------------------ */

/** @brief Opaque scheduler-core handle initialized inside caller-owned storage. */
struct sched_core;

/** @brief Opaque scheduler-task handle initialized inside caller-owned storage. */
struct sched_task;

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Cooperative task handler invoked by `sched_core_run()` or its budgeted variant.
 *
 * @param[in] user_data Borrowed caller context supplied to `sched_task_init()`.
 *
 * @note The handler runs outside the scheduler lock in the dispatch caller's context.
 */
typedef void (*sched_task_fn_t)(void *user_data);

/**
 * @brief Optional idle callback installed with `sched_core_set_idle_hook()`.
 *
 * @param[in] user_data Borrowed caller context supplied when the hook is installed.
 *
 * @note The hook always runs outside the scheduler lock. Platform code must provide any
 * race-free sleep-entry protocol required by the selected MCU.
 */
typedef void (*sched_idle_fn_t)(void *user_data);

/**
 * @brief Caller-provided serialization hooks for one scheduler core.
 *
 * Install both callbacks or pass `NULL` to `sched_core_init()`. The hooks normally mask the tick
 * interrupt while task-context APIs mutate shared state. The scheduler never owns or nests an OS
 * mutex on behalf of the caller.
 */
struct sched_lock_iface {
    /** @brief Enter the caller-defined critical section. Must not be `NULL` when installed. */
    void (*enter)(void *user_data);

    /** @brief Exit the caller-defined critical section. Must not be `NULL` when installed. */
    void (*exit)(void *user_data);

    /** @brief Borrowed opaque context forwarded to `enter` and `exit`. */
    void *user_data;
};

/** @brief Immutable core snapshot returned by `sched_core_get_info()`. */
struct sched_core_info {
    /* 8-byte group ----------------------------------------------------------------------------- */

    /** @brief Current absolute tick in the core's 64-bit monotonic time domain. */
    uint64_t now_ticks;

    /** @brief Number of successful nonzero tick-advance calls, saturating at `UINT64_MAX`. */
    uint64_t successful_tick_count;

    /** @brief Number of handlers that returned from dispatch, saturating at `UINT64_MAX`. */
    uint64_t completed_dispatch_count;

    /* Pointer / 4-byte group ------------------------------------------------------------------- */

    /** @brief Number of tasks currently registered with the core. */
    size_t task_count;

    /** @brief Number of registered tasks currently enabled. */
    size_t enabled_count;

    /** @brief Number of enabled tasks holding one pending release. */
    size_t ready_count;

    /** @brief ABI identifier of the initialized core object. */
    uint32_t abi_version;

    /* 1-byte group + reserved padding ---------------------------------------------------------- */

    /** @brief `true` when caller-provided serialization hooks are installed. */
    bool lock_installed;

    /** @brief `true` while a run API owns the dispatch loop for this core. */
    bool dispatching;

    /** @brief Reserved bytes kept zero for compatible snapshot extension. */
    uint8_t reserved[2];
};

/** @brief Immutable task snapshot returned by task and core query APIs. */
struct sched_task_info {
    /* 8-byte group ----------------------------------------------------------------------------- */

    /** @brief Fixed task-release period in caller-defined ticks. */
    uint64_t period_ticks;

    /** @brief Delay used for the first release after registration or restart. */
    uint64_t phase_ticks;

    /** @brief Absolute next-release tick, or zero when the task is unregistered. */
    uint64_t next_release_tick;

    /** @brief Ticks until release, or zero when disabled, pending, due, or unregistered. */
    uint64_t remaining_ticks;

    /* Pointer / 4-byte group ------------------------------------------------------------------- */

    /** @brief Borrowed diagnostic name supplied at initialization; may be `NULL`. */
    const char *name;

    /** @brief Number of handler dispatches started, saturating at `UINT32_MAX`. */
    uint32_t run_count;

    /** @brief Number of coalesced or missed release windows, saturating at `UINT32_MAX`. */
    uint32_t overrun_count;

    /* 1-byte group ----------------------------------------------------------------------------- */

    /** @brief Dispatch priority; lower numeric values run first. */
    uint8_t priority;

    /** @brief `true` when the task belongs to a core. */
    bool registered;

    /** @brief `true` when the task participates in periodic release processing. */
    bool enabled;

    /** @brief `true` when one release is waiting for cooperative dispatch. */
    bool pending;
};

/* Lifecycle ------------------------------------------------------------------------------------ */

/* Initialize caller-owned core and task storage before any control or runtime API. */

/**
 * @brief Initialize one scheduler core inside caller-owned storage.
 *
 * @param[in,out] storage Raw storage with at least `SCHED_CORE_STORAGE_SIZE` bytes and the required
 * alignment. The scheduler owns the bytes after a successful call.
 * @param[in] storage_size Size of `storage` in bytes.
 * @param[in] lock_iface Optional borrowed serialization hooks, or `NULL` for a single-context core.
 * @param[out] out_core Receives the opaque handle on success and is set to `NULL` on failure after
 * the pointer itself is validated.
 *
 * @retval 0 Core initialized successfully.
 * @retval -EINVAL A pointer, storage span, alignment, or lock interface is invalid.
 * @retval -EBUSY The storage still contains a live core with tasks or an active dispatch loop.
 */
int sched_core_init(
    void *storage,
    size_t storage_size,
    const struct sched_lock_iface *lock_iface,
    struct sched_core **out_core);

/**
 * @brief Install, replace, or remove a core idle hook.
 *
 * @param[in,out] core Initialized core handle.
 * @param[in] idle_hook Borrowed callback, or `NULL` to remove the current hook.
 * @param[in] user_data Borrowed callback context; may be `NULL`.
 *
 * @retval 0 Hook state updated successfully.
 * @retval -EINVAL `core` is not a valid initialized core.
 */
int sched_core_set_idle_hook(struct sched_core *core, sched_idle_fn_t idle_hook, void *user_data);

/**
 * @brief Initialize one periodic task inside caller-owned storage.
 *
 * @param[in,out] storage Raw storage with at least `SCHED_TASK_STORAGE_SIZE` bytes and the required
 * alignment. The scheduler owns the bytes after a successful call.
 * @param[in] storage_size Size of `storage` in bytes.
 * @param[in] name Borrowed diagnostic name; may be `NULL` and is never copied.
 * @param[in] period_ticks Positive fixed release period in caller-defined ticks.
 * @param[in] phase_ticks Positive first-release delay in the range `1..period_ticks`.
 * @param[in] priority Dispatch priority; lower numeric values run first.
 * @param[in] handler Non-`NULL` borrowed task handler.
 * @param[in] user_data Borrowed context forwarded to `handler`; may be `NULL`.
 * @param[out] out_task Receives the opaque handle on success and is set to `NULL` on failure after
 * the pointer itself is validated.
 *
 * @retval 0 Task initialized successfully and enabled by default.
 * @retval -EINVAL A pointer, storage span, alignment, period, phase, or handler is invalid.
 * @retval -EBUSY The storage still contains a registered live task.
 */
int sched_task_init(
    void *storage,
    size_t storage_size,
    const char *name,
    uint64_t period_ticks,
    uint64_t phase_ticks,
    uint8_t priority,
    sched_task_fn_t handler,
    void *user_data,
    struct sched_task **out_task);

/**
 * @brief Register an initialized task with one core.
 *
 * Registration seeds the first release at `core_now + phase_ticks` and inserts the task in
 * priority/FIFO order.
 *
 * @param[in,out] core Initialized core that will own the task.
 * @param[in,out] task Initialized task that is not currently registered.
 *
 * @retval 0 Task registered successfully.
 * @retval -EINVAL A handle is invalid.
 * @retval -EALREADY `task` is already registered.
 * @retval -ERANGE The first-release calculation would overflow the 64-bit time domain.
 */
int sched_core_register(struct sched_core *core, struct sched_task *task);

/**
 * @brief Unregister a task from its owning core.
 *
 * Any pending release is dropped. The task remains initialized and may be reconfigured or
 * registered again. Self-unregistration from the task handler is supported.
 *
 * @param[in,out] core Core expected to own `task`.
 * @param[in,out] task Initialized task to remove.
 *
 * @retval 0 Task unregistered successfully.
 * @retval -EINVAL A handle is invalid.
 * @retval -ENOENT The task is not a member of `core`.
 */
int sched_core_unregister(struct sched_core *core, struct sched_task *task);

/* Task Control --------------------------------------------------------------------------------- */

/* Control APIs serialize through the installed hooks and never call task handlers. */

/**
 * @brief Change the phase used by future registration and restart operations.
 *
 * For a registered task, this call drops a pending release and reseeds the next release from the
 * current core time. For an unregistered task, only the stored phase is changed.
 *
 * @param[in,out] task Initialized task.
 * @param[in] phase_ticks New phase in the range `1..period_ticks`.
 *
 * @retval 0 Phase updated successfully.
 * @retval -EINVAL The task or phase is invalid.
 * @retval -EFAULT The registered task refers to an invalid core.
 * @retval -ERANGE Reseeding would overflow the 64-bit time domain.
 */
int sched_task_set_phase(struct sched_task *task, uint64_t phase_ticks);

/**
 * @brief Enable a task without manufacturing release backlog.
 *
 * If the stored release is stale, the task is fast-forwarded to the first future release window.
 * Calling this function for an already enabled task is a successful no-op.
 *
 * @param[in,out] task Initialized task.
 *
 * @retval 0 Task enabled successfully.
 * @retval -EINVAL `task` is invalid.
 * @retval -EFAULT The registered task refers to an invalid core.
 * @retval -ERANGE Fast-forward arithmetic would overflow the time domain.
 */
int sched_task_enable(struct sched_task *task);

/**
 * @brief Disable a task and drop any pending release.
 *
 * @param[in,out] task Initialized task.
 *
 * @retval 0 Task disabled successfully.
 * @retval -EINVAL `task` is invalid.
 * @retval -EFAULT The registered task refers to an invalid core.
 */
int sched_task_disable(struct sched_task *task);

/**
 * @brief Restart a registered task from the current core time.
 *
 * The next release becomes `core_now + phase_ticks`, and any pending release is discarded.
 *
 * @param[in,out] task Initialized registered task.
 *
 * @retval 0 Task restarted successfully.
 * @retval -EINVAL `task` is invalid.
 * @retval -ENOENT `task` is not registered.
 * @retval -EFAULT The task refers to an invalid core.
 * @retval -ERANGE Reseeding would overflow the 64-bit time domain.
 */
int sched_task_restart(struct sched_task *task);

/**
 * @brief Clear one task's dispatch and overrun statistics.
 *
 * @param[in,out] task Initialized task; registration is optional.
 *
 * @retval 0 Statistics cleared successfully.
 * @retval -EINVAL `task` is invalid.
 * @retval -EFAULT The registered task refers to an invalid core.
 */
int sched_task_clear_stats(struct sched_task *task);

/* Runtime Step --------------------------------------------------------------------------------- */

/* Tick APIs release work; run APIs execute handlers later from cooperative task context. */

/**
 * @brief Advance the core by a nonzero 32-bit tick delta.
 *
 * This compatibility wrapper is equivalent to `sched_core_tick64(core, delta_ticks)`.
 * The function does not acquire caller lock hooks and is suitable for the documented serialized
 * tick context, including an ISR protected by task-context hook masking.
 *
 * @param[in,out] core Initialized core.
 * @param[in] delta_ticks Nonzero elapsed tick count.
 *
 * @return `0` on success, or the negative `-errno` result from `sched_core_tick64()`.
 */
int sched_core_tick(struct sched_core *core, uint32_t delta_ticks);

/**
 * @brief Transactionally advance the core by a nonzero 64-bit tick delta.
 *
 * Due tasks become pending, but no handler is called. All potentially failing deadline arithmetic
 * is validated before any time, deadline, pending flag, ready count, or overrun statistic is
 * committed.
 *
 * @param[in,out] core Initialized core.
 * @param[in] delta_ticks Nonzero elapsed tick count.
 *
 * @retval 0 Tick state committed successfully.
 * @retval -EINVAL `core` is invalid or `delta_ticks` is zero.
 * @retval -ERANGE Core time or a future release would overflow the 64-bit time domain.
 * @retval -EFAULT Internal task ownership or list state is inconsistent.
 */
int sched_core_tick64(struct sched_core *core, uint64_t delta_ticks);

/**
 * @brief Dispatch all currently pending tasks in priority/FIFO order.
 *
 * @param[in,out] core Initialized core.
 * @param[out] run_count Optional destination for the number of handlers called by this invocation.
 *
 * @return `0` on success, or a negative `-errno` value from the budgeted dispatch engine.
 */
int sched_core_run(struct sched_core *core, size_t *run_count);

/**
 * @brief Dispatch at most `budget` currently pending tasks.
 *
 * Handlers execute outside the lock. A nested run call on the same core fails rather than entering
 * a second dispatch loop. `budget == 0` is a successful no-op.
 *
 * @param[in,out] core Initialized core.
 * @param[in] budget Maximum number of handlers to call.
 * @param[out] run_count Optional destination for the number of handlers called by this invocation.
 *
 * @retval 0 Dispatch completed or no work was ready.
 * @retval -EINVAL `core` is invalid.
 * @retval -EBUSY The same core is already dispatching.
 * @retval -EFAULT A cached ready task has inconsistent lifecycle or ownership state.
 */
int sched_core_run_budget(struct sched_core *core, size_t budget, size_t *run_count);

/* Idle Integration ----------------------------------------------------------------------------- */

/* Idle callbacks always run outside locks; race-free sleep entry remains platform-owned. */

/**
 * @brief Call the idle hook outside the lock when no task is ready.
 *
 * The ready check is serialized, but the hook executes after unlock. This helper therefore cannot
 * close every platform-specific check-then-sleep race by itself.
 *
 * @param[in,out] core Initialized core.
 * @param[out] did_idle Optional flag set to `true` only when the hook was called.
 *
 * @retval 0 Idle state evaluated successfully.
 * @retval -EINVAL `core` is invalid.
 */
int sched_core_idle(struct sched_core *core, bool *did_idle);

/**
 * @brief Preserve the historical atomic-idle symbol as an unlocked compatibility alias.
 *
 * Themis forbids invoking external callbacks while a component lock is held. This function now
 * delegates to `sched_core_idle()` and therefore does not provide atomic sleep entry. New code must
 * use `sched_core_idle()` with a platform-proven race-free idle hook or keep the complete sleep
 * protocol in the BSP/platform owner.
 *
 * @param[in,out] core Initialized core.
 * @param[out] did_idle Optional flag set to `true` only when the hook was called.
 *
 * @return `0` on success, or the negative `-errno` result from `sched_core_idle()`.
 *
 * @deprecated Use `sched_core_idle()`; this compatibility name will be removed in a future major
 * version.
 */
int sched_core_idle_atomic(struct sched_core *core, bool *did_idle);

/* Snapshot / Diagnostics ----------------------------------------------------------------------- */

/* Snapshot APIs copy state under the installed lock. Mutable implementation data stays private. */

/**
 * @brief Copy a consistent snapshot of one core.
 *
 * @param[in,out] core Initialized core.
 * @param[out] info Caller-owned destination written completely on success.
 *
 * @retval 0 Snapshot copied successfully.
 * @retval -EINVAL A pointer is invalid.
 */
int sched_core_get_info(struct sched_core *core, struct sched_core_info *info);

/**
 * @brief Copy a consistent snapshot of one initialized task.
 *
 * @param[in] task Initialized task.
 * @param[out] info Caller-owned destination written completely on success.
 *
 * @retval 0 Snapshot copied successfully.
 * @retval -EINVAL A pointer or task object is invalid.
 * @retval -EFAULT A registered task refers to an invalid core.
 */
int sched_task_get_info(const struct sched_task *task, struct sched_task_info *info);

/**
 * @brief Copy the task snapshot that the next dispatch step would select.
 *
 * @param[in,out] core Initialized core.
 * @param[out] info Caller-owned destination written completely on success.
 *
 * @retval 0 Snapshot copied successfully.
 * @retval -EINVAL A pointer is invalid.
 * @retval -ENOENT No task is currently ready.
 * @retval -EFAULT The cached ready task is invalid.
 */
int sched_core_peek_next_ready(struct sched_core *core, struct sched_task_info *info);

/**
 * @brief Copy the task snapshot at one priority/FIFO list index.
 *
 * @param[in,out] core Initialized core.
 * @param[in] index Zero-based index in scheduler list order.
 * @param[out] info Caller-owned destination written completely on success.
 *
 * @retval 0 Snapshot copied successfully.
 * @retval -EINVAL A pointer is invalid.
 * @retval -ENOENT `index` is outside the current task list.
 * @retval -EFAULT The selected task is invalid.
 */
int sched_core_get_task_info_at(
    struct sched_core *core,
    size_t index,
    struct sched_task_info *info);

/**
 * @brief Validate task-list ordering, ownership, flags, counters, and the ready cache.
 *
 * This diagnostic API is deterministic and allocation-free but scans the complete task list. It
 * is intended for host tests, diagnostics, maintenance commands, and fault investigation rather
 * than every hot-path iteration.
 *
 * @param[in,out] core Initialized core.
 *
 * @retval 0 All checked invariants hold.
 * @retval -EINVAL `core` is invalid.
 * @retval -EFAULT A list, ownership, ordering, flag, count, or ready-cache invariant is broken.
 */
int sched_core_validate(struct sched_core *core);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */