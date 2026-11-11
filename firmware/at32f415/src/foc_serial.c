/**
 * @file    foc_serial.c
 * @brief   V1 session, Runtime command admission and RAM parameter policy.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Parse in Foreground after supervision and report scoped operation outcomes.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_serial.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

enum {
    PARAM_RX_BUDGET = 1,
    PARAM_CURRENT_LIMIT = 2,
    PARAM_POSITION_LIMIT = 3,
    PARAM_POLE_PAIRS = 16,
    PARAM_ELECTRICAL_OFFSET = 17,
    PARAM_VELOCITY_LIMIT = 18,
    PARAM_U32 = 1,
    PARAM_F32 = 2,
    UNIT_NONE = 0,
    UNIT_AMPERE = 1,
    UNIT_RADIAN = 2,
    UNIT_RAD_S = 3,
    FLAG_LOCKED = 1,
    FLAG_SOFTWARE_ACK = 2,
    FLAG_SENSOR_VALID = 4,
    FLAG_COMMAND_SEEN = 8,
    FLAG_INHIBIT_PENDING = 16,
    FLAG_DIRTY = 32,
    FLAG_FAST_SEEN = 64,
    FLAG_SAMPLE_SEEN = 128,
};

/* Private Types ---------------------------------------------------------------- */

struct foc_serial_parameter {
    uint32_t value, minimum, maximum;
    uint8_t type, unit;
    bool writable;
    bool calibration_required;
};

/* Private Helpers -------------------------------------------------------------- */

_Static_assert(
    sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
    "Wire float requires IEEE-754 binary32"
);

static uint32_t foc_serial_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float foc_serial_bits_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void foc_serial_capability(struct foc_protocol_frame *response)
{
    const uint8_t capability[] = {FOC_PROTO_OK, 1, 0, 1, 0, 7, 0, 15, 0};
    memcpy(response->payload, capability, sizeof capability);
    response->length = sizeof capability;
}

static int foc_serial_status(
    struct foc_serial *serial,
    struct foc_uart_buffer *uart,
    struct foc_runtime *runtime,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    const bool sensed = view.sensor_valid &&
        foc_safe_timestamp_fresh(now_us, view.sensor_us, runtime->config.angle_timeout_us);
    uint8_t *body = response->payload;
    body[1] = (uint8_t)view.state;
    body[2] = (uint8_t)view.mode;
    body[3] = FLAG_LOCKED | (view.output_confirmed ? FLAG_SOFTWARE_ACK : 0) |
        (sensed ? FLAG_SENSOR_VALID : 0) | (view.inhibit_pending ? FLAG_INHIBIT_PENDING : 0) |
        (serial->dirty ? FLAG_DIRTY : 0) | (view.command_seen ? FLAG_COMMAND_SEEN : 0) |
        (view.fast_seen ? FLAG_FAST_SEEN : 0) | (view.sample_seen ? FLAG_SAMPLE_SEEN : 0);
    body[4] = (uint8_t)view.action;
    foc_protocol_write_u32(body + 5, view.faults);
    foc_protocol_write_u32(body + 9, now_us);
    foc_protocol_write_u32(body + 13, (uint32_t)(int32_t)view.pipeline_status);
    body[17] = (uint8_t)view.clear_result;
    foc_protocol_write_u32(body + 18, view.sample_us);
    foc_protocol_write_u32(body + 22, view.command_us);
    foc_protocol_write_u32(body + 26, view.command_seq);
    response->length = 30;
    if (response->type == FOC_MSG_TELEMETRY_READ) {
        /* Locked Board1 has no calibrated live CSA measurement or applied PWM. */
        foc_protocol_write_u32(body + 30, 0);
        foc_protocol_write_u32(body + 34, 0);
        foc_protocol_write_u32(body + 38, foc_serial_float_bits(view.mechanical_rad));
        foc_protocol_write_u32(body + 42, foc_serial_float_bits(view.mechanical_rad_s));
        foc_protocol_write_u32(body + 46, view.sensor_us);
        foc_protocol_write_u32(body + 50, serial->crc_errors);
        foc_protocol_write_u32(body + 54, serial->dropped);
        foc_protocol_write_u32(body + 58, atomic_load(&uart->rx_lost));
        body[62] = 0; /* Both physical applied and calibrated current validity are false. */
        response->length = 63;
    }
    return FOC_PROTO_OK;
}

static bool foc_serial_parameter_read(
    const struct foc_serial *serial,
    const struct foc_runtime *runtime,
    uint16_t id,
    struct foc_serial_parameter *parameter
)
{
    *parameter = (struct foc_serial_parameter){.type = PARAM_F32};
    switch (id) {
    case PARAM_RX_BUDGET:
        *parameter = (struct foc_serial_parameter){
            .value = serial->rx_budget,
            .minimum = 8,
            .maximum = 32,
            .type = PARAM_U32,
            .unit = UNIT_NONE,
            .writable = true,
        };
        break;
    case PARAM_CURRENT_LIMIT:
        parameter->value = foc_serial_float_bits(serial->current_limit_a);
        parameter->minimum = foc_serial_float_bits(0.05f);
        parameter->maximum = foc_serial_float_bits(runtime->config.motion.iq_limit_a);
        parameter->unit = UNIT_AMPERE;
        parameter->writable = true;
        break;
    case PARAM_POSITION_LIMIT:
        parameter->value = foc_serial_float_bits(serial->position_limit_rad);
        parameter->minimum = foc_serial_float_bits(1.0f);
        parameter->maximum = foc_serial_float_bits(1000.0f);
        parameter->unit = UNIT_RADIAN;
        parameter->writable = true;
        break;
    case PARAM_POLE_PAIRS:
        parameter->type = PARAM_U32;
        parameter->value = runtime->config.motion.rotor.pole_pairs;
        parameter->calibration_required = true;
        break;
    case PARAM_ELECTRICAL_OFFSET:
        parameter->value =
            foc_serial_float_bits(runtime->config.motion.rotor.electrical_offset_rad);
        parameter->unit = UNIT_RADIAN;
        parameter->calibration_required = true;
        break;
    case PARAM_VELOCITY_LIMIT:
        parameter->value =
            foc_serial_float_bits(runtime->config.motion.position.velocity_limit_rad_s);
        parameter->unit = UNIT_RAD_S;
        break;
    default:
        return false;
    }
    if (!parameter->writable) {
        parameter->minimum = parameter->maximum = parameter->value;
    }
    return true;
}

static int foc_serial_parameter(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    struct foc_protocol_frame *response
)
{
    const bool writing = request->type == FOC_MSG_PARAM_WRITE;
    if (request->length != (writing ? 7U : 2U)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint16_t id = foc_protocol_read_u16(request->payload);
    struct foc_serial_parameter parameter;
    if (!foc_serial_parameter_read(serial, runtime, id, &parameter)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    if (writing) {
        struct foc_runtime_snapshot view;
        if (!parameter.writable) {
            return FOC_PROTO_NOT_SUPPORTED;
        }
        if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
            return FOC_PROTO_BUSY;
        }
        if (view.state != FOC_SAFE_STATE_DISABLED || view.inhibit_pending ||
            !view.output_confirmed || view.action != FOC_DEVICE_OUTPUT_INHIBIT) {
            return FOC_PROTO_BAD_STATE;
        }
        const uint32_t value = foc_protocol_read_u32(request->payload + 3);
        if (request->payload[2] != parameter.type) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
        if (parameter.type == PARAM_U32) {
            if (value < parameter.minimum || value > parameter.maximum) {
                return FOC_PROTO_INVALID_ARGUMENT;
            }
        } else {
            const float floating = foc_serial_bits_float(value);
            if (!isfinite(floating) || floating < foc_serial_bits_float(parameter.minimum) ||
                floating > foc_serial_bits_float(parameter.maximum)) {
                return FOC_PROTO_INVALID_ARGUMENT;
            }
        }
        switch (id) {
        case PARAM_RX_BUDGET:
            serial->rx_budget = value;
            break;
        case PARAM_CURRENT_LIMIT:
            serial->current_limit_a = foc_serial_bits_float(value);
            break;
        case PARAM_POSITION_LIMIT:
            serial->position_limit_rad = foc_serial_bits_float(value);
            break;
        default:
            return FOC_PROTO_INTERNAL_ERROR;
        }
        serial->dirty = true;
        parameter.value = value;
    }
    foc_protocol_write_u16(response->payload + 1, id);
    response->payload[3] = parameter.type;
    response->payload[4] = parameter.unit;
    response->payload[5] =
        2U | (parameter.writable ? 1U : 0U) | (parameter.calibration_required ? 4U : 0U);
    foc_protocol_write_u32(response->payload + 6, parameter.minimum);
    foc_protocol_write_u32(response->payload + 10, parameter.maximum);
    foc_protocol_write_u32(response->payload + 14, parameter.value);
    response->length = 18;
    return writing ? FOC_PROTO_APPLIED : FOC_PROTO_OK;
}

static int foc_serial_setpoint(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us
)
{
    if (request->length != 5U && request->length != 9U) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint8_t mode = request->payload[0];
    if (mode > FOC_RUNTIME_MODE_POSITION ||
        request->length != (mode == FOC_RUNTIME_MODE_CURRENT ? 9U : 5U)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const float first = foc_serial_bits_float(foc_protocol_read_u32(request->payload + 1));
    const float second = mode == FOC_RUNTIME_MODE_CURRENT
        ? foc_serial_bits_float(foc_protocol_read_u32(request->payload + 5))
        : 0.0f;
    if (!isfinite(first) || !isfinite(second)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    if ((mode == FOC_RUNTIME_MODE_CURRENT &&
         first * first + second * second > serial->current_limit_a * serial->current_limit_a) ||
        (mode == FOC_RUNTIME_MODE_VELOCITY &&
         fabsf(first) > runtime->config.motion.position.velocity_limit_rad_s) ||
        (mode == FOC_RUNTIME_MODE_POSITION && fabsf(first) > serial->position_limit_rad)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    if (view.state == FOC_SAFE_STATE_FAULT_LATCHED || view.inhibit_pending || mode != view.mode) {
        return FOC_PROTO_BAD_STATE;
    }
    const uint32_t seq = serial->publication_seq + 1U;
    int result;
    if (mode == FOC_RUNTIME_MODE_CURRENT) {
        const struct foc_pipeline_command command = {{first, second}, seq, now_us};
        result = foc_runtime_publish_command(runtime, &command, now_us);
    } else {
        const struct foc_runtime_command command = {
            .mode = (enum foc_runtime_mode)mode,
            .seq = seq,
            .produced_us = now_us,
            .target.motion_ref = first,
        };
        result = foc_runtime_publish_motion_command(runtime, &command, now_us);
    }
    if (result != FOC_RUNTIME_OK) {
        return FOC_PROTO_BAD_STATE;
    }
    serial->publication_seq = seq;
    return FOC_PROTO_ACCEPTED;
}

static int foc_serial_dispatch(
    struct foc_serial *serial,
    struct foc_uart_buffer *uart,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    if (request->type == FOC_MSG_PARAM_READ || request->type == FOC_MSG_PARAM_WRITE) {
        return foc_serial_parameter(serial, runtime, request, response);
    }
    if (request->type == FOC_MSG_SETPOINT) {
        const int result = foc_serial_setpoint(serial, runtime, request, now_us);
        if (result == FOC_PROTO_ACCEPTED) {
            foc_protocol_write_u32(response->payload + 1, serial->publication_seq);
            response->length = 5;
        }
        return result;
    }
    if (request->length != 0U) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    switch (request->type) {
    case FOC_MSG_CAPABILITY_READ:
        foc_serial_capability(response);
        return FOC_PROTO_OK;
    case FOC_MSG_STATUS_READ:
    case FOC_MSG_FAULT_READ:
    case FOC_MSG_TELEMETRY_READ:
        return foc_serial_status(serial, uart, runtime, now_us, response);
    case FOC_MSG_STOP:
        foc_runtime_revoke_command(runtime);
        return FOC_PROTO_ACCEPTED;
    case FOC_MSG_ARM:
    case FOC_MSG_START:
        return FOC_PROTO_OUTPUT_LOCKED;
    case FOC_MSG_FAULT_CLEAR: {
        const int result = foc_runtime_request_timeout_clear(runtime, now_us);
        return result == FOC_RUNTIME_OK      ? FOC_PROTO_ACCEPTED
            : result == FOC_RUNTIME_ERR_BUSY ? FOC_PROTO_BUSY
                                             : FOC_PROTO_BAD_STATE;
    }
    case FOC_MSG_PARAM_SAVE:
    case FOC_MSG_TELEMETRY_STREAM:
        return FOC_PROTO_NOT_SUPPORTED;
    default:
        return FOC_PROTO_UNKNOWN_COMMAND;
    }
}

static int foc_serial_request(
    struct foc_serial *serial,
    struct foc_uart_buffer *uart,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    if (request->flags != FOC_PROTOCOL_REQUEST) {
        return FOC_PROTO_BAD_FRAME;
    }
    if (request->type == FOC_MSG_HELLO) {
        if (request->session != 0U || request->length != 0U) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
        if (serial->session == 0U || request->sequence != serial->hello_seq) {
            foc_runtime_revoke_command(runtime);
            serial->session++;
            if (serial->session == 0U) {
                serial->session = 1U;
            }
            serial->hello_seq = request->sequence;
            serial->request_seen = false;
        }
        response->session = serial->session;
        foc_serial_capability(response);
        return FOC_PROTO_OK;
    }
    if (serial->session == 0U || request->session != serial->session) {
        return FOC_PROTO_BAD_SESSION;
    }
    if (!foc_safe_sequence_newer(request->sequence, serial->request_seq, serial->request_seen)) {
        return FOC_PROTO_REPLAY;
    }
    serial->request_seq = request->sequence;
    serial->request_seen = true;
    return foc_serial_dispatch(serial, uart, runtime, request, now_us, response);
}

static bool foc_serial_send(struct foc_uart_buffer *uart, const struct foc_protocol_frame *response)
{
    uint8_t wire[FOC_PROTOCOL_WIRE_MAX];
    const size_t length = foc_protocol_encode(response, wire, sizeof wire);
    return foc_uart_tx_submit(uart, wire, length);
}

/* Public Functions ------------------------------------------------------------- */

void foc_serial_init(struct foc_serial *serial, const struct foc_runtime *runtime)
{
    *serial = (struct foc_serial){
        .current_limit_a = runtime->config.motion.iq_limit_a,
        .position_limit_rad = 100.0f,
        .rx_budget = FOC_SERIAL_RX_DEFAULT,
    };
}

void foc_serial_poll(
    struct foc_serial *serial,
    struct foc_uart_buffer *uart,
    struct foc_runtime *runtime,
    foc_serial_clock_t clock,
    void *argument
)
{
    if (serial->stop_pending && foc_uart_tx_available(uart)) {
        const struct foc_protocol_frame response = {
            .type = FOC_MSG_STOP,
            .flags = FOC_PROTOCOL_RESPONSE,
            .sequence = serial->stop_seq,
            .session = serial->stop_session,
            .length = 1,
            .payload = {FOC_PROTO_ACCEPTED},
        };
        if (foc_serial_send(uart, &response)) {
            serial->stop_pending = false;
        }
    }
    for (uint32_t i = 0; i < serial->rx_budget; ++i) {
        uint16_t value;
        if (!foc_uart_rx_pop(uart, &value)) {
            return;
        }
        if (value == FOC_UART_RX_GAP) {
            serial->stream = (struct foc_protocol_stream){0};
            serial->dropped++;
            continue;
        }
        struct foc_protocol_frame request;
        const int parsed = foc_protocol_feed(&serial->stream, (uint8_t)value, &request);
        if (parsed == FOC_PROTO_MORE) {
            continue;
        }
        if (parsed != FOC_PROTO_OK && parsed != FOC_PROTO_UNKNOWN_COMMAND) {
            serial->crc_errors += parsed == FOC_PROTO_BAD_CRC;
            serial->dropped++;
            return;
        }
        const bool available = foc_uart_tx_available(uart);
        const bool stop = request.type == FOC_MSG_STOP && request.length == 0U &&
            request.flags == FOC_PROTOCOL_REQUEST && request.session == serial->session &&
            serial->session != 0U &&
            foc_safe_sequence_newer(request.sequence, serial->request_seq, serial->request_seen);
        if (!available && (!stop || serial->stop_pending)) {
            serial->dropped++;
            return;
        }
        struct foc_protocol_frame response = {
            .type = request.type,
            .flags = FOC_PROTOCOL_RESPONSE,
            .sequence = request.sequence,
            .session = request.session,
            .length = 1,
        };
        const int result =
            foc_serial_request(serial, uart, runtime, &request, clock(argument), &response);
        response.payload[0] = (uint8_t)result;
        if (result != FOC_PROTO_OK && result != FOC_PROTO_APPLIED && result != FOC_PROTO_ACCEPTED) {
            response.length = 1U;
        }
        if (available) {
            if (!foc_serial_send(uart, &response)) {
                serial->dropped++;
            }
        } else {
            serial->stop_seq = request.sequence;
            serial->stop_session = request.session;
            serial->stop_pending = true;
        }
        return;
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
