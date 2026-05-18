#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cmsis_os2.h"
#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

void            host_rtos_reset(void);
void            host_rtos_set_ipsr(uint32_t ipsr);
uint32_t        host_rtos_get_ipsr(void);
void            host_rtos_set_tick_count(uint32_t ticks);
uint32_t        host_rtos_get_tick_count(void);
void            host_rtos_advance_ticks(uint32_t delta);
void            host_rtos_set_scheduler_state(BaseType_t state);
BaseType_t      host_rtos_get_scheduler_state(void);
void            host_rtos_set_kernel_state(osKernelState_t state);
osKernelState_t host_rtos_get_kernel_state(void);
void            host_rtos_fail_next_mutex_acquire(osStatus_t status);
void            host_rtos_set_free_heap(size_t bytes);
size_t          host_rtos_get_free_heap(void);
void            host_rtos_set_min_ever_free_heap(size_t bytes);
size_t          host_rtos_get_min_ever_free_heap(void);
void            host_rtos_fail_next_allocations(uint32_t count);
void            host_rtos_set_thread_stack_space(osThreadId_t thread_id, uint32_t bytes);

uint32_t    host_rtos_get_suspend_all_count(void);
uint32_t    host_rtos_get_resume_all_count(void);
uint32_t    host_rtos_get_task_critical_enter_count(void);
uint32_t    host_rtos_get_task_critical_exit_count(void);
uint32_t    host_rtos_get_isr_critical_enter_count(void);
uint32_t    host_rtos_get_isr_critical_exit_count(void);
uint32_t    host_rtos_get_thread_create_count(void);
const char *host_rtos_last_thread_name(void);
uint32_t    host_rtos_get_vtask_delay_count(void);
TickType_t  host_rtos_get_vtask_delay_total_ticks(void);

void     host_rtos_task_enter_critical(void);
void     host_rtos_task_exit_critical(void);
uint32_t host_rtos_task_enter_critical_from_isr(void);
void     host_rtos_task_exit_critical_from_isr(uint32_t level);

#ifdef __cplusplus
}
#endif
