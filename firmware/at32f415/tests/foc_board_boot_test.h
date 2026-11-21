/**
 * @file    foc_board_boot_test.h
 * @brief   Host fixture: a boot-selected Board1 instance behind the production UART adapter.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Design overview
 * ---------------
 * Encodes V1 requests into the production RX ring, services the real Runtime
 * Foreground and serial adapter, and decodes the single TX response.
 *
 * Usage example
 * -------------
 * Zero a fixture, call foc_boot_test_select(), then foc_boot_test_hello() and
 * foc_boot_test_call() for each request.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * One fixture per test. Time is explicit and advanced by the caller.
 *
 * Failure / recovery
 * ------------------
 * Helpers return nonzero on a framing or status mismatch so tests stop early.
 */
#pragma once

#include "foc_board_boot.h"
#include "foc_serial.h"

#include <stdio.h>
#include <string.h>

/* Public Types ---------------------------------------------------------------- */

struct foc_boot_test {
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder;
    struct foc_board_boot boot;
    struct foc_serial serial;
    struct foc_uart_buffer uart;
    uint32_t time;
    uint32_t session;
    uint32_t sequence;
};

/** Decoded typed PARAM_READ/PARAM_WRITE response. */
struct foc_boot_test_parameter {
    uint8_t status;
    uint16_t id;
    uint8_t type;
    uint8_t unit;
    uint8_t attributes;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t value;
};

/* Public Functions ------------------------------------------------------------ */

static inline uint32_t foc_boot_test_clock(void *argument)
{
    return *(const uint32_t *)argument;
}

static inline uint32_t foc_boot_test_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static inline float foc_boot_test_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/** Formal selection, then the production serial adapter with the report attached. */
static inline int foc_boot_test_select(struct foc_boot_test *test,
                                       const struct foc_board_calibration_supply *supply,
                                       const struct foc_board_calibration_expectation *expected)
{
    const int status = foc_board_boot_select(&test->boot,
                                             &test->runtime,
                                             &test->output,
                                             &test->encoder,
                                             supply,
                                             expected,
                                             0U);
    if (status == FOC_RUNTIME_OK) {
        foc_serial_init(&test->serial, &test->runtime);
        foc_serial_attach_boot(&test->serial, foc_board_boot_serial_readback, &test->boot);
    }
    return status;
}

/** Send one frame and decode the response. Returns the response status byte or -1. */
static inline int foc_boot_test_exchange(struct foc_boot_test *test,
                                         const struct foc_protocol_frame *request,
                                         struct foc_protocol_frame *response)
{
    uint8_t wire[FOC_PROTOCOL_WIRE_MAX];
    const size_t count = foc_protocol_encode(request, wire, sizeof wire);
    for (size_t index = 0U; index < count; ++index) {
        foc_uart_rx_push(&test->uart, wire[index], false);
    }
    for (unsigned int poll = 0U; poll < 8U; ++poll) {
        struct foc_runtime_foreground_result result;
        (void)foc_runtime_foreground_poll(&test->runtime, test->time, 0U, 0U, &result);
        foc_serial_poll(&test->serial,
                        &test->uart,
                        &test->runtime,
                        foc_boot_test_clock,
                        &test->time);
    }
    struct foc_protocol_stream stream = {0};
    uint8_t byte;
    while (foc_uart_tx_pop(&test->uart, &byte)) {
        const int parsed = foc_protocol_feed(&stream, byte, response);
        if (parsed == FOC_PROTO_OK || parsed == FOC_PROTO_UNKNOWN_COMMAND) {
            if (response->flags != FOC_PROTOCOL_RESPONSE || response->type != request->type ||
                response->sequence != request->sequence || response->length == 0U) {
                return -1;
            }
            return response->payload[0];
        }
        if (parsed != FOC_PROTO_MORE) {
            return -1;
        }
    }
    return -1;
}

static inline int foc_boot_test_hello(struct foc_boot_test *test)
{
    const struct foc_protocol_frame request = {
        .flags = FOC_PROTOCOL_REQUEST,
        .type = FOC_MSG_HELLO,
        .sequence = 100U,
    };
    struct foc_protocol_frame response;
    if (foc_boot_test_exchange(test, &request, &response) != FOC_PROTO_OK ||
        response.session == 0U) {
        return 1;
    }
    test->session = response.session;
    test->sequence = 100U;
    return 0;
}

/** Ordinary request with the next sequence in the current session. */
static inline int foc_boot_test_call(struct foc_boot_test *test,
                                     uint8_t type,
                                     const uint8_t *payload,
                                     uint16_t length,
                                     struct foc_protocol_frame *response)
{
    struct foc_protocol_frame request = {
        .flags = FOC_PROTOCOL_REQUEST,
        .type = type,
        .session = test->session,
        .sequence = ++test->sequence,
        .length = length,
    };
    if (length != 0U) {
        memcpy(request.payload, payload, length);
    }
    return foc_boot_test_exchange(test, &request, response);
}

static inline int foc_boot_test_read(struct foc_boot_test *test,
                                     uint16_t id,
                                     struct foc_boot_test_parameter *parameter)
{
    uint8_t payload[2];
    struct foc_protocol_frame response;
    foc_protocol_write_u16(payload, id);
    const int status = foc_boot_test_call(test, FOC_MSG_PARAM_READ, payload, 2U, &response);
    *parameter = (struct foc_boot_test_parameter){.status = (uint8_t)status};
    if (status == FOC_PROTO_OK) {
        if (response.length != 18U) {
            return -1;
        }
        parameter->id = foc_protocol_read_u16(response.payload + 1);
        parameter->type = response.payload[3];
        parameter->unit = response.payload[4];
        parameter->attributes = response.payload[5];
        parameter->minimum = foc_protocol_read_u32(response.payload + 6);
        parameter->maximum = foc_protocol_read_u32(response.payload + 10);
        parameter->value = foc_protocol_read_u32(response.payload + 14);
    }
    return status;
}

/* ---------------------------------------- end of file ----------------------------------------- */
