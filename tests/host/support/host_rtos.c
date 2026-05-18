#include "host_rtos.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "task.h"

struct host_mutex {
    uint32_t attr_bits;
    int32_t  depth;
};

struct host_semaphore {
    uint32_t max_count;
    uint32_t count;
};

struct host_message_queue {
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t msg_size;
    uint32_t count;
    uint32_t head;
    uint32_t tail;
};

struct host_thread {
    osThreadFunc_t func;
    void          *argument;
    const char    *name;
    uint32_t       stack_space_bytes;
};

static uint32_t        g_ipsr                      = 0U;
static uint32_t        g_tick_count                = 0U;
static BaseType_t      g_scheduler_state           = taskSCHEDULER_RUNNING;
static osKernelState_t g_kernel_state              = osKernelRunning;
static osStatus_t      g_next_mutex_acquire_status = osOK;

static uint32_t    g_suspend_all_count         = 0U;
static uint32_t    g_resume_all_count          = 0U;
static uint32_t    g_task_critical_enter_count = 0U;
static uint32_t    g_task_critical_exit_count  = 0U;
static uint32_t    g_isr_critical_enter_count  = 0U;
static uint32_t    g_isr_critical_exit_count   = 0U;
static uint32_t    g_thread_create_count       = 0U;
static const char *g_last_thread_name          = NULL;
static size_t      g_free_heap                 = 0U;
static size_t      g_min_ever_free_heap        = 0U;
static uint32_t    g_vtask_delay_count         = 0U;
static TickType_t  g_vtask_delay_total_ticks   = 0U;
static uint32_t    g_fail_allocations_remaining = 0U;

void host_rtos_reset(void)
{
    g_ipsr                      = 0U;
    g_tick_count                = 0U;
    g_scheduler_state           = taskSCHEDULER_RUNNING;
    g_kernel_state              = osKernelRunning;
    g_next_mutex_acquire_status = osOK;
    g_suspend_all_count         = 0U;
    g_resume_all_count          = 0U;
    g_task_critical_enter_count = 0U;
    g_task_critical_exit_count  = 0U;
    g_isr_critical_enter_count  = 0U;
    g_isr_critical_exit_count   = 0U;
    g_thread_create_count       = 0U;
    g_last_thread_name          = NULL;
    g_free_heap                 = 0U;
    g_min_ever_free_heap        = 0U;
    g_vtask_delay_count         = 0U;
    g_vtask_delay_total_ticks   = 0U;
    g_fail_allocations_remaining = 0U;
}

void host_rtos_set_ipsr(uint32_t ipsr)
{
    g_ipsr = ipsr;
}

uint32_t host_rtos_get_ipsr(void)
{
    return g_ipsr;
}

void host_rtos_set_tick_count(uint32_t ticks)
{
    g_tick_count = ticks;
}

uint32_t host_rtos_get_tick_count(void)
{
    return g_tick_count;
}

void host_rtos_advance_ticks(uint32_t delta)
{
    g_tick_count += delta;
}

void host_rtos_set_scheduler_state(BaseType_t state)
{
    g_scheduler_state = state;
}

BaseType_t host_rtos_get_scheduler_state(void)
{
    return g_scheduler_state;
}

void host_rtos_set_kernel_state(osKernelState_t state)
{
    g_kernel_state = state;
}

osKernelState_t host_rtos_get_kernel_state(void)
{
    return g_kernel_state;
}

void host_rtos_fail_next_mutex_acquire(osStatus_t status)
{
    g_next_mutex_acquire_status = status;
}

void host_rtos_set_free_heap(size_t bytes)
{
    g_free_heap = bytes;
}

size_t host_rtos_get_free_heap(void)
{
    return g_free_heap;
}

void host_rtos_set_min_ever_free_heap(size_t bytes)
{
    g_min_ever_free_heap = bytes;
}

size_t host_rtos_get_min_ever_free_heap(void)
{
    return g_min_ever_free_heap;
}

void host_rtos_fail_next_allocations(uint32_t count)
{
    g_fail_allocations_remaining = count;
}

void host_rtos_set_thread_stack_space(osThreadId_t thread_id, uint32_t bytes)
{
    struct host_thread *thread = (struct host_thread *)thread_id;

    if (thread == NULL) {
        return;
    }

    thread->stack_space_bytes = bytes;
}

uint32_t host_rtos_get_suspend_all_count(void)
{
    return g_suspend_all_count;
}

uint32_t host_rtos_get_resume_all_count(void)
{
    return g_resume_all_count;
}

uint32_t host_rtos_get_task_critical_enter_count(void)
{
    return g_task_critical_enter_count;
}

uint32_t host_rtos_get_task_critical_exit_count(void)
{
    return g_task_critical_exit_count;
}

uint32_t host_rtos_get_isr_critical_enter_count(void)
{
    return g_isr_critical_enter_count;
}

uint32_t host_rtos_get_isr_critical_exit_count(void)
{
    return g_isr_critical_exit_count;
}

uint32_t host_rtos_get_thread_create_count(void)
{
    return g_thread_create_count;
}

const char *host_rtos_last_thread_name(void)
{
    return g_last_thread_name;
}

uint32_t host_rtos_get_vtask_delay_count(void)
{
    return g_vtask_delay_count;
}

TickType_t host_rtos_get_vtask_delay_total_ticks(void)
{
    return g_vtask_delay_total_ticks;
}

void host_rtos_task_enter_critical(void)
{
    g_task_critical_enter_count++;
}

void host_rtos_task_exit_critical(void)
{
    g_task_critical_exit_count++;
}

uint32_t host_rtos_task_enter_critical_from_isr(void)
{
    g_isr_critical_enter_count++;
    return g_isr_critical_enter_count;
}

void host_rtos_task_exit_critical_from_isr(uint32_t level)
{
    (void)level;
    g_isr_critical_exit_count++;
}

uint32_t __get_IPSR(void)
{
    return g_ipsr;
}

BaseType_t xTaskGetSchedulerState(void)
{
    return g_scheduler_state;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)g_tick_count;
}

TickType_t xTaskGetTickCountFromISR(void)
{
    return (TickType_t)g_tick_count;
}

void vTaskSuspendAll(void)
{
    g_suspend_all_count++;
}

BaseType_t xTaskResumeAll(void)
{
    g_resume_all_count++;
    return pdFALSE;
}

void vTaskDelay(TickType_t ticks)
{
    g_vtask_delay_count++;
    g_vtask_delay_total_ticks += ticks;
    host_rtos_advance_ticks((uint32_t)ticks);
}

void *pvPortMalloc(size_t size)
{
    if (g_fail_allocations_remaining > 0U) {
        g_fail_allocations_remaining--;
        return NULL;
    }

    return malloc(size);
}

void vPortFree(void *ptr)
{
    free(ptr);
}

size_t xPortGetMinimumEverFreeHeapSize(void)
{
    return g_min_ever_free_heap;
}

size_t xPortGetFreeHeapSize(void)
{
    return g_free_heap;
}

osKernelState_t osKernelGetState(void)
{
    return g_kernel_state;
}

uint32_t osKernelGetTickCount(void)
{
    return g_tick_count;
}

uint32_t osThreadGetCount(void)
{
    return g_thread_create_count;
}

osStatus_t osDelay(uint32_t ticks)
{
    host_rtos_advance_ticks(ticks);
    return osOK;
}

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    struct host_mutex *mutex = (struct host_mutex *)calloc(1U, sizeof(*mutex));

    if (mutex == NULL) {
        return NULL;
    }

    if (attr != NULL) {
        mutex->attr_bits = attr->attr_bits;
    }

    return (osMutexId_t)mutex;
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
    struct host_mutex *mutex = (struct host_mutex *)mutex_id;

    (void)timeout;
    if (mutex == NULL) {
        return osErrorParameter;
    }

    if (g_next_mutex_acquire_status != osOK) {
        osStatus_t injected = g_next_mutex_acquire_status;

        g_next_mutex_acquire_status = osOK;
        return injected;
    }

    if ((mutex->depth > 0) && ((mutex->attr_bits & osMutexRecursive) == 0U)) {
        return (timeout == 0U) ? osErrorResource : osErrorTimeout;
    }

    mutex->depth++;
    return osOK;
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
    struct host_mutex *mutex = (struct host_mutex *)mutex_id;

    if (mutex == NULL) {
        return osErrorParameter;
    }
    if (mutex->depth > 0) {
        mutex->depth--;
    }

    return osOK;
}

osStatus_t osMutexDelete(osMutexId_t mutex_id)
{
    free(mutex_id);
    return osOK;
}

osSemaphoreId_t osSemaphoreNew(uint32_t                 max_count,
                               uint32_t                 initial_count,
                               const osSemaphoreAttr_t *attr)
{
    struct host_semaphore *semaphore;

    (void)attr;
    if ((max_count == 0U) || (initial_count > max_count)) {
        return NULL;
    }

    semaphore = (struct host_semaphore *)calloc(1U, sizeof(*semaphore));
    if (semaphore == NULL) {
        return NULL;
    }

    semaphore->max_count = max_count;
    semaphore->count     = initial_count;
    return (osSemaphoreId_t)semaphore;
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)
{
    struct host_semaphore *semaphore = (struct host_semaphore *)semaphore_id;

    if (semaphore == NULL) {
        return osErrorParameter;
    }
    if (semaphore->count == 0U) {
        return (timeout == 0U) ? osErrorResource : osErrorTimeout;
    }

    semaphore->count--;
    return osOK;
}

osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)
{
    struct host_semaphore *semaphore = (struct host_semaphore *)semaphore_id;

    if (semaphore == NULL) {
        return osErrorParameter;
    }
    if (semaphore->count >= semaphore->max_count) {
        return osErrorResource;
    }

    semaphore->count++;
    return osOK;
}

osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id)
{
    free(semaphore_id);
    return osOK;
}

osMessageQueueId_t osMessageQueueNew(uint32_t                    msg_count,
                                     uint32_t                    msg_size,
                                     const osMessageQueueAttr_t *attr)
{
    struct host_message_queue *queue;

    (void)attr;
    if ((msg_count == 0U) || (msg_size == 0U)) {
        return NULL;
    }

    queue = (struct host_message_queue *)calloc(1U, sizeof(*queue));
    if (queue == NULL) {
        return NULL;
    }

    queue->buffer = (uint8_t *)calloc((size_t)msg_count, (size_t)msg_size);
    if (queue->buffer == NULL) {
        free(queue);
        return NULL;
    }

    queue->capacity = msg_count;
    queue->msg_size = msg_size;
    return (osMessageQueueId_t)queue;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id,
                             const void        *msg_ptr,
                             uint8_t            msg_prio,
                             uint32_t           timeout)
{
    struct host_message_queue *queue = (struct host_message_queue *)mq_id;
    uint8_t                   *slot;

    (void)msg_prio;
    (void)timeout;
    if ((queue == NULL) || (msg_ptr == NULL)) {
        return osErrorParameter;
    }
    if (queue->count >= queue->capacity) {
        return osErrorResource;
    }

    slot = queue->buffer + ((size_t)queue->tail * (size_t)queue->msg_size);
    memcpy(slot, msg_ptr, queue->msg_size);
    queue->tail = (queue->tail + 1U) % queue->capacity;
    queue->count++;

    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id,
                             void              *msg_ptr,
                             uint8_t           *msg_prio,
                             uint32_t           timeout)
{
    struct host_message_queue *queue = (struct host_message_queue *)mq_id;
    uint8_t                   *slot;

    if ((queue == NULL) || (msg_ptr == NULL)) {
        return osErrorParameter;
    }
    if (queue->count == 0U) {
        return (timeout == 0U) ? osErrorResource : osErrorTimeout;
    }

    slot = queue->buffer + ((size_t)queue->head * (size_t)queue->msg_size);
    memcpy(msg_ptr, slot, queue->msg_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    queue->count--;

    if (msg_prio != NULL) {
        *msg_prio = 0U;
    }

    return osOK;
}

osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)
{
    struct host_message_queue *queue = (struct host_message_queue *)mq_id;

    if (queue == NULL) {
        return osErrorParameter;
    }

    free(queue->buffer);
    free(queue);
    return osOK;
}

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr)
{
    struct host_thread *thread;

    if (func == NULL) {
        return NULL;
    }

    thread = (struct host_thread *)calloc(1U, sizeof(*thread));
    if (thread == NULL) {
        return NULL;
    }

    thread->func     = func;
    thread->argument = argument;
    thread->name     = (attr != NULL) ? attr->name : NULL;

    g_thread_create_count++;
    g_last_thread_name = thread->name;

    return (osThreadId_t)thread;
}

void osThreadExit(void)
{
    abort();
}

uint32_t osThreadGetStackSpace(osThreadId_t thread_id)
{
    const struct host_thread *thread = (const struct host_thread *)thread_id;

    if (thread == NULL) {
        return 0U;
    }

    return thread->stack_space_bytes;
}
