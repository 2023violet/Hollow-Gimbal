/**
 * @file    test_foc_serial.c
 * @brief   Exercise V1 commands and UART backpressure with the real Runtime.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Drive fake UART interrupts and time through the production byte handoff.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"
#include "foc_board_cogging.h"
#include "foc_parameter_table.h"
#include "foc_serial.h"

#include <stdio.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
            return 1;                                       \
        }                                                   \
    } while (0)

/* Private Types ---------------------------------------------------------------- */

struct fixture {
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_serial serial;
    struct foc_uart_buffer uart;
    uint32_t time;
};

/* Private Helpers -------------------------------------------------------------- */

static uint32_t clock_us(void *arg)
{
    return *(const uint32_t *)arg;
}

static void poll(struct fixture *test)
{
    struct foc_runtime_foreground_result result;
    (void)foc_runtime_foreground_poll(&test->runtime, test->time, 0U, 0U, &result);
    foc_serial_poll(&test->serial, &test->uart, &test->runtime, clock_us, &test->time);
}

static int init(struct fixture *test)
{
    CHECK(foc_board_init(&test->runtime, &test->output, 0) == FOC_RUNTIME_OK);
    foc_serial_init(&test->serial, &test->runtime);
    return 0;
}

static void inject(struct fixture *test, const struct foc_protocol_frame *frame, bool corrupt)
{
    uint8_t wire[FOC_PROTOCOL_WIRE_MAX];
    const size_t count = foc_protocol_encode(frame, wire, sizeof wire);
    if (corrupt) {
        wire[2] ^= 1U;
    }
    for (size_t i = 0; i < count; ++i) {
        foc_uart_rx_push(&test->uart, wire[i], false);
    }
    for (unsigned int i = 0; i < 8; ++i) {
        poll(test);
    }
}

static int receive(struct fixture *test, struct foc_protocol_frame *frame)
{
    struct foc_protocol_stream stream = {0};
    uint8_t byte;
    while (foc_uart_tx_pop(&test->uart, &byte)) {
        const int result = foc_protocol_feed(&stream, byte, frame);
        if (result != FOC_PROTO_MORE) {
            return result;
        }
    }
    return FOC_PROTO_MORE;
}

static void h0(struct fixture *test)
{
    const struct foc_runtime_fast_input input = {
        .adc = {1241U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, test->time, test->time},
        .now_us = test->time,
    };
    (void)foc_runtime_h0_step(&test->runtime, &input);
}

static void pressure_poll(struct fixture *test)
{
    struct foc_runtime_foreground_result result;
    h0(test);
    (void)foc_runtime_foreground_poll(&test->runtime, test->time + 2U, 0U, 0U, &result);
    foc_serial_poll(&test->serial, &test->uart, &test->runtime, clock_us, &test->time);
}

/* Analytic stationary zero-code source, not a measured Encoder first frame. */
static struct foc_runtime_sensor mock_sensor(struct foc_runtime *runtime, uint32_t at, uint32_t seq)
{
    struct foc_runtime_sensor sensor = {.captured_us = at, .seq = seq, .valid = true};
    if (runtime->config.motion.enabled) {
        sensor.electrical_phase_valid = foc_rotor_phase_from_counts(
                                            &runtime->config.motion.rotor,
                                            0U,
                                            2U,
                                            &sensor.theta_elec_rad
                                        ) == FOC_STATUS_OK;
    }
    return sensor;
}

static void runtime_tick(struct fixture *test)
{
    struct foc_runtime_foreground_result result;
    const struct foc_runtime_sensor sensor = mock_sensor(&test->runtime, test->time, test->time);
    (void)foc_runtime_h1_step(&test->runtime, &sensor, test->time, 0U);
    h0(test);
    (void)foc_runtime_foreground_poll(&test->runtime, test->time + 2U, 0U, 0U, &result);
}

static int transact(
    struct fixture *test,
    struct foc_protocol_frame request,
    struct foc_protocol_frame *response,
    int status
)
{
    inject(test, &request, false);
    CHECK(receive(test, response) == FOC_PROTO_OK);
    CHECK(
        response->type == request.type && response->flags == FOC_PROTOCOL_RESPONSE &&
        response->sequence == request.sequence && response->payload[0] == status
    );
    return 0;
}

static int test_commands(void)
{
    struct fixture test = {0};
    struct foc_protocol_frame response;
    struct foc_protocol_frame request =
        {.flags = FOC_PROTOCOL_REQUEST, .type = FOC_MSG_HELLO, .sequence = 9};
    CHECK(init(&test) == 0);
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0);
    const uint32_t first = response.session;
    CHECK(first != 0 && response.payload[6] == 0U);
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0 && response.session == first);
    test.time = 10;
    h0(&test);
    request.session = first;
    request.type = FOC_MSG_SETPOINT;
    request.sequence = UINT32_MAX;
    request.length = 9;
    request.payload[4] = 0x3f;
    request.payload[3] = 0x80;
    test.time = 29;
    CHECK(transact(&test, request, &response, FOC_PROTO_ACCEPTED) == 0);
    const unsigned int index = atomic_load(&test.runtime.command_slot);
    CHECK(index < 2 && test.runtime.command_slots[index].produced_us == 29);
    test.time = 33;
    CHECK(transact(&test, request, &response, FOC_PROTO_REPLAY) == 0);
    CHECK(test.runtime.command_slots[index].produced_us == 29);
    request.sequence = 0;
    inject(&test, &request, true);
    CHECK(receive(&test, &response) == FOC_PROTO_MORE && test.serial.crc_errors == 1);
    CHECK(test.runtime.command_slots[index].produced_us == 29);
    CHECK(transact(&test, request, &response, FOC_PROTO_ACCEPTED) == 0);
    request.sequence = 0x80000000U;
    CHECK(transact(&test, request, &response, FOC_PROTO_REPLAY) == 0);
    request.sequence = 1;
    request.payload[4] = 0x7f;
    CHECK(transact(&test, request, &response, FOC_PROTO_INVALID_ARGUMENT) == 0);
    CHECK(test.runtime.command_slots[atomic_load(&test.runtime.command_slot)].produced_us == 33);
    request.length = 0;
    request.type = FOC_MSG_ARM;
    request.sequence++;
    CHECK(transact(&test, request, &response, FOC_PROTO_OUTPUT_LOCKED) == 0);
    request.type = FOC_MSG_START;
    request.sequence++;
    CHECK(transact(&test, request, &response, FOC_PROTO_OUTPUT_LOCKED) == 0);
    request.type = FOC_MSG_TELEMETRY_STREAM;
    request.sequence++;
    CHECK(transact(&test, request, &response, FOC_PROTO_NOT_SUPPORTED) == 0);
    request.type = 0x7f;
    request.sequence++;
    inject(&test, &request, false);
    CHECK(
        receive(&test, &response) == FOC_PROTO_UNKNOWN_COMMAND &&
        response.payload[0] == FOC_PROTO_UNKNOWN_COMMAND
    );
    request.type = FOC_MSG_HELLO;
    request.session = 0;
    request.sequence++;
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0 && response.session != first);
    CHECK(atomic_load(&test.runtime.command_slot) >= 2);
    request.type = FOC_MSG_STATUS_READ;
    request.session = first;
    request.sequence++;
    CHECK(transact(&test, request, &response, FOC_PROTO_BAD_SESSION) == 0);
    CHECK(test.output.declined_updates == 0);
    return 0;
}

static void runtime_tick(struct fixture *test);

static int test_host_policy_keeps_output_locked(void)
{
    struct fixture test = {0};
    struct foc_protocol_frame response;
    struct foc_protocol_frame request = {
        .flags = FOC_PROTOCOL_REQUEST,
        .type = FOC_MSG_HELLO,
        .sequence = 0U,
    };
    CHECK(foc_board_init(&test.runtime, &test.output, 0U) == FOC_RUNTIME_OK);
    foc_serial_init_with_policy(&test.serial, &test.runtime, FOC_SERIAL_TARGET_VIRTUAL_ONLY);
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0);
    request.session = response.session;
    h0(&test);
    request.sequence = 1U;
    request.type = FOC_MSG_SETPOINT;
    request.length = 5U;
    request.payload[0] = FOC_RUNTIME_MODE_IMPEDANCE;
    test.time = 1U;
    CHECK(transact(&test, request, &response, FOC_PROTO_INVALID_ARGUMENT) == 0);
    CHECK(atomic_load(&test.runtime.command_slot) >= 2U && test.serial.publication_seq == 0U);
    request.sequence++;
    request.length = 9U;
    request.payload[0] = FOC_RUNTIME_MODE_CURRENT;
    foc_protocol_write_u32(request.payload + 1U, 0U);
    foc_protocol_write_u32(request.payload + 5U, 0x3f000000U);
    CHECK(transact(&test, request, &response, FOC_PROTO_ACCEPTED) == 0);

    request.sequence++;
    request.type = FOC_MSG_ARM;
    request.length = 0U;
    CHECK(transact(&test, request, &response, FOC_PROTO_ACCEPTED) == 0);
    foc_runtime_request_inhibit(&test.runtime, FOC_SAFE_FAULT_INVALID_STATE);
    test.time = 200U;
    runtime_tick(&test);
    struct foc_runtime_lifecycle_result lifecycle;
    CHECK(
        foc_serial_read_lifecycle_result(&test.serial, &test.runtime, &lifecycle) == FOC_RUNTIME_OK
    );
    CHECK(
        lifecycle.result == FOC_RUNTIME_REQUEST_CANCELLED &&
        lifecycle.action == FOC_RUNTIME_ACTION_ARM
    );

    request.sequence++;
    request.type = FOC_MSG_START;
    CHECK(transact(&test, request, &response, FOC_PROTO_ACCEPTED) == 0);
    foc_runtime_request_inhibit(&test.runtime, FOC_SAFE_FAULT_INVALID_STATE);
    test.time = 400U;
    runtime_tick(&test);
    CHECK(
        foc_serial_read_lifecycle_result(&test.serial, &test.runtime, &lifecycle) == FOC_RUNTIME_OK
    );
    CHECK(
        lifecycle.result == FOC_RUNTIME_REQUEST_CANCELLED &&
        lifecycle.action == FOC_RUNTIME_ACTION_START
    );
    CHECK(test.output.declined_updates == 0U);
    return 0;
}

static int test_uart(void)
{
    struct foc_uart_buffer uart = {0};
    for (unsigned int i = 0; i < FOC_UART_RX_CAPACITY; ++i) {
        foc_uart_rx_push(&uart, (uint8_t)i, false);
    }
    foc_uart_rx_push(&uart, 0x88, false);
    CHECK(atomic_load(&uart.rx_lost) == 1);
    uint16_t value;
    for (unsigned int i = 0; i < FOC_UART_RX_CAPACITY; ++i) {
        CHECK(foc_uart_rx_pop(&uart, &value) && value == i);
    }
    foc_uart_rx_push(&uart, 1, false);
    foc_uart_rx_push(&uart, 0, false);
    foc_uart_rx_push(&uart, 42, false);
    CHECK(foc_uart_rx_pop(&uart, &value) && value == FOC_UART_RX_GAP);
    CHECK(foc_uart_rx_pop(&uart, &value) && value == 42);
    CHECK(!foc_uart_rx_pop(&uart, &value));
    const uint8_t bytes[] = {1, 2, 3};
    CHECK(foc_uart_tx_submit(&uart, bytes, 3));
    CHECK(!foc_uart_tx_submit(&uart, bytes + 1, 2));
    uint8_t byte;
    for (unsigned int i = 0; i < 3; ++i) {
        CHECK(foc_uart_tx_pop(&uart, &byte) && byte == bytes[i]);
    }
    CHECK(foc_uart_tx_available(&uart));
    atomic_store(&uart.rx_head, UINT32_MAX - 15U);
    atomic_store(&uart.rx_tail, UINT32_MAX - 15U);
    for (unsigned int i = 0; i < 64; ++i) {
        foc_uart_rx_push(&uart, (uint8_t)i, false);
        CHECK(foc_uart_rx_pop(&uart, &value) && value == i);
    }
    foc_uart_rx_push(&uart, 8, true);
    foc_uart_rx_push(&uart, 0, false);
    CHECK(foc_uart_rx_pop(&uart, &value) && value == FOC_UART_RX_GAP);
    return 0;
}

static int test_backpressure(void)
{
    struct fixture test = {0};
    struct foc_protocol_frame response;
    struct foc_protocol_frame request = {.flags = FOC_PROTOCOL_REQUEST, .type = FOC_MSG_HELLO};
    CHECK(init(&test) == 0);
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0);
    h0(&test);
    request.session = response.session;
    request.type = FOC_MSG_STATUS_READ;
    inject(&test, &request, false);
    CHECK(!foc_uart_tx_available(&test.uart));
    request.type = FOC_MSG_SETPOINT;
    request.length = 9;
    request.sequence = 1;
    inject(&test, &request, false);
    CHECK(test.serial.publication_seq == 0 && test.serial.dropped == 1);
    request.type = FOC_MSG_STOP;
    request.length = 0;
    request.sequence = 2;
    inject(&test, &request, false);
    CHECK(test.serial.stop_pending && atomic_load(&test.runtime.stop_requests) != 0);
    CHECK(receive(&test, &response) == 0 && response.type == FOC_MSG_STATUS_READ);
    poll(&test);
    CHECK(
        receive(&test, &response) == 0 && response.type == FOC_MSG_STOP && response.sequence == 2 &&
        response.payload[0] == FOC_PROTO_ACCEPTED
    );
    CHECK(!test.serial.stop_pending);
    CHECK(transact(&test, request, &response, FOC_PROTO_REPLAY) == 0);
    return 0;
}

static int test_parameters(void)
{
    struct fixture test = {0};
    struct foc_protocol_frame response;
    struct foc_protocol_frame request = {.flags = FOC_PROTOCOL_REQUEST, .type = FOC_MSG_HELLO};
    CHECK(init(&test) == 0);
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0);
    h0(&test);
    request.session = response.session;
    request.type = FOC_MSG_PARAM_WRITE;
    request.length = 7;
    request.payload[0] = 1;
    request.payload[2] = 1;
    request.payload[3] = 16;
    CHECK(transact(&test, request, &response, FOC_PROTO_APPLIED) == 0);
    CHECK(test.serial.rx_budget == 16 && test.serial.dirty);
    request.sequence++;
    request.payload[3] = 33;
    CHECK(transact(&test, request, &response, FOC_PROTO_INVALID_ARGUMENT) == 0);
    request.sequence++;
    request.payload[0] = 2;
    request.payload[2] = 2;
    foc_protocol_write_u32(request.payload + 3, 0x7fc00000);
    CHECK(transact(&test, request, &response, FOC_PROTO_INVALID_ARGUMENT) == 0);
    request.sequence++;
    foc_protocol_write_u32(request.payload + 3, 0x3f800000);
    CHECK(
        transact(&test, request, &response, FOC_PROTO_APPLIED) == 0 &&
        test.serial.current_limit_a == 1.0f
    );
    request.sequence++;
    request.payload[0] = 16;
    request.payload[2] = 1;
    CHECK(transact(&test, request, &response, FOC_PROTO_NOT_SUPPORTED) == 0);
    request.sequence++;
    request.payload[0] = 255;
    CHECK(transact(&test, request, &response, FOC_PROTO_INVALID_ARGUMENT) == 0);
    /* COGGING_READ: 64 codes per chunk, 24 in the last; an empty table reads scale 0. */
    {
        foc_board_cogging_state.table.code[1000] = -7;
        struct foc_protocol_frame read = request;
        read.type = FOC_MSG_COGGING_READ;
        read.length = 2;
        read.sequence = ++request.sequence;
        foc_protocol_write_u16(read.payload, 0U);
        CHECK(transact(&test, read, &response, FOC_PROTO_OK) == 0);
        CHECK(response.length == 74U && foc_protocol_read_u16(response.payload + 1) == 1024U);
        CHECK(foc_protocol_read_u32(response.payload + 3) == 0U && response.payload[9] == 64U);
        read.sequence = ++request.sequence;
        foc_protocol_write_u16(read.payload, 1000U);
        CHECK(transact(&test, read, &response, FOC_PROTO_OK) == 0);
        CHECK(response.length == 34U && response.payload[9] == 24U);
        CHECK(foc_protocol_read_u16(response.payload + 7) == 1000U);
        CHECK((int8_t)response.payload[10] == -7);
        read.sequence = ++request.sequence;
        foc_protocol_write_u16(read.payload, 1024U);
        CHECK(transact(&test, read, &response, FOC_PROTO_INVALID_ARGUMENT) == 0);
        read.sequence = ++request.sequence;
        read.length = 1;
        CHECK(transact(&test, read, &response, FOC_PROTO_INVALID_ARGUMENT) == 0);
        foc_board_cogging_state.table.code[1000] = 0;
    }
    /* NTC thresholds: a channel's derating must start below its limit. */
    request.sequence++;
    request.payload[0] = FOC_PARAM_MOTOR_DERATE_TEMP;
    request.payload[2] = 2;
    foc_protocol_write_u32(request.payload + 3, 0x42c80000U); /* 100 °C */
    CHECK(transact(&test, request, &response, FOC_PROTO_INVALID_ARGUMENT) == 0);
    CHECK(test.serial.thermal.motor.derate_c == 75.0f);
    request.sequence++;
    request.payload[0] = FOC_PARAM_MOTOR_LIMIT_TEMP;
    foc_protocol_write_u32(request.payload + 3, 0x42f00000U); /* 120 °C */
    CHECK(transact(&test, request, &response, FOC_PROTO_APPLIED) == 0);
    request.sequence++;
    request.payload[0] = FOC_PARAM_MOTOR_DERATE_TEMP;
    foc_protocol_write_u32(request.payload + 3, 0x42c80000U);
    CHECK(transact(&test, request, &response, FOC_PROTO_APPLIED) == 0);
    CHECK(
        test.serial.thermal.motor.derate_c == 100.0f && test.serial.thermal.motor.limit_c == 120.0f
    );
    /* Temperatures read NaN without a poll; θ is 0 and the ceiling the current limit. */
    const uint8_t reads[] = {
        FOC_PARAM_FET_TEMPERATURE,
        FOC_PARAM_MOTOR_TEMPERATURE,
        FOC_PARAM_I2T_LOAD,
        FOC_PARAM_THERMAL_CURRENT_LIMIT
    };
    uint32_t values[4];
    for (unsigned int index = 0U; index < 4U; ++index) {
        struct foc_protocol_frame read = request;
        read.sequence = ++request.sequence;
        read.type = FOC_MSG_PARAM_READ;
        read.length = 2;
        read.payload[0] = reads[index];
        read.payload[1] = 0;
        CHECK(transact(&test, read, &response, FOC_PROTO_OK) == 0);
        values[index] = foc_protocol_read_u32(response.payload + 14);
    }
    CHECK((values[0] & 0x7f800000U) == 0x7f800000U && (values[0] & 0x007fffffU) != 0U);
    CHECK((values[1] & 0x7f800000U) == 0x7f800000U && (values[1] & 0x007fffffU) != 0U);
    CHECK(values[2] == 0U);
    float ceiling;
    memcpy(&ceiling, &values[3], sizeof ceiling);
    CHECK(ceiling == test.runtime.pipeline.current.config.current_limit_a);
    request.sequence++;
    request.type = FOC_MSG_PARAM_WRITE;
    request.length = 7;
    request.payload[0] = 1;
    request.payload[2] = 1;
    request.payload[3] = 16;
    request.payload[4] = 0;
    request.payload[5] = 0;
    request.payload[6] = 0;
    foc_runtime_request_inhibit(&test.runtime, FOC_SAFE_FAULT_SAMPLE_INVALID);
    CHECK(transact(&test, request, &response, FOC_PROTO_BAD_STATE) == 0);
    return 0;
}

static void mock_tick(struct fixture *test, uint32_t at, enum foc_runtime_action action)
{
    test->time = at;
    struct foc_runtime_foreground_result result;
    (void)foc_runtime_foreground_poll(&test->runtime, at, 0, 0, &result);
    const struct foc_runtime_sensor sensor = mock_sensor(&test->runtime, at, at);
    (void)foc_runtime_h1_step(&test->runtime, &sensor, at, 0);
    const struct foc_runtime_fast_input input = {
        .adc = {1200, 2048, 2048, 2048, FOC_DEVICE_SAMPLE_ALL_VALID, at, at},
        .now_us = at + 1,
        .action = action,
    };
    (void)foc_runtime_h0_step(&test->runtime, &input);
}

static int test_lease_and_clear(void)
{
    struct fixture test = {0};
    const struct foc_pipeline_config pipeline = {
        .current = {{1, 2, 0, 1, .001f}, {1, 1}, {100, 100}, 2.5f},
        .adc = {.01f, .001f, 1, 2048, 2048, 2048, FOC_DEVICE_PHASE_ORDER_ABC},
        .pwm = {6000, FOC_DEVICE_PHASE_ORDER_ABC},
        .safety = {1000, 500, 500, 500, 3},
    };
    const struct foc_runtime_config policy = {
        .h0 = {100, 20, 10},
        .h1 = {100, 20, 10},
        .supervision = {100, 20, 10},
        .telemetry = {1000, 20, 10},
        .foreground_budget_us = 15,
        .angle_timeout_us = 100,
        .motion.iq_limit_a = 2.5f,
    };
    CHECK(foc_runtime_init(&test.runtime, &pipeline, &policy, 0) == 0);
    foc_serial_init(&test.serial, &test.runtime);
    struct foc_protocol_frame response;
    struct foc_protocol_frame request = {.flags = FOC_PROTOCOL_REQUEST, .type = FOC_MSG_HELLO};
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0);
    mock_tick(&test, 0, FOC_RUNTIME_ACTION_NONE);
    request.session = response.session;
    request.type = FOC_MSG_SETPOINT;
    request.length = 9;
    request.payload[3] = 0x80;
    request.payload[4] = 0x3f;
    test.time = 100;
    CHECK(transact(&test, request, &response, FOC_PROTO_ACCEPTED) == 0);
    CHECK(response.length == 5 && foc_protocol_read_u32(response.payload + 1) == 1U);
    mock_tick(&test, 100, FOC_RUNTIME_ACTION_ARM);
    mock_tick(&test, 200, FOC_RUNTIME_ACTION_START);
    mock_tick(&test, 300, FOC_RUNTIME_ACTION_NONE);
    CHECK(test.runtime.pipeline.mock.updates == 1U);
    struct foc_protocol_frame write = {
        .flags = FOC_PROTOCOL_REQUEST,
        .type = FOC_MSG_PARAM_WRITE,
        .session = request.session,
        .sequence = 1,
        .length = 7,
        .payload = {1, 0, 1, 16, 0, 0, 0},
    };
    CHECK(transact(&test, write, &response, FOC_PROTO_BAD_STATE) == 0);
    for (uint32_t at = 400; at <= 1200; at += 100) {
        test.time = at;
        if (at == 600) {
            inject(&test, &request, true);
            CHECK(receive(&test, &response) == FOC_PROTO_MORE);
        } else {
            CHECK(transact(&test, request, &response, FOC_PROTO_REPLAY) == 0);
        }
        mock_tick(&test, at, FOC_RUNTIME_ACTION_NONE);
        CHECK(test.runtime.pipeline.safety.last_command_us == 100U);
    }
    CHECK(test.runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK(test.runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_COMMAND_TIMEOUT);
    request.type = FOC_MSG_FAULT_CLEAR;
    request.length = 0;
    request.sequence = 2;
    test.time = 1202;
    CHECK(transact(&test, request, &response, FOC_PROTO_ACCEPTED) == 0);
    mock_tick(&test, 1300, FOC_RUNTIME_ACTION_NONE);
    mock_tick(&test, 1400, FOC_RUNTIME_ACTION_NONE);
    request.type = FOC_MSG_FAULT_READ;
    request.sequence++;
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0);
    CHECK(
        response.payload[1] == FOC_SAFE_STATE_DISABLED &&
        response.payload[17] == FOC_RUNTIME_CLEAR_APPLIED
    );
    CHECK((response.payload[3] & 8U) == 0U);
    return 0;
}

static int test_line_rate_pressure(void)
{
    struct fixture test = {0};
    struct foc_protocol_frame response;
    struct foc_protocol_frame request = {
        .flags = FOC_PROTOCOL_REQUEST,
        .type = FOC_MSG_HELLO,
        .sequence = 0U,
    };
    CHECK(init(&test) == 0);
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0);
    request.type = FOC_MSG_STATUS_READ;
    request.session = response.session;
    request.sequence = 1U;
    uint32_t next_poll = 4000U;
    for (unsigned int frame_index = 0; frame_index < 90U; ++frame_index) {
        uint8_t wire[FOC_PROTOCOL_WIRE_MAX];
        const size_t length = foc_protocol_encode(&request, wire, sizeof wire);
        if ((frame_index % 3U) == 1U) {
            wire[2] ^= 0x10U;
        }
        const size_t bytes = (frame_index % 3U) == 2U ? 180U : length;
        for (size_t index = 0; index < bytes; ++index) {
            test.time += 87U;
            foc_uart_rx_push(&test.uart, (frame_index % 3U) == 2U ? 0xffU : wire[index], false);
            runtime_tick(&test);
            if (test.time >= next_poll) {
                foc_serial_poll(&test.serial, &test.uart, &test.runtime, clock_us, &test.time);
                next_poll += 4000U;
            }
        }
        foc_uart_rx_push(&test.uart, 0U, false);
        request.sequence++;
    }
    for (unsigned int index = 0; index < 32U; ++index) {
        for (unsigned int tick = 0; tick < 46U; ++tick) {
            test.time += 87U;
            runtime_tick(&test);
        }
        foc_serial_poll(&test.serial, &test.uart, &test.runtime, clock_us, &test.time);
    }
    CHECK(test.serial.crc_errors > 0U && test.serial.dropped > 0U);
    CHECK(atomic_load(&test.uart.rx_lost) > 0U);
    CHECK(test.runtime.pipeline.mock.updates == 0U && test.output.declined_updates == 0U);
    CHECK(test.runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    CHECK(test.runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_NONE);
    printf(
        "uart pressure crc=%u dropped=%u rx_loss=%u updates=%u h0_late=%u h1_late=%u fg_late=%u\n",
        test.serial.crc_errors,
        test.serial.dropped,
        atomic_load(&test.uart.rx_lost),
        test.runtime.pipeline.mock.updates,
        test.runtime.h0.late_count,
        test.runtime.h1.late_count,
        test.runtime.supervision.late_count
    );
    return 0;
}

static int test_foreground_load(void)
{
    struct fixture test = {0};
    struct foc_protocol_frame response;
    struct foc_protocol_frame request = {.flags = FOC_PROTOCOL_REQUEST, .type = FOC_MSG_HELLO};
    CHECK(init(&test) == 0);
    CHECK(transact(&test, request, &response, FOC_PROTO_OK) == 0);
    request.session = response.session;
    request.type = FOC_MSG_STATUS_READ;
    for (uint32_t cycle = 0; cycle < 100; ++cycle) {
        test.time = cycle * FOC_BOARD_H0_US;
        request.sequence = cycle;
        uint8_t wire[FOC_PROTOCOL_WIRE_MAX];
        const size_t count = foc_protocol_encode(&request, wire, sizeof wire);
        for (size_t i = 0; i < count; ++i) {
            foc_uart_rx_push(&test.uart, wire[i], false);
        }
        const unsigned int before = atomic_load(&test.uart.rx_tail);
        poll(&test);
        CHECK(atomic_load(&test.uart.rx_tail) - before <= test.serial.rx_budget);
        const struct foc_runtime_sensor sensor = mock_sensor(&test.runtime, test.time, cycle);
        (void)foc_runtime_h1_step(&test.runtime, &sensor, test.time, 0);
        h0(&test);
        if (cycle % 7U == 0) {
            (void)receive(&test, &response);
        }
    }
    CHECK(test.runtime.supervision.due_count == 100U);
    CHECK(test.runtime.supervision.late_count == 0U && test.runtime.h0.late_count == 0U);
    CHECK(test.runtime.h0.missed == 0U && test.serial.dropped > 0U);
    CHECK(test.runtime.pipeline.mock.updates == 0U && test.output.declined_updates == 0U);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_uart() || test_commands() || test_host_policy_keeps_output_locked() ||
        test_backpressure() || test_parameters() || test_lease_and_clear() ||
        test_foreground_load() || test_line_rate_pressure();
}

/* ---------------------------------------- end of file ----------------------------------------- */
