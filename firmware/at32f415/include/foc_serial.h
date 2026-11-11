/**
 * @file    foc_serial.h
 * @brief   Board1 UART byte handoff and Foreground V1 command adapter.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Hand bytes between a low-priority IRQ and a bounded Foreground service.
 *
 * Usage example
 * -------------
 * Zero both contexts before enabling the IRQ. Service Runtime before each poll.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * IRQ is the sole RX producer and TX consumer. Foreground owns the parser,
 * session and RAM policy. The lifetime of both contexts spans the IRQ service.
 *
 * Failure / recovery
 * ------------------
 * RX loss inserts a gap marker after a delimiter. Ordinary requests require a
 * free response slot, with one additional acknowledgement reserved for STOP.
 */
#pragma once

#include "foc_protocol.h"
#include "foc_runtime.h"

/* Public Types ---------------------------------------------------------------- */

/** Fixed UART queue depth and default number of bytes examined per poll. */
enum foc_serial_limits {
    FOC_UART_RX_CAPACITY = 256,
    FOC_UART_RX_GAP = 256,
    FOC_SERIAL_RX_DEFAULT = 24,
};

/** SPSC transport state. Only atomics are written by one domain and read by both. */
struct foc_uart_buffer {
    /** RX ring elements, with one out-of-band gap marker. */
    uint16_t rx[FOC_UART_RX_CAPACITY];
    /** IRQ-owned monotonically advancing RX write position. */
    atomic_uint rx_head;
    /** Foreground-owned monotonically advancing RX read position. */
    atomic_uint rx_tail;
    /** Count of RX-overflow quarantine entries and UART error events. */
    atomic_uint rx_lost;
    /** Producer drops through a delimiter after RX corruption or overrun. */
    bool rx_discard;
    /** Complete response storage, immutable while TX size is nonzero. */
    uint8_t tx[FOC_PROTOCOL_WIRE_MAX];
    /** Published TX length, zero when the buffer can be reused. */
    atomic_uint tx_size;
    /** IRQ-owned index of the next TX byte. */
    unsigned int tx_next;
};

/** Callback for the MCU clock, sampled when a frame finishes validation. */
typedef uint32_t (*foc_serial_clock_t)(void *argument);

/** Foreground-owned protocol state and configuration. */
struct foc_serial {
    /** Framing state for one UART. */
    struct foc_protocol_stream stream;
    /** Current logical session, zero before HELLO. */
    uint32_t session;
    /** Sequence of the HELLO that opened the current session. */
    uint32_t hello_seq;
    /** Last admitted ordinary request sequence. */
    uint32_t request_seq;
    /** Runtime publication sequence, continuous across wire sessions. */
    uint32_t publication_seq;
    /** Request sequence of the reserved STOP response. */
    uint32_t stop_seq;
    /** Session of the reserved STOP response. */
    uint32_t stop_session;
    /** Number of observed CRC failures. */
    uint32_t crc_errors;
    /** Number of frames/gap markers discarded by the consumer. */
    uint32_t dropped;
    /** Applied RAM-only remote current-vector limit in A. */
    float current_limit_a;
    /** Applied RAM-only absolute position-reference limit in mechanical rad. */
    float position_limit_rad;
    /** Applied RAM-only RX service cap in bytes per poll. */
    uint32_t rx_budget;
    /** Whether the ordinary sequence watermark is initialized. */
    bool request_seen;
    /** Whether the reserved STOP response must still be sent. */
    bool stop_pending;
    /** Whether a parameter has changed since initialization. */
    bool dirty;
};

/* Public Functions ------------------------------------------------------------- */

/** IRQ only. Queue a byte or mark a peripheral error; bad byte content is ignored. */
void foc_uart_rx_push(struct foc_uart_buffer *buffer, uint8_t byte, bool error);

/** Foreground only. Consume one byte or gap marker. */
bool foc_uart_rx_pop(struct foc_uart_buffer *buffer, uint16_t *value);

/** Foreground only. True when the complete TX slot may be filled. */
bool foc_uart_tx_available(const struct foc_uart_buffer *buffer);

/** Foreground only. Copy a complete response if the TX slot is available. */
bool foc_uart_tx_submit(struct foc_uart_buffer *buffer, const uint8_t *data, size_t length);

/** IRQ only. Remove one byte for the UART data register. */
bool foc_uart_tx_pop(struct foc_uart_buffer *buffer, uint8_t *byte);

/** Initialize the Foreground adapter for a fresh boot before receiving bytes. */
void foc_serial_init(struct foc_serial *serial, const struct foc_runtime *runtime);

/** After Runtime supervision, examine at most rx_budget bytes and one complete frame. */
void foc_serial_poll(struct foc_serial *serial,
                     struct foc_uart_buffer *uart,
                     struct foc_runtime *runtime,
                     foc_serial_clock_t clock,
                     void *argument);

/* ---------------------------------------- end of file ----------------------------------------- */
