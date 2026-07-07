/**
 * @file    bsp_console.h
 * @brief   FalconFoc thin console/telemetry UART BSP contract
 * @version v1.1.0
 * @date    2026.06.02
 *
 * Design overview
 * ---------------
 * This BSP owns the shared USART1 + DMA transport details used by FalconFoc
 * shell transport and optional debug telemetry paths. It intentionally
 * exposes only the current product-facing transport actions and query
 * helpers; it is not a generic UART HAL or future-proof device layer.
 *
 * Usage example
 * -------------
 * (void)bsp_console_shell_rx_start(rx_buf, rx_len);
 * if (bsp_console_shell_irq_idle_pending()) {
 *     bsp_console_shell_irq_ack_idle();
 * }
 * (void)bsp_console_write_poll(frame, len, wait_loops, char_timeout, tdc_timeout);
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Shared USART1 board truth stays southbound in `bsp_console.c`.
 * - `bsp_console_shell_rx_start()` is a thread-mode transport bring-up helper
 *   and may be called before or after scheduler start.
 * - `bsp_console_shell_irq_ack_idle()` and
 *   `bsp_console_shell_irq_idle_pending()` are interrupt-safe IRQ owner
 *   helpers used by the live USART1 idle interrupt path.
 * - `bsp_console_write_poll()` and `bsp_console_write_dma()` are thread-mode
 *   transport actions. ISR callers are rejected.
 *
 * Failure / recovery
 * ------------------
 * - Action functions return `0` on success or negative `-errno` on failure.
 * - Query helpers keep primitive return types.
 * - Higher-level queueing, retry, output policy, and shell/telemetry semantics
 *   remain owned above BSP.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport RX binding ------------------------------------------------------------------------- */

/**
 * @brief  Bind the shell DMA RX ring buffer and enable RX DMA.
 *
 * Contract:
 * - thread mode only
 * - allowed before or after scheduler start
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int bsp_console_shell_rx_start(uint8_t *rx_buf, uint16_t len);

/* RX/IRQ queries -------------------------------------------------------------------------------- */

uint16_t bsp_console_shell_rx_position(uint16_t ring_len);
void     bsp_console_shell_irq_ack_idle(void);
bool     bsp_console_shell_irq_idle_pending(void);

/* Thread-mode TX actions ----------------------------------------------------------------------- */

/**
 * @brief  Write one frame through the console USART using polling.
 *
 * Contract:
 * - thread mode only
 * - caller owns timeout policy
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int bsp_console_write_poll(const char *data, uint16_t len, uint32_t dma_wait_loops,
                           uint32_t char_timeout, uint32_t tdc_timeout);

/**
 * @brief  Start one DMA TX frame on the shared console USART.
 *
 * Contract:
 * - thread mode only
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int bsp_console_write_dma(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
