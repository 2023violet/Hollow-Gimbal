/**
 * @file    bsp_console.c
 * @brief   FalconFoc thin console/telemetry UART BSP port
 * @version v1.1.0
 * @date    2026.06.02
 *
 * Implementation role
 * -------------------
 * This file owns the southbound shared USART1 + DMA transport path used by
 * FalconFoc shell transport and optional debug telemetry. Vendor DMA/USART
 * details stay local to this `.c` so the public BSP contract remains small
 * and product-focused.
 *
 * Key collaborators
 * -----------------
 * - `bsp_console.h`: public transport contract
 * - `tm_runtime.h`: shared execution-context detection
 * - `wk_dma.h` + CMSIS device header: southbound DMA/UART truth
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "bsp_console.h"
#include <errno.h>
#include <stdbool.h>
#include "tm_runtime.h"

#if !defined(THEMIS_HOST_TEST)
#ifndef CMSIS_device_header
#error "bsp_console.c requires CMSIS_device_header from platform compile definitions."
#endif

#include CMSIS_device_header
#include "wk_dma.h"
#endif

/* Private Macros ------------------------------------------------------------------------------- */

#if !defined(THEMIS_HOST_TEST)
#define CONSOLE_USART               USART1
#define CONSOLE_RX_DMA_CHANNEL      DMA1_CHANNEL1
#define CONSOLE_TX_DMA_CHANNEL      DMA1_CHANNEL2
#define CONSOLE_TX_DMA_DONE_FLAG    DMA1_FDT2_FLAG
#define CONSOLE_USART_DATA_REG_ADDR ((uint32_t)&USART1->dt)
#endif

/* Private Function Prototypes ------------------------------------------------------------------ */

static bool _bsp_console_buffer_args_are_valid(const void *data, uint16_t len);
static bool _bsp_console_thread_mode_is_valid(void);

/* Public Functions ----------------------------------------------------------------------------- */

int bsp_console_shell_rx_start(uint8_t *rx_buf, uint16_t len)
{
    if (!_bsp_console_thread_mode_is_valid()) {
        return -EPERM;
    }
    if (!_bsp_console_buffer_args_are_valid(rx_buf, len)) {
        return -EINVAL;
    }

#if !defined(THEMIS_HOST_TEST)
    wk_dma_channel_config(CONSOLE_RX_DMA_CHANNEL,
                          CONSOLE_USART_DATA_REG_ADDR,
                          (uint32_t)rx_buf,
                          len);
    dma_channel_enable(CONSOLE_RX_DMA_CHANNEL, TRUE);
#endif

    return 0;
}

uint16_t bsp_console_shell_rx_position(uint16_t ring_len)
{
#if !defined(THEMIS_HOST_TEST)
    if (ring_len == 0U) {
        return 0U;
    }

    return (uint16_t)(ring_len - (uint16_t)(CONSOLE_RX_DMA_CHANNEL->dtcnt));
#else
    (void)ring_len;
    return 0U;
#endif
}

void bsp_console_shell_irq_ack_idle(void)
{
#if !defined(THEMIS_HOST_TEST)
    usart_flag_clear(CONSOLE_USART, USART_IDLEF_FLAG);
#endif
}

bool bsp_console_shell_irq_idle_pending(void)
{
#if !defined(THEMIS_HOST_TEST)
    return (usart_interrupt_flag_get(CONSOLE_USART, USART_IDLEF_FLAG) != RESET);
#else
    return false;
#endif
}

int bsp_console_write_poll(const char *data, uint16_t len, uint32_t dma_wait_loops,
                           uint32_t char_timeout, uint32_t tdc_timeout)
{
    if (!_bsp_console_thread_mode_is_valid()) {
        return -EPERM;
    }
    if (!_bsp_console_buffer_args_are_valid(data, len)) {
        return -EINVAL;
    }

#if !defined(THEMIS_HOST_TEST)
    volatile uint32_t timeout;

    timeout = dma_wait_loops;
    while (!dma_flag_get(CONSOLE_TX_DMA_DONE_FLAG) && (timeout > 0U)) {
        timeout--;
    }

    for (uint16_t i = 0U; i < len; i++) {
        timeout = char_timeout;
        while ((usart_flag_get(CONSOLE_USART, USART_TDBE_FLAG) == RESET) && (timeout > 0U)) {
            timeout--;
        }
        usart_data_transmit(CONSOLE_USART, (uint16_t)(uint8_t)data[i]);
    }

    timeout = tdc_timeout;
    while ((usart_flag_get(CONSOLE_USART, USART_TDC_FLAG) == RESET) && (timeout > 0U)) {
        timeout--;
    }
#endif

    (void)dma_wait_loops;
    (void)char_timeout;
    (void)tdc_timeout;
    return 0;
}

int bsp_console_write_dma(const uint8_t *data, uint16_t len)
{
    if (!_bsp_console_thread_mode_is_valid()) {
        return -EPERM;
    }
    if (!_bsp_console_buffer_args_are_valid(data, len)) {
        return -EINVAL;
    }

#if !defined(THEMIS_HOST_TEST)
    dma_channel_enable(CONSOLE_TX_DMA_CHANNEL, FALSE);
    dma_flag_clear(CONSOLE_TX_DMA_DONE_FLAG);
    wk_dma_channel_config(CONSOLE_TX_DMA_CHANNEL,
                          CONSOLE_USART_DATA_REG_ADDR,
                          (uint32_t)data,
                          len);
    dma_channel_enable(CONSOLE_TX_DMA_CHANNEL, TRUE);
#endif

    return 0;
}

/* Private Functions ---------------------------------------------------------------------------- */

static bool _bsp_console_buffer_args_are_valid(const void *data, uint16_t len)
{
    return ((data != NULL) && (len != 0U));
}

static bool _bsp_console_thread_mode_is_valid(void)
{
    return !tm_is_in_isr();
}

/* ---------------------------------------- end of file ----------------------------------------- */
