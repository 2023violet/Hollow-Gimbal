/**
 * @file vofa_task.c
 * @brief  VOFA+ Oscilloscope Task Implementation
 * @version v1.0.0
 * @date 2025.12.23
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_common.h"
#include "shell_config.h"
#include "console_port.h"
#include "foc_common.h"
#include "tm_export.h"
#include "vofa.h"
#include "foc_param.h"
#include "traj_trap.h"
#include "core_time.h"
#include "cmsis_os2.h"

#if SHELL_ENABLE && (TM_APP_FEATURE_UART_TELEMETRY != 0U)

#ifdef __cplusplus
extern "C" {
#endif

/* Private variables ---------------------------------------------------------------------------- */
/* Task Handle */
static osThreadId_t vofa_task_handle;

/* Task Attributes */
static const osThreadAttr_t vofa_task_attr = {
    .name       = "vofa_task",
    .stack_size = 128 * 4,
    .priority   = (osPriority_t)osPriorityBelowNormal,
};

/* Task performance statistics */
#if (TASK_PERF_ENABLE == 1)
static struct core_time_perf vofa_task_perf;
static uint32_t              vofa_time_us = 0;
#endif

/* Private function prototypes ------------------------------------------------------------------ */
static void _vofa_task_entry(void *argument);
static void _usart_dma_send_data(uint8_t *p_data, uint16_t len);
// static void _usart_poll_send_data(const char *data, uint16_t len);

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the VOFA task
 * @note   Called automatically by tm_export system
 */
void vofa_task_init(void)
{
    /* Initialize pointer array and counter */
    vofa_register_send_callback(_usart_dma_send_data);
    vofa_task_handle = osThreadNew(_vofa_task_entry, NULL, &vofa_task_attr);
}
INIT_EXPORT(vofa_task_init, EXPORT_LEVEL_APP);

/* Private functions ---------------------------------------------------------------------------- */

/**
 * @brief  Low-level DMA send function
 * 
 * @param  p_data  Pointer to data buffer
 * @param  len     Data length in bytes
 */
void _usart_dma_send_data(uint8_t *p_data, uint16_t len)
{
    console_port_write_dma(p_data, len);
}

// static void _usart_poll_send_data(const char *data, uint16_t len)
// {
//     volatile uint32_t timeout;

//     for (uint16_t i = 0; i < len; i++) {
//         timeout = 0xFFFF;
//         while (usart_flag_get(USART1, USART_TDBE_FLAG) == RESET) {
//             if (timeout-- == 0)
//                 break;
//         }
//         usart_data_transmit(USART1, (uint16_t)data[i]);
//     }

//     timeout = 0xFFFFF;
//     while (usart_flag_get(USART1, USART_TDC_FLAG) == RESET) {
//         if (timeout-- == 0)
//             break;
//     }
// }

/**
 * @brief  Main Task Entry
 * 
 * @param  argument  Unused
 */
static void _vofa_task_entry(void *argument)
{
    for (;;) {
#if (TASK_PERF_ENABLE == 1)
        core_time_perf_start(&vofa_task_perf);
#endif

#if (VOFA_PROTOCOL == VOFA_PROTO_JUSTFLOAT)
        vofa_process_frame();
#endif

#if (TASK_PERF_ENABLE == 1)
        core_time_perf_stop(&vofa_task_perf);
        vofa_time_us = (uint32_t)core_time_perf_get_us(&vofa_task_perf);
        (void)(vofa_time_us);
#endif

        osDelay(1);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* SHELL_ENABLE && (TM_APP_FEATURE_UART_TELEMETRY != 0U) */

/* ---------------------------------------- end of file ----------------------------------------- */
