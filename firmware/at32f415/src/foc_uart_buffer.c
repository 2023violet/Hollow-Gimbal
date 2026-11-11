/**
 * @file    foc_uart_buffer.c
 * @brief   Single-producer UART receive ring and complete-frame TX handoff.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Keep interrupt work bounded and preserve response storage until transmission.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_serial.h"

#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "UART handoff requires lock-free word atomics");

static bool foc_uart_rx_store(struct foc_uart_buffer *buffer, uint16_t value)
{
    const unsigned int head = atomic_load_explicit(&buffer->rx_head, memory_order_relaxed);
    const unsigned int tail = atomic_load_explicit(&buffer->rx_tail, memory_order_acquire);
    if (head - tail >= FOC_UART_RX_CAPACITY) {
        return false;
    }
    buffer->rx[head % FOC_UART_RX_CAPACITY] = value;
    atomic_store_explicit(&buffer->rx_head, head + 1U, memory_order_release);
    return true;
}

/* Public Functions ------------------------------------------------------------- */

void foc_uart_rx_push(struct foc_uart_buffer *buffer, uint8_t byte, bool error)
{
    if (error) {
        buffer->rx_discard = true;
        atomic_fetch_add_explicit(&buffer->rx_lost, 1U, memory_order_relaxed);
    } else if (buffer->rx_discard) {
        if (byte == 0U && foc_uart_rx_store(buffer, FOC_UART_RX_GAP)) {
            buffer->rx_discard = false;
        }
    } else if (!foc_uart_rx_store(buffer, byte)) {
        buffer->rx_discard = true;
        atomic_fetch_add_explicit(&buffer->rx_lost, 1U, memory_order_relaxed);
    }
}

bool foc_uart_rx_pop(struct foc_uart_buffer *buffer, uint16_t *value)
{
    const unsigned int tail = atomic_load_explicit(&buffer->rx_tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&buffer->rx_head, memory_order_acquire)) {
        return false;
    }
    *value = buffer->rx[tail % FOC_UART_RX_CAPACITY];
    atomic_store_explicit(&buffer->rx_tail, tail + 1U, memory_order_release);
    return true;
}

bool foc_uart_tx_available(const struct foc_uart_buffer *buffer)
{
    return atomic_load_explicit(&buffer->tx_size, memory_order_acquire) == 0U;
}

bool foc_uart_tx_submit(struct foc_uart_buffer *buffer, const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0U || length > sizeof buffer->tx ||
        !foc_uart_tx_available(buffer)) {
        return false;
    }
    memcpy(buffer->tx, data, length);
    atomic_store_explicit(&buffer->tx_size, (unsigned int)length, memory_order_release);
    return true;
}

bool foc_uart_tx_pop(struct foc_uart_buffer *buffer, uint8_t *byte)
{
    const unsigned int size = atomic_load_explicit(&buffer->tx_size, memory_order_acquire);
    if (size == 0U) {
        return false;
    }
    *byte = buffer->tx[buffer->tx_next++];
    if (buffer->tx_next == size) {
        buffer->tx_next = 0U;
        atomic_store_explicit(&buffer->tx_size, 0U, memory_order_release);
    }
    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
