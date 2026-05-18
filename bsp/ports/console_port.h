/**
 * @file    console_port.h
 * @brief   BSP thin console/telemetry UART port
 * @version v1.0.0
 * @date    2026.05.14
 *
 * Design overview
 * ---------------
 * This port owns the shared USART1 + DMA transport details used by the shell
 * transport path and optional debug telemetry path.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     console_port_shell_rx_start(uint8_t *rx_buf, uint16_t len);
uint16_t console_port_shell_rx_position(uint16_t ring_len);
void     console_port_shell_irq_ack_idle(void);
bool     console_port_shell_irq_idle_pending(void);
void     console_port_write_poll(const char *data, uint16_t len, uint32_t dma_wait_loops,
                                 uint32_t char_timeout, uint32_t tdc_timeout);
void     console_port_write_dma(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
