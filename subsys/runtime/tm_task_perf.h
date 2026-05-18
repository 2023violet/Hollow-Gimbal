/**
 * @file    tm_task_perf.h
 * @brief   Shared task runtime observation helper
 * @version v1.0.0
 * @date    2026.04.14
 *
 * Design overview
 * ---------------
 * This helper keeps task runtime observation lightweight and consistent across
 * RTOS task owners:
 * - one compile-time switch in `config/tm_core_config.h`
 * - one debugger-visible snapshot per observed region
 * - no-op call sites when observation is disabled
 *
 * The intent is to observe task-owned work regions, not to build a heavy
 * system-wide trace framework. Deliberate waits such as `osDelay()` should
 * stay outside the measured region whenever possible.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Task owners place these macros around task-local work regions only.
 * - This helper has no thread of its own and keeps no product state.
 * - Debugger-visible snapshots are caller-owned static storage.
 *
 * Blocking / resources / diagnostics
 * ----------------------------------
 * - Enabled builds only measure caller-owned inline regions; no heap, queue,
 *   or background task is introduced by this helper.
 * - Disabled builds collapse to no-op macros so shared call sites can stay
 *   uniform without carrying firmware-only branches.
 * - Diagnostics live in the caller-owned `tm_task_perf_snapshot` storage.
 *
 * Failure / recovery
 * ------------------
 * - Null region pointers degrade to bounded no-op behavior rather than hidden
 *   crashes or auto-allocation.
 * - Time rounding saturates explicitly at `UINT32_MAX`; the helper owns no
 *   retry or overflow recovery policy.
 * - Observed-region placement remains the task owner's responsibility.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "config/tm_core_config.h"

#if (TM_TASK_PERF_ENABLE == 1U)
#include "core_time.h"
#include "tm_runtime.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Debugger-visible runtime snapshot for one task-owned region.
 *
 * `sample_count == 0` means the region has not completed a measurement yet.
 */
struct tm_task_perf_snapshot {
    uint32_t last_time_us;
    uint32_t max_time_us;
    uint32_t min_time_us;
    uint32_t sample_count;
};

#if (TM_TASK_PERF_ENABLE == 1U)

/**
 * @brief  Measurement storage for one observed task region.
 */
struct tm_task_perf_region {
    struct core_time_perf                 probe;
    volatile struct tm_task_perf_snapshot snapshot;
};

#define TM_TASK_PERF_REGION_INIT      \
    {                                 \
        .probe    = {0U, 0U},         \
        .snapshot = {0U, 0U, 0U, 0U}, \
    }

#define TM_TASK_PERF_REGION_DEFINE(name) \
    static struct tm_task_perf_region name = TM_TASK_PERF_REGION_INIT

#define TM_TASK_PERF_BEGIN(region) tm_task_perf_begin(&(region))
#define TM_TASK_PERF_END(region)   tm_task_perf_end(&(region))

static inline uint32_t tm_task_perf_round_us(float elapsed_us)
{
    if (elapsed_us <= 0.0f) {
        return 0U;
    }

    if (elapsed_us >= 4294967040.0f) {
        return UINT32_MAX;
    }

    return (uint32_t)(elapsed_us + 0.5f);
}

static inline void tm_task_perf_begin(struct tm_task_perf_region *region)
{
    if (region == NULL) {
        return;
    }

    core_time_perf_start(&region->probe);
}

static inline void tm_task_perf_end(struct tm_task_perf_region *region)
{
    struct tm_task_perf_snapshot snapshot;
    uint32_t                     level;
    uint32_t elapsed_us;

    if (region == NULL) {
        return;
    }

    core_time_perf_stop(&region->probe);
    elapsed_us = tm_task_perf_round_us(core_time_perf_get_us(&region->probe));

    snapshot = region->snapshot;
    snapshot.last_time_us = elapsed_us;
    if ((snapshot.sample_count == 0U) || (elapsed_us < snapshot.min_time_us)) {
        snapshot.min_time_us = elapsed_us;
    }
    if (elapsed_us > snapshot.max_time_us) {
        snapshot.max_time_us = elapsed_us;
    }
    if (snapshot.sample_count < UINT32_MAX) {
        snapshot.sample_count++;
    }

    level            = tm_irq_critical_enter();
    region->snapshot = snapshot;
    tm_irq_critical_exit(level);
}

#else

#define TM_TASK_PERF_REGION_DEFINE(name)
#define TM_TASK_PERF_BEGIN(region) ((void)0)
#define TM_TASK_PERF_END(region)   ((void)0)

#endif

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
