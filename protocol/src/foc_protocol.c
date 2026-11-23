/**
 * @file    foc_protocol.c
 * @brief   Portable FalconFOC V1 framing and CRC.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Validate bounded bytes before publishing a decoded frame.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_protocol.h"
#include "cobs.h"

#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

static bool foc_protocol_flag_valid(uint8_t flags)
{
    return flags == FOC_PROTOCOL_REQUEST || flags == FOC_PROTOCOL_RESPONSE ||
        flags == FOC_PROTOCOL_EVENT;
}

static bool foc_protocol_known(uint8_t type)
{
    switch (type) {
    case FOC_MSG_HELLO:
    case FOC_MSG_CAPABILITY_READ:
    case FOC_MSG_STATUS_READ:
    case FOC_MSG_SETPOINT:
    case FOC_MSG_STOP:
    case FOC_MSG_ARM:
    case FOC_MSG_START:
    case FOC_MSG_SET_MODE:
    case FOC_MSG_LIFECYCLE_READ:
    case FOC_MSG_FAULT_READ:
    case FOC_MSG_FAULT_CLEAR:
    case FOC_MSG_FAULT_SNAPSHOT_READ:
    case FOC_MSG_PARAM_READ:
    case FOC_MSG_PARAM_WRITE:
    case FOC_MSG_PARAM_SAVE:
    case FOC_MSG_CONFIG_ERASE:
    case FOC_MSG_TELEMETRY_READ:
    case FOC_MSG_TELEMETRY_STREAM:
    case FOC_MSG_CALIBRATE_START:
    case FOC_MSG_CALIBRATE_READ:
    case FOC_MSG_CALIBRATE_ABORT:
    case FOC_MSG_COMMUTATION_WRITE:
        return true;
    default:
        return false;
    }
}

/* Public Functions ------------------------------------------------------------- */

uint16_t foc_protocol_read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

uint32_t foc_protocol_read_u32(const uint8_t *bytes)
{
    return (uint32_t)foc_protocol_read_u16(bytes) |
        ((uint32_t)foc_protocol_read_u16(bytes + 2) << 16);
}

void foc_protocol_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

void foc_protocol_write_u32(uint8_t *bytes, uint32_t value)
{
    foc_protocol_write_u16(bytes, (uint16_t)value);
    foc_protocol_write_u16(bytes + 2, (uint16_t)(value >> 16));
}

uint16_t foc_protocol_crc(const uint8_t *bytes, size_t length)
{
    uint16_t crc = 0xffffU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)((uint16_t)bytes[i] << 8);
        for (unsigned int bit = 0; bit < 8U; ++bit) {
            crc = (uint16_t)(((uint32_t)crc << 1) ^ ((crc & 0x8000U) != 0U ? 0x1021U : 0U));
        }
    }
    return crc;
}

size_t foc_protocol_encode(const struct foc_protocol_frame *frame, uint8_t *wire, size_t capacity)
{
    if (frame == NULL || wire == NULL || frame->length > FOC_PROTOCOL_PAYLOAD_MAX ||
        !foc_protocol_flag_valid(frame->flags)) {
        return 0;
    }
    const size_t checked = FOC_PROTOCOL_HEADER + frame->length;
    if (capacity < checked + 4U) {
        return 0;
    }
    uint8_t raw[FOC_PROTOCOL_RAW_MAX] =
        {0x46, 0x46, FOC_PROTOCOL_VERSION, frame->type, frame->flags};
    foc_protocol_write_u32(raw + 6, frame->sequence);
    foc_protocol_write_u32(raw + 10, frame->session);
    foc_protocol_write_u16(raw + 14, frame->length);
    memcpy(raw + FOC_PROTOCOL_HEADER, frame->payload, frame->length);
    foc_protocol_write_u16(raw + checked, foc_protocol_crc(raw, checked));
    size_t encoded;
    if (cobs_encode(raw, checked + 2U, wire, capacity - 1U, &encoded) != 0) {
        return 0;
    }
    wire[encoded] = 0U;
    return encoded + 1U;
}

enum foc_protocol_status
foc_protocol_decode(const uint8_t *wire, size_t length, struct foc_protocol_frame *frame)
{
    uint8_t raw[FOC_PROTOCOL_RAW_MAX];
    size_t size;
    if (wire == NULL || frame == NULL || length == 0U || length >= FOC_PROTOCOL_WIRE_MAX ||
        cobs_decode(wire, length, raw, sizeof raw, &size) != 0 || size < FOC_PROTOCOL_HEADER + 2U) {
        return FOC_PROTO_BAD_FRAME;
    }
    const size_t checked = size - 2U;
    if (foc_protocol_read_u16(raw + checked) != foc_protocol_crc(raw, checked)) {
        return FOC_PROTO_BAD_CRC;
    }
    const uint16_t payload = foc_protocol_read_u16(raw + 14);
    if (raw[0] != 0x46U || raw[1] != 0x46U || raw[5] != 0U || !foc_protocol_flag_valid(raw[4]) ||
        payload > FOC_PROTOCOL_PAYLOAD_MAX || checked != FOC_PROTOCOL_HEADER + (size_t)payload) {
        return FOC_PROTO_BAD_FRAME;
    }
    if (raw[2] != FOC_PROTOCOL_VERSION) {
        return FOC_PROTO_UNSUPPORTED_VERSION;
    }
    frame->type = raw[3];
    frame->flags = raw[4];
    frame->sequence = foc_protocol_read_u32(raw + 6);
    frame->session = foc_protocol_read_u32(raw + 10);
    frame->length = payload;
    memcpy(frame->payload, raw + FOC_PROTOCOL_HEADER, payload);
    return foc_protocol_known(frame->type) ? FOC_PROTO_OK : FOC_PROTO_UNKNOWN_COMMAND;
}

enum foc_protocol_status foc_protocol_feed(
    struct foc_protocol_stream *stream,
    uint8_t byte,
    struct foc_protocol_frame *frame
)
{
    if (byte != 0U) {
        if (!stream->dropping) {
            if (stream->used == sizeof stream->data) {
                stream->dropping = true;
            } else {
                stream->data[stream->used++] = byte;
            }
        }
        return FOC_PROTO_MORE;
    }
    const uint8_t used = stream->used;
    const bool dropped = stream->dropping;
    stream->used = 0U;
    stream->dropping = false;
    if (dropped) {
        return FOC_PROTO_BAD_FRAME;
    }
    return used == 0U ? FOC_PROTO_MORE : foc_protocol_decode(stream->data, used, frame);
}

/* ---------------------------------------- end of file ----------------------------------------- */
