#pragma once

#include "FreeRTOS.h"
#include "host_rtos.h"

#ifdef __cplusplus
extern "C" {
#endif

#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_SUSPENDED   1
#define taskSCHEDULER_RUNNING     2

BaseType_t xTaskGetSchedulerState(void);
TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
void       vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
void       vTaskDelay(TickType_t ticks);

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif

#define taskENTER_CRITICAL()              host_rtos_task_enter_critical()
#define taskEXIT_CRITICAL()               host_rtos_task_exit_critical()
#define taskENTER_CRITICAL_FROM_ISR()     host_rtos_task_enter_critical_from_isr()
#define taskEXIT_CRITICAL_FROM_ISR(level) host_rtos_task_exit_critical_from_isr(level)

#ifdef __cplusplus
}
#endif
