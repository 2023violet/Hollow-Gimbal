/**
 * @file    console_port.c
 * @brief   BSP thin console/telemetry UART port
 * @version v1.0.0
 * @date    2026.05.14
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "console_port.h"

#if !defined(THEMIS_HOST_TEST)
#include "at32f415.h"
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

/* Public Functions ----------------------------------------------------------------------------- */

void console_port_shell_rx_start(uint8_t *rx_buf, uint16_t len)
{
#if !defined(THEMIS_HOST_TEST)
    if ((rx_buf == NULL) || (len == 0U)) {
        return;
    }

    wk_dma_channel_config(CONSOLE_RX_DMA_CHANNEL,
                          CONSOLE_USART_DATA_REG_ADDR,
                          (uint32_t)rx_buf,
                          len);
    dma_channel_enable(CONSOLE_RX_DMA_CHANNEL, TRUE);
#else
    (void)rx_buf;
    (void)len;
#endif
}

uint16_t console_port_shell_rx_position(uint16_t ring_len)
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

void console_port_shell_irq_ack_idle(void)
{
#if !defined(THEMIS_HOST_TEST)
    usart_flag_clear(CONSOLE_USART, USART_IDLEF_FLAG);
#endif
}

bool console_port_shell_irq_idle_pending(void)
{
#if !defined(THEMIS_HOST_TEST)
    return (usart_interrupt_flag_get(CONSOLE_USART, USART_IDLEF_FLAG) != RESET);
#else
    return false;
#endif
}

void console_port_write_poll(const char *data, uint16_t len, uint32_t dma_wait_loops,
                             uint32_t char_timeout, uint32_t tdc_timeout)
{
#if !defined(THEMIS_HOST_TEST)
    volatile uint32_t timeout;

    if ((data == NULL) || (len == 0U)) {
        return;
    }

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
#else
    (void)data;
    (void)len;
    (void)dma_wait_loops;
    (void)char_timeout;
    (void)tdc_timeout;
#endif
}

void console_port_write_dma(const uint8_t *data, uint16_t len)
{
#if !defined(THEMIS_HOST_TEST)
    if ((data == NULL) || (len == 0U)) {
        return;
    }

    dma_channel_enable(CONSOLE_TX_DMA_CHANNEL, FALSE);
    dma_flag_clear(CONSOLE_TX_DMA_DONE_FLAG);
    wk_dma_channel_config(CONSOLE_TX_DMA_CHANNEL,
                          CONSOLE_USART_DATA_REG_ADDR,
                          (uint32_t)data,
                          len);
    dma_channel_enable(CONSOLE_TX_DMA_CHANNEL, TRUE);
#else
    (void)data;
    (void)len;
#endif
}

/* ---------------------------------------- end of file ----------------------------------------- */
