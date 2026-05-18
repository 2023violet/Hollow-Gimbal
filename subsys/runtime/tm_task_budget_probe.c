/**
 * @file    tm_task_budget_probe.c
 * @brief   Shared RTOS heap/stack budget probe helper
 * @version v1.0.0
 * @date    2026.04.14
 */

#include "tm_task_budget_probe.h"
#include <assert.h>
#include <string.h>
#include "FreeRTOS.h"
#include "tm_runtime.h"

size_t xPortGetMinimumEverFreeHeapSize(void);

struct tm_task_budget_entry {
    const char  *name;
    osThreadId_t thread_id;
    uint32_t     stack_size_bytes;
};

static struct tm_task_budget_entry _tm_task_budget_entries[TM_TASK_BUDGET_PROBE_MAX_TASKS];
static uint8_t                     _tm_task_budget_count = 0U;

void tm_task_budget_probe_register(const char  *name,
                                   osThreadId_t thread_id,
                                   uint32_t     stack_size_bytes)
{
    uint32_t level;

    if ((name == NULL) || (thread_id == NULL) || (stack_size_bytes == 0U)) {
        return;
    }

    level = tm_irq_critical_enter();

    for (uint32_t i = 0U; i < _tm_task_budget_count; i++) {
        if ((_tm_task_budget_entries[i].thread_id == thread_id) ||
            (strcmp(_tm_task_budget_entries[i].name, name) == 0)) {
            _tm_task_budget_entries[i].name             = name;
            _tm_task_budget_entries[i].thread_id        = thread_id;
            _tm_task_budget_entries[i].stack_size_bytes = stack_size_bytes;
            tm_irq_critical_exit(level);
            return;
        }
    }

    assert(_tm_task_budget_count < TM_TASK_BUDGET_PROBE_MAX_TASKS);
    if (_tm_task_budget_count < TM_TASK_BUDGET_PROBE_MAX_TASKS) {
        _tm_task_budget_entries[_tm_task_budget_count].name             = name;
        _tm_task_budget_entries[_tm_task_budget_count].thread_id        = thread_id;
        _tm_task_budget_entries[_tm_task_budget_count].stack_size_bytes = stack_size_bytes;
        _tm_task_budget_count++;
    }

    tm_irq_critical_exit(level);
}

bool tm_task_budget_probe_snapshot(struct tm_task_budget_snapshot *snapshot)
{
    struct tm_task_budget_entry entries[TM_TASK_BUDGET_PROBE_MAX_TASKS];
    uint8_t                     entry_count;
    uint32_t                    level;

    if (snapshot == NULL) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    level       = tm_irq_critical_enter();
    entry_count = _tm_task_budget_count;
    if (entry_count > TM_TASK_BUDGET_PROBE_MAX_TASKS) {
        entry_count = TM_TASK_BUDGET_PROBE_MAX_TASKS;
    }
    memcpy(entries, _tm_task_budget_entries, sizeof(entries));
    tm_irq_critical_exit(level);

    snapshot->heap_min_ever_free_bytes = xPortGetMinimumEverFreeHeapSize();
    snapshot->task_count               = entry_count;

    for (uint32_t i = 0U; i < entry_count; i++) {
        snapshot->tasks[i].name             = entries[i].name;
        snapshot->tasks[i].stack_size_bytes = entries[i].stack_size_bytes;
        snapshot->tasks[i].stack_high_water_free_bytes =
            osThreadGetStackSpace(entries[i].thread_id);
    }

    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
