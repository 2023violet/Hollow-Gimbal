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

#include "FreeRTOS.h"
#include "app_shell_banner.h"
#include "app_shell_task.h"
#include "bsp_console.h"
#include "cmsis_os2.h"
#include "core_time.h"
#include "ring_buffer.h"
#include "tm_common.h"
#include "tm_export.h"
#include "tm_shell.h"
#include "tm_shell_output.h"

#include <string.h>

#if SHELL_ENABLE

/* Private Macros ------------------------------------------------------------------------------- */

#define SHELL_TX_OVERFLOW_NOTICE "\r\n[shell] cmd=rx result=error rc=-7\r\n"
#define SHELL_TX_FRAME_HDR_SIZE  (2U)

/* Private State -------------------------------------------------------------------------------- */

static uint8_t _shell_rx_dma_buf[SHELL_RX_BUF_SIZE];
static uint8_t _shell_tx_ring_storage[SHELL_TX_RING_SIZE];
static struct ring_buf _shell_tx_ring = {0};
static volatile uint16_t _dma_prev_pos = 0U;
static char _line_buf[SHELL_LINE_BUF_SIZE];
static uint16_t _line_len = 0U;
static volatile uint32_t _shell_tx_drop_count = 0U;

static osThreadId_t _shell_task_handle = NULL;

static const osThreadAttr_t _shell_task_attr = {
    .name = "shell_task",
    .stack_size = TM_SHELL_TASK_STACK_BYTES,
    .priority = (osPriority_t)osPriorityBelowNormal,
};

#if (TM_TASK_PERF_ENABLE == 1)
static struct core_time_perf _shell_task_perf;
static uint32_t _shell_time_us = 0U;
#endif

/* Private Function Prototypes ------------------------------------------------------------------ */

static void _shell_task_entry(void *argument);
static void _process_dma_rx(void);
static void _hw_usart_tx(const char *str, uint16_t len);
static void _shell_async_print(const char *str, uint16_t len);
static void _drain_tx_queue(void);
static bool _shell_try_enqueue_tx_frame(const uint8_t *data, uint16_t len);
static bool _shell_try_take_tx_frame(uint16_t *len_out);

/* Public Functions ----------------------------------------------------------------------------- */

void shell_task_init(void)
{
    shell_init();

    (void)ring_buf_init(&_shell_tx_ring, _shell_tx_ring_storage, sizeof(_shell_tx_ring_storage));

    (void)bsp_console_shell_rx_start(_shell_rx_dma_buf, SHELL_RX_BUF_SIZE);

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
    if (bsp_console_shell_irq_idle_pending()) {
        bsp_console_shell_irq_ack_idle();
        shell_task_rx_notify();
    }
}

/* Private Functions ---------------------------------------------------------------------------- */

static void _shell_async_print(const char *str, uint16_t len)
{
    uint32_t level;
    uint16_t copy_len;

    if ((str == NULL) || (len == 0U) || (_shell_task_handle == NULL)) {
        return;
    }

    copy_len = len;
    if (copy_len > (uint16_t)(SHELL_TX_RING_SIZE - SHELL_TX_FRAME_HDR_SIZE)) {
        copy_len = (uint16_t)(SHELL_TX_RING_SIZE - SHELL_TX_FRAME_HDR_SIZE);
    }

    level = tm_irq_critical_enter();
    if (!_shell_try_enqueue_tx_frame((const uint8_t *)str, copy_len)) {
        _shell_tx_drop_count++;
        tm_irq_critical_exit(level);
        return;
    }
    tm_irq_critical_exit(level);
    (void)osThreadFlagsSet(_shell_task_handle, FLAG_SHELL_TX);
}

static void _hw_usart_tx(const char *str, uint16_t len)
{
    (void)bsp_console_write_poll(
        str,
        len,
        SHELL_TX_WAIT_VOFA_LOOPS,
        SHELL_TX_CHAR_TIMEOUT,
        SHELL_TX_TDC_TIMEOUT
    );
}

static void _drain_tx_queue(void)
{
    uint16_t pay_len;
    size_t chunk_len;
    uint8_t *chunk;

    while (_shell_try_take_tx_frame(&pay_len)) {
        size_t sent_total = 0U;

        while (sent_total < pay_len) {
            chunk = ring_buf_get_read_ptr(&_shell_tx_ring, &chunk_len);
            if ((chunk == NULL) || (chunk_len == 0U)) {
                return;
            }

            if (chunk_len > (size_t)(pay_len - sent_total)) {
                chunk_len = (size_t)(pay_len - sent_total);
            }

            _hw_usart_tx((const char *)chunk, (uint16_t)chunk_len);
            (void)ring_buf_read_advance(&_shell_tx_ring, chunk_len);
            sent_total += chunk_len;
        }
    }
}

static bool _shell_try_enqueue_tx_frame(const uint8_t *data, uint16_t len)
{
    uint8_t header[SHELL_TX_FRAME_HDR_SIZE];

    if ((data == NULL) || (len == 0U)) {
        return false;
    }

    if (ring_buf_space_len(&_shell_tx_ring) < ((size_t)len + SHELL_TX_FRAME_HDR_SIZE)) {
        return false;
    }

    header[0] = (uint8_t)(len & 0xFFU);
    header[1] = (uint8_t)(len >> 8U);

    if (ring_buf_put(&_shell_tx_ring, header, sizeof(header)) != (ssize_t)sizeof(header)) {
        return false;
    }

    if (ring_buf_put(&_shell_tx_ring, data, len) != (ssize_t)len) {
        (void)ring_buf_write_rollback(&_shell_tx_ring, sizeof(header));
        return false;
    }

    return true;
}

static bool _shell_try_take_tx_frame(uint16_t *len_out)
{
    uint8_t header[SHELL_TX_FRAME_HDR_SIZE];

    if (len_out == NULL) {
        return false;
    }

    if (ring_buf_data_len(&_shell_tx_ring) < SHELL_TX_FRAME_HDR_SIZE) {
        return false;
    }

    if (ring_buf_peek(&_shell_tx_ring, 0U, header, sizeof(header)) != (ssize_t)sizeof(header)) {
        return false;
    }

    *len_out = (uint16_t)header[0] | (uint16_t)(header[1] << 8U);
    if (ring_buf_data_len(&_shell_tx_ring) < ((size_t)(*len_out) + SHELL_TX_FRAME_HDR_SIZE)) {
        return false;
    }

    (void)ring_buf_drop(&_shell_tx_ring, SHELL_TX_FRAME_HDR_SIZE);
    return true;
}

static void _process_dma_rx(void)
{
    uint16_t curr_pos;
    uint16_t prev_pos;
    uint16_t new_byte_count;

    curr_pos = bsp_console_shell_rx_position(SHELL_RX_BUF_SIZE);
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
        char ch = (char)_shell_rx_dma_buf[buf_idx];

        if ((ch == '\0') || (ch == '\r')) {
            continue;
        }

        if (ch == '\n') {
            int submit_rc;

            _line_buf[_line_len] = '\0';
            submit_rc = tm_shell_task_submit_line(_line_buf, _line_len);
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
