/**
 * @file    tm_runtime.c
 * @brief   Themis runtime context, tick, heap, and critical-section bridge
 * @version v1.2.1
 * @date    2026.03.31
 *
 * Implementation role
 * -------------------
 * This file deliberately centralizes the small set of RTOS/CPU-context checks
 * that many Themis layers need. The objective is to keep higher layers
 * focused on their own contracts rather than re-deriving "ISR or task",
 * "scheduler up yet", and "which critical section primitive is legal
 * here" in each module.
 *
 * Key collaborators
 * -----------------
 * - `tm_runtime.h`: public runtime bridge contract
 * - FreeRTOS / CMSIS-RTOS2 headers: actual scheduler and context primitives
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_runtime.h"
#include <errno.h>
#include <string.h>
#include "cmsis_os2.h"

#if defined(__CC_ARM) || defined(__CLANG_ARM) || defined(__GNUC__)
#include "cmsis_compiler.h"
#endif
#include "FreeRTOS.h"
#include "task.h"

TM_STATIC_ASSERT(((uint32_t)TM_TICK_PER_SECOND == (uint32_t)configTICK_RATE_HZ),
                 "TM_TICK_PER_SECOND must match FreeRTOS configTICK_RATE_HZ");

/* Public Functions ----------------------------------------------------------------------------- */

bool tm_is_in_isr(void)
{
    return (__get_IPSR() != 0U);
}

bool tm_scheduler_is_running(void)
{
    return (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED);
}

uint32_t tm_tick_get(void)
{
    if (!tm_scheduler_is_running()) {
        /* Pre-scheduler boot has no meaningful RTOS tick timeline yet. */
        return 0U;
    }

    if (tm_is_in_isr()) {
        /* FreeRTOS provides a dedicated ISR-safe accessor for this path. */
        return (uint32_t)xTaskGetTickCountFromISR();
    }

    /* Keep task and ISR paths on the same native kernel tick source. */
    return (uint32_t)xTaskGetTickCount();
}

uint32_t tm_tick_get_ms(void)
{
    if (!tm_scheduler_is_running()) {
        return 0U;
    }

    return tm_ticks_to_ms(tm_tick_get());
}

uint32_t tm_tick_hz_get(void)
{
    return (uint32_t)configTICK_RATE_HZ;
}

uint32_t tm_ms_to_ticks_ceil(uint32_t milliseconds)
{
    if (milliseconds == 0U) {
        return 0U;
    }

#if (TM_TICK_PER_SECOND == 1000U)
    return milliseconds;
#else
    uint32_t tick_hz;
    uint64_t scaled_ticks;

    tick_hz = tm_tick_hz_get();
    if (tick_hz == 0U) {
        return 0U;
    }

    scaled_ticks = ((uint64_t)milliseconds * (uint64_t)tick_hz) + 999ULL;
    scaled_ticks /= 1000ULL;
    if (scaled_ticks == 0ULL) {
        return 1U;
    }
    if (scaled_ticks > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }

    return (uint32_t)scaled_ticks;
#endif
}

uint32_t tm_ticks_to_ms(uint32_t ticks)
{
    if (ticks == 0U) {
        return 0U;
    }

#if (TM_TICK_PER_SECOND == 1000U)
    return ticks;
#else
    uint32_t tick_hz;
    uint64_t scaled_ms;

    tick_hz = tm_tick_hz_get();
    if (tick_hz == 0U) {
        return 0U;
    }

    scaled_ms = ((uint64_t)ticks * 1000ULL) / (uint64_t)tick_hz;
    if (scaled_ms == 0ULL) {
        return 1U;
    }
    if (scaled_ms > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }

    return (uint32_t)scaled_ms;
#endif
}

uint32_t tm_tick_elapsed_ms_update(uint32_t *last_tick)
{
    uint32_t current_tick;
    uint32_t elapsed_ms;

    if (last_tick == NULL) {
        return 0U;
    }

    current_tick = tm_tick_get();
    elapsed_ms   = tm_ticks_to_ms(current_tick - *last_tick);
    if ((elapsed_ms == 0U) && (current_tick != *last_tick)) {
        elapsed_ms = 1U;
    }

    *last_tick = current_tick;
    return elapsed_ms;
}

void *tm_malloc(uint32_t size)
{
    if ((size == 0U) || tm_is_in_isr()) {
        /* Heap access from ISR is intentionally rejected project-wide. */
        return NULL;
    }

    return pvPortMalloc(size);
}

void tm_free(void *memory)
{
    if ((memory != NULL) && !tm_is_in_isr()) {
        vPortFree(memory);
    }
}

void *tm_calloc(uint32_t n, uint32_t size)
{
    void    *memory;
    uint64_t total_size;

    if ((n == 0U) || (size == 0U) || tm_is_in_isr()) {
        return NULL;
    }

    total_size = (uint64_t)n * (uint64_t)size;
    if (total_size > UINT32_MAX) {
        return NULL;
    }

    memory = tm_malloc((uint32_t)total_size);
    if (memory != NULL) {
        memset(memory, 0, (size_t)total_size);
    }

    return memory;
}

void tm_task_critical_enter(void)
{
    if (!tm_is_in_isr() && tm_scheduler_is_running()) {
        /* Scheduler suspension is only meaningful once task scheduling exists. */
        vTaskSuspendAll();
    }
}

void tm_task_critical_exit(void)
{
    if (!tm_is_in_isr() && tm_scheduler_is_running()) {
        (void)xTaskResumeAll();
    }
}

uint32_t tm_irq_critical_enter(void)
{
    if (tm_is_in_isr()) {
        /* Preserve and return the ISR nesting token expected by FreeRTOS. */
        return taskENTER_CRITICAL_FROM_ISR();
    }

    /* Thread-mode callers still get the same API surface without extra wrappers. */
    taskENTER_CRITICAL();
    return 0U;
}

void tm_irq_critical_exit(uint32_t level)
{
    if (tm_is_in_isr()) {
        taskEXIT_CRITICAL_FROM_ISR(level);
        return;
    }

    /* Task-context calls ignore the synthetic token returned by enter(). */
    TM_UNUSED(level);
    taskEXIT_CRITICAL();
}

/*
 * newlib _write syscall stub.
 *
 * Rationale for placement here rather than in bsp/:
 * This stub satisfies the newlib weak symbol that printf() pulls in on
 * ARM bare-metal targets. The Themis BSP layer is the logically correct home,
 * but placing it there creates a hard link-time dependency between the BSP and
 * the C library's syscall interface, which couples two otherwise orthogonal
 * layers. Instead, it lives here as a safe no-op fallback that silently
 * discards output until the application installs a real backend via
 * tm_log_init() or a BSP-level UART write hook. Any BSP that provides its own
 * _write will override this via the linker's weak-symbol resolution.
 *
 * The implementation reports `-ENOSYS` so early callers do not mistake the
 * lack of a real backend for successful evidence emission.
 */
static int _tm_runtime_missing_backend_write(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;

    return -ENOSYS;
}

#if defined(THEMIS_HOST_TEST)
int tm_runtime_weak_write_stub_for_test(int file, char *ptr, int len)
{
    return _tm_runtime_missing_backend_write(file, ptr, len);
}
#endif

TM_WEAK int _write(int file, char *ptr, int len)
{
    return _tm_runtime_missing_backend_write(file, ptr, len);
}

/* ---------------------------------------- end of file ----------------------------------------- */
