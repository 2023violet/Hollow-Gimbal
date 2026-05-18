/**
 * @file    shell_task.c
 * @brief   Shell UART transport task
 * @version v1.0.0
 * @date    2026.05.12
 *
 * Design overview
 * ---------------
 * This module owns only the UART/DMA transport side of the interactive shell:
 * - USART1 DMA RX byte collection
 * - bounded async TX staging
 * - bridging complete input lines into `app_shell_task`
 *
 * Product banner, command ownership, and synchronous `tm_shell` execution now
 * belong to application-owned shell modules.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "shell_task.h"
#include <string.h>
#include "FreeRTOS.h"
#include "console_port.h"
#include "cmsis_os2.h"
#include "core_time.h"
#include "mempool.h"
#include "spsc_queue.h"
#include "tm_common.h"
#include "tm_export.h"
#include "tm_shell.h"
#include "app_shell_banner.h"
#include "app_shell_task.h"
#include "tm_shell_output.h"

#if SHELL_ENABLE

/* Private Macros ------------------------------------------------------------------------------- */

#define SHELL_TX_OVERFLOW_NOTICE "\r\n[shell] cmd=rx result=error rc=-7\r\n"

/* Private State -------------------------------------------------------------------------------- */

MEMPOOL_DEFINE(_tx_mempool, SHELL_TX_BLOCK_SIZE, SHELL_TX_QUEUE_CAP);
SPSC_QUEUE_DEFINE(_tx_queue, SHELL_TX_QUEUE_CAP);

static uint8_t           _shell_rx_dma_buf[SHELL_RX_BUF_SIZE];
static volatile uint16_t _dma_prev_pos = 0U;
static char              _line_buf[SHELL_LINE_BUF_SIZE];
static uint16_t          _line_len = 0U;

static osThreadId_t _shell_task_handle = NULL;

static const osThreadAttr_t _shell_task_attr = {
    .name       = "shell_task",
    .stack_size = 256U * 4U,
    .priority   = (osPriority_t)osPriorityBelowNormal,
};

#if (TM_TASK_PERF_ENABLE == 1)
static struct core_time_perf _shell_task_perf;
static uint32_t              _shell_time_us = 0U;
#endif

/* Private Function Prototypes ------------------------------------------------------------------ */

static void _shell_task_entry(void *argument);
static void _process_dma_rx(void);
static void _hw_usart_tx(const char *str, uint16_t len);
static void _shell_async_print(const char *str, uint16_t len);
static void _drain_tx_queue(void);

/* Public Functions ----------------------------------------------------------------------------- */

void shell_task_init(void)
{
    shell_init();

    mempool_init(&_tx_mempool, __tx_mempool_buf, sizeof(__tx_mempool_buf), SHELL_TX_BLOCK_SIZE);
    spsc_queue_init(&_tx_queue, __tx_queue_buf, SHELL_TX_QUEUE_CAP);

    console_port_shell_rx_start(_shell_rx_dma_buf, SHELL_RX_BUF_SIZE);

    shell_set_print((shell_print_fn_t)_shell_async_print);
    (void)tm_shell_task_init();
    tm_shell_task_set_banner_hook(app_shell_banner_print);
    _shell_task_handle = osThreadNew(_shell_task_entry, NULL, &_shell_task_attr);
}

void shell_task_rx_notify(void)
{
    if (_shell_task_handle != NULL) {
        (void)osThreadFlagsSet(_shell_task_handle, FLAG_SHELL_RX);
    }
}

void USART1_IRQHandler(void)
{
    if (console_port_shell_irq_idle_pending()) {
        console_port_shell_irq_ack_idle();
        shell_task_rx_notify();
    }
}

/* Private Functions ---------------------------------------------------------------------------- */

static void _shell_async_print(const char *str, uint16_t len)
{
    uint32_t level;
    uint8_t *block;
    uint16_t copy_len;

    if ((str == NULL) || (len == 0U) || (_shell_task_handle == NULL)) {
        return;
    }

    level = tm_irq_critical_enter();
    block = (uint8_t *)mempool_alloc(&_tx_mempool);
    if (block == NULL) {
        tm_irq_critical_exit(level);
        return;
    }

    copy_len                   = (len <= (uint16_t)_TX_MAX_STR) ? len : (uint16_t)_TX_MAX_STR;
    block[_TX_LEN_OFFSET + 0U] = (uint8_t)copy_len;
    block[_TX_LEN_OFFSET + 1U] = (uint8_t)(copy_len >> 8U);
    memcpy(block + _TX_STR_OFFSET, str, copy_len);
    block[_TX_STR_OFFSET + copy_len] = '\0';

    if (spsc_queue_push(&_tx_queue, block) != 0) {
        mempool_free(&_tx_mempool, block);
        tm_irq_critical_exit(level);
        return;
    }

    tm_irq_critical_exit(level);
    (void)osThreadFlagsSet(_shell_task_handle, FLAG_SHELL_TX);
}

static void _hw_usart_tx(const char *str, uint16_t len)
{
    console_port_write_poll(str,
                            len,
                            SHELL_TX_WAIT_VOFA_LOOPS,
                            SHELL_TX_CHAR_TIMEOUT,
                            SHELL_TX_TDC_TIMEOUT);
}

static void _drain_tx_queue(void)
{
    void *ptr;

    while (spsc_queue_pop(&_tx_queue, &ptr) == 0) {
        uint8_t *blk = (uint8_t *)ptr;
        uint16_t pay_len =
            (uint16_t)(blk[_TX_LEN_OFFSET + 0U]) | (uint16_t)(blk[_TX_LEN_OFFSET + 1U] << 8U);
        uint32_t level;

        _hw_usart_tx((const char *)(blk + _TX_STR_OFFSET), pay_len);

        level = tm_irq_critical_enter();
        mempool_free(&_tx_mempool, ptr);
        tm_irq_critical_exit(level);
    }
}

static void _process_dma_rx(void)
{
    uint16_t curr_pos;
    uint16_t prev_pos;
    uint16_t new_byte_count;

    curr_pos = console_port_shell_rx_position(SHELL_RX_BUF_SIZE);
    prev_pos = _dma_prev_pos;

    if (curr_pos == prev_pos) {
        return;
    }

    if (curr_pos > prev_pos) {
        new_byte_count = curr_pos - prev_pos;
    } else {
        new_byte_count = (SHELL_RX_BUF_SIZE - prev_pos) + curr_pos;
    }

    for (uint16_t i = 0U; i < new_byte_count; i++) {
        uint16_t buf_idx = (uint16_t)((prev_pos + i) % SHELL_RX_BUF_SIZE);
        char     ch      = (char)_shell_rx_dma_buf[buf_idx];

        if ((ch == '\0') || (ch == '\r')) {
            continue;
        }

        if (ch == '\n') {
            int submit_rc;

            _line_buf[_line_len] = '\0';
            submit_rc            = tm_shell_task_submit_line(_line_buf, _line_len);
            if (submit_rc != 0) {
                shell_print(SHELL_TX_OVERFLOW_NOTICE);
                TM_SHELL_PROMPT();
            }
            _line_len = 0U;
            continue;
        }

        if (_line_len < (SHELL_LINE_BUF_SIZE - 1U)) {
            _line_buf[_line_len++] = ch;
            continue;
        }

        shell_print(SHELL_TX_OVERFLOW_NOTICE);
        TM_SHELL_PROMPT();
        _line_len = 0U;
    }

    _dma_prev_pos = curr_pos;
}

static void _shell_task_entry(void *argument)
{
    (void)argument;

    tm_shell_task_notify_transport_ready();
    _drain_tx_queue();

    for (;;) {
        uint32_t flags;

#if (TM_TASK_PERF_ENABLE == 1)
        core_time_perf_start(&_shell_task_perf);
#endif

        flags = osThreadFlagsWait(FLAG_SHELL_RX | FLAG_SHELL_TX, osFlagsWaitAny, osWaitForever);
        if ((flags & FLAG_SHELL_RX) != 0U) {
            _process_dma_rx();
        }
        _drain_tx_queue();

#if (TM_TASK_PERF_ENABLE == 1)
        core_time_perf_stop(&_shell_task_perf);
        _shell_time_us = (uint32_t)core_time_perf_get_us(&_shell_task_perf);
        (void)_shell_time_us;
#endif
    }
}

#endif /* SHELL_ENABLE */

/* ---------------------------------------- end of file ----------------------------------------- */
