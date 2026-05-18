/**
 * @file    sched.h
 * @brief   Opaque cooperative periodic scheduler for bare-metal loops
 * @version v2.0.0
 * @date    2026.05.01
 *
 * Design overview
 * ---------------
 * `sched` is a caller-owned cooperative scheduler for periodic task release
 * and main-loop dispatch on small MCUs. Public task and core objects are fully
 * opaque handles. Callers provide raw storage bytes, and the scheduler owns
 * every internal field inside that storage.
 *
 * The time model is explicitly 64-bit monotonic:
 * - `sched_core_tick()` advances a `uint64_t` absolute tick domain;
 * - each task stores its next release tick in the same domain;
 * - overrun and remaining-time calculations are performed in 64-bit space;
 * - no signed wraparound arithmetic is used.
 *
 * This eliminates the old "half-range wrap-safe" caveat, but it does not make
 * time infinite. If cumulative uptime approaches `UINT64_MAX`, or if adding a
 * phase / period / delta would overflow the 64-bit domain, the affected API
 * fails with `-ERANGE`. Long-lived products should choose a coarse enough tick
 * unit that `UINT64_MAX` is comfortably beyond the intended device lifetime.
 *
 * Concurrency model
 * -----------------
 * This component uses one pure strategy only: caller-supplied critical hooks
 * serialize all task-context API calls against the tick context. No internal
 * atomic fields are used.
 *
 * - If lock hooks are installed, every task-context API enters and exits them.
 * - `sched_core_tick()` never calls the hooks itself; it is expected to run
 *   either from the protected timer ISR or from the same serialized context.
 * - If lock hooks are not installed, all APIs must run from one execution
 *   context. Task/ISR split use without hooks is unsupported.
 *
 * Usage example
 * -------------
 * SCHED_CORE_STORAGE(core_mem) = {0};
 * SCHED_TASK_STORAGE(task_mem) = {0};
 * struct sched_core *core = NULL;
 * struct sched_task *task = NULL;
 *
 * if (sched_core_init(core_mem, sizeof(core_mem), NULL, &core) == 0 &&
 *     sched_task_init(task_mem,
 *                     sizeof(task_mem),
 *                     "blink",
 *                     10U,
 *                     10U,
 *                     0U,
 *                     blink_handler,
 *                     NULL,
 *                     &task) == 0 &&
 *     sched_core_register(core, task) == 0) {
 *     (void)sched_core_tick(core, 1U);
 *     (void)sched_core_run(core, NULL);
 * }
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns every raw storage buffer and every referenced callback
 *   context object.
 * - The scheduler borrows task names; it does not copy them.
 * - Task handlers and idle hooks execute outside the scheduler lock.
 * - `sched_task_set_phase()` and `sched_task_restart()` drop any current
 *   pending release for that task and reseed from the current core tick.
 * - Re-entrant only across distinct core/task instances. One core instance is
 *   not thread-safe without the documented lock hooks. Not ISR-safe except for
 *   `sched_core_tick()` in the documented split model.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no RTOS dependency.
 * - The scheduler never allocates dynamic memory and never owns sleep queues.
 * - Tick release bookkeeping is O(N) in registered task count because tasks
 *   are stored in a priority-sorted intrusive list.
 * - `sched_core_peek_next_ready()` is O(1) and reports the exact task that
 *   the next dispatch step would choose.
 *
 * Failure / recovery
 * ------------------
 * - All public APIs return `0` or negative `-errno`.
 * - Invalid handles, NULL output pointers, bad storage spans, and malformed
 *   lifecycle transitions fail with `-EINVAL`.
 * - Duplicate registration fails with `-EALREADY`.
 * - Missing membership for unregister/restart/query fails with `-ENOENT`.
 * - Time-domain overflow fails with `-ERANGE`.
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

/* Public Constants ----------------------------------------------------------------------------- */

/**
 * @brief Alignment required for scheduler core storage.
 *
 * Eight-byte alignment keeps the hidden 64-bit tick domain naturally aligned
 * on the low-end MCUs targeted by Themis.
 */
#define SCHED_CORE_STORAGE_ALIGN 8U

/**
 * @brief Alignment required for scheduler task storage.
 */
#define SCHED_TASK_STORAGE_ALIGN 8U

/**
 * @brief Byte size reserved for one opaque scheduler core.
 *
 * This constant is part of the public ABI of the opaque-storage contract.
 */
#define SCHED_CORE_STORAGE_SIZE  128U

/**
 * @brief Byte size reserved for one opaque scheduler task.
 *
 * This constant is part of the public ABI of the opaque-storage contract.
 */
#define SCHED_TASK_STORAGE_SIZE  128U

/**
 * @brief Declare aligned storage for one scheduler core.
 */
#define SCHED_CORE_STORAGE(name) TM_ALIGN(SCHED_CORE_STORAGE_ALIGN) uint8_t name[SCHED_CORE_STORAGE_SIZE]

/**
 * @brief Declare aligned storage for one scheduler task.
 */
#define SCHED_TASK_STORAGE(name) TM_ALIGN(SCHED_TASK_STORAGE_ALIGN) uint8_t name[SCHED_TASK_STORAGE_SIZE]

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Opaque scheduler core handle.
 */
struct sched_core;

/**
 * @brief Opaque scheduler task handle.
 */
struct sched_task;

/**
 * @brief Task handler executed from `sched_core_run()` or `sched_core_run_budget()`.
 *
 * The callback runs in caller context with the scheduler lock already
 * released.
 */
typedef void (*sched_task_fn_t)(void *user_data);

/**
 * @brief Optional idle hook executed only when no ready task exists.
 *
 * The callback runs in caller context with the scheduler lock already
 * released.
 */
typedef void (*sched_idle_fn_t)(void *user_data);

/**
 * @brief Optional lock hooks used to serialize task-context APIs against tick context.
 */
struct sched_lock_iface {
    /**
     * @brief Enter the caller-defined critical section.
     */
    void (*enter)(void *user_data);

    /**
     * @brief Exit the caller-defined critical section.
     */
    void (*exit)(void *user_data);

    /**
     * @brief Opaque caller-owned context forwarded to both hooks.
     */
    void *user_data;
};

/**
 * @brief Immutable core snapshot returned by `sched_core_get_info()`.
 */
struct sched_core_info {
    /**
     * @brief Current absolute tick in the 64-bit monotonic domain.
     */
    uint64_t now_ticks;

    /**
     * @brief Number of registered tasks.
     */
    size_t task_count;

    /**
     * @brief Number of enabled tasks.
     */
    size_t enabled_count;

    /**
     * @brief Number of pending and enabled tasks ready for dispatch.
     */
    size_t ready_count;

    /**
     * @brief True when caller-supplied lock hooks are installed.
     */
    bool lock_installed;
};

/**
 * @brief Immutable task snapshot returned by task/core query APIs.
 */
struct sched_task_info {
    /**
     * @brief Borrowed debug name pointer supplied at task initialisation.
     */
    const char *name;

    /**
     * @brief Fixed release period in ticks.
     */
    uint64_t period_ticks;

    /**
     * @brief Phase delay used by register/restart semantics.
     */
    uint64_t phase_ticks;

    /**
     * @brief Absolute next release tick in the core's 64-bit domain.
     *
     * Zero when the task is not registered.
     */
    uint64_t next_release_tick;

    /**
     * @brief Ticks remaining until the next release window.
     *
     * Zero when the task is pending, disabled, or not registered.
     */
    uint64_t remaining_ticks;

    /**
     * @brief Number of completed handler dispatches.
     */
    uint32_t run_count;

    /**
     * @brief Number of missed release windows while the task stayed pending.
     */
    uint32_t overrun_count;

    /**
     * @brief Lower numeric value means higher dispatch priority.
     */
    uint8_t priority;

    /**
     * @brief True when linked into a core.
     */
    bool registered;

    /**
     * @brief True when the task participates in release timing.
     */
    bool enabled;

    /**
     * @brief True when one release is waiting for dispatch.
     */
    bool pending;
};

/* Core Lifecycle -------------------------------------------------------------------------------- */

/**
 * @brief Initialise one opaque scheduler core inside caller-owned storage.
 *
 * @param[in,out] storage       Raw caller-owned storage buffer.
 * @param[in]     storage_size  Byte size of @p storage.
 * @param[in]     lock_iface    Optional lock hooks. Pass NULL for single-
 *                              context use only.
 * @param[out]    out_core      Receives the opaque core handle on success.
 *
 * @retval 0         Initialisation succeeded.
 * @retval -EINVAL   The storage span, lock interface, or output pointer was invalid.
 */
int sched_core_init(void                  *storage,
                    size_t                 storage_size,
                    const struct sched_lock_iface *lock_iface,
                    struct sched_core    **out_core);

/**
 * @brief Install or replace the optional idle hook.
 *
 * @param[in,out] core       Opaque scheduler core.
 * @param[in]     idle_hook  Idle hook, or NULL to disable it.
 * @param[in]     user_data  Opaque caller-owned context forwarded to @p idle_hook.
 *
 * @retval 0         Update succeeded.
 * @retval -EINVAL   @p core was invalid.
 */
int sched_core_set_idle_hook(struct sched_core *core, sched_idle_fn_t idle_hook, void *user_data);

/* Task Lifecycle -------------------------------------------------------------------------------- */

/**
 * @brief Initialise one opaque scheduler task inside caller-owned storage.
 *
 * @param[in,out] storage       Raw caller-owned storage buffer.
 * @param[in]     storage_size  Byte size of @p storage.
 * @param[in]     name          Borrowed short debug name. May be NULL.
 * @param[in]     period_ticks  Positive release period in ticks.
 * @param[in]     phase_ticks   Positive phase delay in ticks. Must be within
 *                              `1..period_ticks`.
 * @param[in]     priority      Lower numeric value means higher priority.
 * @param[in]     handler       Non-NULL task handler.
 * @param[in]     user_data     Opaque caller-owned context forwarded to @p handler.
 * @param[out]    out_task      Receives the opaque task handle on success.
 *
 * @retval 0         Initialisation succeeded.
 * @retval -EINVAL   The storage span or lifecycle contract was invalid.
 */
int sched_task_init(void               *storage,
                    size_t              storage_size,
                    const char         *name,
                    uint64_t            period_ticks,
                    uint64_t            phase_ticks,
                    uint8_t             priority,
                    sched_task_fn_t     handler,
                    void               *user_data,
                    struct sched_task **out_task);

/**
 * @brief Register one task into one core.
 *
 * The task is inserted into the core's priority-sorted list. Equal-priority
 * peers preserve FIFO registration order.
 *
 * @retval 0          Registration succeeded.
 * @retval -EINVAL    A handle was invalid.
 * @retval -EALREADY  The task is already registered.
 * @retval -ERANGE    Seeding the next release tick would overflow the 64-bit domain.
 */
int sched_core_register(struct sched_core *core, struct sched_task *task);

/**
 * @brief Unregister one task from one core.
 *
 * Self-unregister from the current handler is supported because handlers run
 * outside the scheduler lock.
 *
 * @retval 0         Unregister succeeded.
 * @retval -EINVAL   A handle was invalid.
 * @retval -ENOENT   The task is not registered with @p core.
 */
int sched_core_unregister(struct sched_core *core, struct sched_task *task);

/* Task Control --------------------------------------------------------------------------------- */

/**
 * @brief Update the phase delay used by register/restart semantics.
 *
 * When the task is already registered, the scheduler drops any current pending
 * release for that task and reseeds its next release from the current core
 * tick.
 *
 * @retval 0         Update succeeded.
 * @retval -EINVAL   A handle or phase contract was invalid.
 * @retval -ERANGE   Reseeding the next release tick would overflow the 64-bit domain.
 */
int sched_task_set_phase(struct sched_task *task, uint64_t phase_ticks);

/**
 * @brief Enable one task.
 *
 * If the task stayed disabled long enough for its stored deadline to become
 * stale, the scheduler fast-forwards it to the first future release window
 * without manufacturing backlog.
 *
 * @retval 0         Enable succeeded.
 * @retval -EINVAL   @p task was invalid.
 * @retval -ERANGE   Fast-forward bookkeeping overflowed the 64-bit domain.
 */
int sched_task_enable(struct sched_task *task);

/**
 * @brief Disable one task and drop any current pending release.
 *
 * @retval 0         Disable succeeded.
 * @retval -EINVAL   @p task was invalid.
 */
int sched_task_disable(struct sched_task *task);

/**
 * @brief Restart one registered task from the current core tick.
 *
 * The next release uses the task's current phase delay.
 *
 * @retval 0         Restart succeeded.
 * @retval -EINVAL   @p task was invalid.
 * @retval -ENOENT   The task is not registered.
 * @retval -ERANGE   Reseeding the next release tick would overflow the 64-bit domain.
 */
int sched_task_restart(struct sched_task *task);

/**
 * @brief Reset run and overrun statistics to zero.
 *
 * @retval 0         Reset succeeded.
 * @retval -EINVAL   @p task was invalid.
 */
int sched_task_clear_stats(struct sched_task *task);

/* Runtime Step --------------------------------------------------------------------------------- */

/**
 * @brief Advance the core's 64-bit monotonic tick domain.
 *
 * This API marks due tasks pending but never calls task handlers.
 *
 * @retval 0         Tick advance succeeded.
 * @retval -EINVAL   @p core was invalid or @p delta_ticks was zero.
 * @retval -ERANGE   The 64-bit tick domain would overflow.
 */
int sched_core_tick(struct sched_core *core, uint32_t delta_ticks);

/**
 * @brief Dispatch all currently pending tasks.
 *
 * @param[in,out] core       Opaque scheduler core.
 * @param[out]    run_count  Optional completed-dispatch count output.
 *
 * @retval 0         Dispatch completed.
 * @retval -EINVAL   @p core was invalid.
 */
int sched_core_run(struct sched_core *core, size_t *run_count);

/**
 * @brief Dispatch at most @p budget currently pending tasks.
 *
 * @param[in,out] core       Opaque scheduler core.
 * @param[in]     budget     Maximum number of handlers to dispatch.
 * @param[out]    run_count  Optional completed-dispatch count output.
 *
 * @retval 0         Dispatch completed.
 * @retval -EINVAL   @p core was invalid.
 */
int sched_core_run_budget(struct sched_core *core, size_t budget, size_t *run_count);

/**
 * @brief Execute the idle hook only when no ready work exists.
 *
 * @param[in,out] core      Opaque scheduler core.
 * @param[out]    did_idle  Optional output set to true only when the hook ran.
 *
 * @retval 0         The idle check completed.
 * @retval -EINVAL   @p core was invalid.
 */
int sched_core_idle(struct sched_core *core, bool *did_idle);

/* Snapshot / Diagnostics ----------------------------------------------------------------------- */

/**
 * @brief Copy one immutable core snapshot.
 *
 * @retval 0         Snapshot copy succeeded.
 * @retval -EINVAL   A handle or output pointer was invalid.
 */
int sched_core_get_info(const struct sched_core *core, struct sched_core_info *info);

/**
 * @brief Copy one immutable task snapshot.
 *
 * @retval 0         Snapshot copy succeeded.
 * @retval -EINVAL   A handle or output pointer was invalid.
 */
int sched_task_get_info(const struct sched_task *task, struct sched_task_info *info);

/**
 * @brief Copy the snapshot of the next task that would be dispatched.
 *
 * This is an O(1) query over the scheduler's cached ready head. The returned
 * snapshot follows the same priority/FIFO order that `sched_core_run()` and
 * `sched_core_run_budget()` use for dispatch.
 *
 * @retval 0         Snapshot copy succeeded.
 * @retval -EINVAL   A handle or output pointer was invalid.
 * @retval -ENOENT   No enabled pending task is ready for dispatch.
 */
int sched_core_peek_next_ready(const struct sched_core *core, struct sched_task_info *info);

/**
 * @brief Copy the snapshot of the task at one list index.
 *
 * Index `0` corresponds to the first task in priority/FIFO dispatch order.
 *
 * @retval 0         Snapshot copy succeeded.
 * @retval -EINVAL   A handle or output pointer was invalid.
 * @retval -ENOENT   @p index lies outside the registered task list.
 */
int sched_core_get_task_info_at(const struct sched_core *core,
                                size_t                   index,
                                struct sched_task_info  *info);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
