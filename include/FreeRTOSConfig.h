/**
 * @file    FreeRTOSConfig.h
 * @brief   Themis FreeRTOS kernel configuration
 * @version v1.0.0
 * @date    2026.04.24
 *
 * Design overview
 * ---------------
 * This header is the project-owned FreeRTOS configuration surface consumed by
 * both the kernel and the CMSIS-RTOS V2 wrapper. The file remains in
 * `include/` because it is part of the build contract, but the configuration
 * ownership stays with Themis rather than with generated platform code.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Values derived from product/profile policy come from `tm_core_config.h`.
 * - MCU clock truth still comes from the platform startup path through
 *   `system_core_clock`.
 * - This file is compile-time configuration only; it does not own runtime
 *   policy or task-level behavior by itself.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>
#include "config/tm_core_config.h"

#if defined(__ICCARM__) || defined(__CC_ARM) || defined(__GNUC__)
extern unsigned int system_core_clock;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Core Scheduling ------------------------------------------------------------------------------ */

#define configENABLE_FPU                        0
#define configENABLE_MPU                        0
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configCPU_CLOCK_HZ                      (system_core_clock)
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    (56)
#define configMINIMAL_STACK_SIZE                ((uint16_t)TM_FREERTOS_MINIMAL_STACK_WORDS)
#define configMAX_TASK_NAME_LEN                 (16)
#define configUSE_16_BIT_TICKS                  0
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_MUTEXES                       1
#define configQUEUE_REGISTRY_SIZE               8
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_COUNTING_SEMAPHORES           1

/* Memory Allocation ---------------------------------------------------------------------------- */

#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         1
#define configTOTAL_HEAP_SIZE                   ((size_t)TM_FREERTOS_HEAP_SIZE_BYTES)

/* Hooks ---------------------------------------------------------------------------------------- */

#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0

/* Runtime Stats / Trace ------------------------------------------------------------------------ */

#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Co-routines ---------------------------------------------------------------------------------- */

#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         (2)

/* Software Timers ------------------------------------------------------------------------------ */

#define configUSE_TIMERS                        TM_FREERTOS_USE_TIMERS
#define configTIMER_TASK_PRIORITY               (2)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            (configMINIMAL_STACK_SIZE * 2)

/* Message Buffers ------------------------------------------------------------------------------ */

#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

/* CMSIS-RTOS2 ---------------------------------------------------------------------------------- */

#define configUSE_OS2_THREAD_SUSPEND_RESUME     1
#define configUSE_OS2_THREAD_ENUMERATE          1
#define configUSE_OS2_EVENTFLAGS_FROM_ISR       TM_FREERTOS_USE_OS2_EVENTFLAGS_FROM_ISR
#define configUSE_OS2_THREAD_FLAGS              1
#define configUSE_OS2_TIMER                     TM_FREERTOS_USE_OS2_TIMER
#define configUSE_OS2_MUTEX                     1

/* Optional FreeRTOS APIs ----------------------------------------------------------------------- */

#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskCleanUpResources           1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          TM_FREERTOS_INCLUDE_TIMER_PEND_FUNCTION_CALL
#define INCLUDE_xQueueGetMutexHolder            1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetCurrentTaskHandle       1

/* Interrupt Priorities ------------------------------------------------------------------------- */

/*
 * The CMSIS-RTOS V2 FreeRTOS wrapper depends on the heap implementation chosen
 * by the application. The current project uses heap_4.
 */
#define USE_FreeRTOS_HEAP_4

#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS __NVIC_PRIO_BITS
#else
#define configPRIO_BITS 4
#endif

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Assertions ----------------------------------------------------------------------------------- */

#define configASSERT(x)           \
    if ((x) == 0) {               \
        taskDISABLE_INTERRUPTS(); \
        for (;;)                  \
            ;                     \
    }

/* Handler Mapping ------------------------------------------------------------------------------ */

#define vPortSVCHandler                           SVC_Handler
#define xPortPendSVHandler                        PendSV_Handler

/*
 * After the FreeRTOS 10.3.1 update, `Systick_Handler` comes either from NVIC
 * or from `cmsis_os2.c` depending on the selected system time base.
 */
#define USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION 0

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
