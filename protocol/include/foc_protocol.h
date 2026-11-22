/**
 * @file    foc_protocol.h
 * @brief   Bounded FalconFOC V1 wire codec and streaming frame parser.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Encode explicit little-endian fields and recover at a zero delimiter.
 *
 * Usage example
 * -------------
 * Zero a stream, feed bytes to foc_protocol_feed, and dispatch complete frames.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns storage and serializes each stream. No pointer is retained.
 *
 * Failure / recovery
 * ------------------
 * A rejected frame cannot be dispatched. Unknown types have a trusted header
 * after successful integrity checks and may receive UNKNOWN_COMMAND.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Public Types ---------------------------------------------------------------- */

/** V1 version and buffer sizes including the delimiter in WIRE_MAX. */
enum foc_protocol_limits {
    FOC_PROTOCOL_VERSION = 1,
    FOC_PROTOCOL_HEADER = 16,
    FOC_PROTOCOL_PAYLOAD_MAX = 96,
    FOC_PROTOCOL_RAW_MAX = FOC_PROTOCOL_HEADER + FOC_PROTOCOL_PAYLOAD_MAX + 2,
    FOC_PROTOCOL_WIRE_MAX = FOC_PROTOCOL_RAW_MAX + 2,
};

/** Single-bit frame roles. Combinations are invalid. */
enum foc_protocol_flag {
    FOC_PROTOCOL_REQUEST = 1,
    FOC_PROTOCOL_RESPONSE = 2,
    FOC_PROTOCOL_EVENT = 4,
};

/** V1 message IDs. */
enum foc_protocol_message {
    FOC_MSG_HELLO = 0x01,
    FOC_MSG_CAPABILITY_READ = 0x02,
    FOC_MSG_STATUS_READ = 0x03,
    FOC_MSG_SETPOINT = 0x10,
    FOC_MSG_STOP = 0x11,
    FOC_MSG_ARM = 0x12,
    FOC_MSG_START = 0x13,
    /** Select CURRENT, VELOCITY or POSITION while disabled (M1, appended). */
    FOC_MSG_SET_MODE = 0x14,
    /** Read the latest ARM/START/SET_MODE result (M1, appended). */
    FOC_MSG_LIFECYCLE_READ = 0x15,
    FOC_MSG_FAULT_READ = 0x20,
    FOC_MSG_FAULT_CLEAR = 0x21,
    FOC_MSG_PARAM_READ = 0x30,
    FOC_MSG_PARAM_WRITE = 0x31,
    FOC_MSG_PARAM_SAVE = 0x32,
    /** Erase stored settings; the next boot uses the boot configuration (M2, appended). */
    FOC_MSG_CONFIG_ERASE = 0x33,
    FOC_MSG_TELEMETRY_READ = 0x40,
    FOC_MSG_TELEMETRY_STREAM = 0x41,
    /** Start on-MCU calibration CAL-1 and CAL-2 (M3, appended). */
    FOC_MSG_CALIBRATE_START = 0x50,
    /** Read calibration progress and results (M3, appended). */
    FOC_MSG_CALIBRATE_READ = 0x51,
    /** Abort a running calibration (M3, appended). */
    FOC_MSG_CALIBRATE_ABORT = 0x52,
};

/** First response payload byte. MORE is local to the stream parser. */
enum foc_protocol_status {
    FOC_PROTO_OK = 0,
    FOC_PROTO_ACCEPTED = 1,
    FOC_PROTO_APPLIED = 2,
    FOC_PROTO_BAD_FRAME = 3,
    FOC_PROTO_BAD_CRC = 4,
    FOC_PROTO_UNSUPPORTED_VERSION = 5,
    FOC_PROTO_UNKNOWN_COMMAND = 6,
    FOC_PROTO_INVALID_ARGUMENT = 7,
    FOC_PROTO_BAD_SESSION = 8,
    FOC_PROTO_REPLAY = 9,
    FOC_PROTO_BUSY = 10,
    FOC_PROTO_BAD_STATE = 11,
    FOC_PROTO_OUTPUT_LOCKED = 12,
    FOC_PROTO_NOT_SUPPORTED = 13,
    FOC_PROTO_INTERNAL_ERROR = 14,
    FOC_PROTO_MORE = 255,
};

/** Decoded application frame. The version and reserved byte are implicit. */
struct foc_protocol_frame {
    /** Sequence copied back in the response. */
    uint32_t sequence;
    /** Logical session. Zero is reserved for initial HELLO. */
    uint32_t session;
    /** Payload length in bytes. */
    uint16_t length;
    /** Wire message ID. */
    uint8_t type;
    /** One wire role flag. */
    uint8_t flags;
    /** Bytes before the CRC. */
    uint8_t payload[FOC_PROTOCOL_PAYLOAD_MAX];
};

/** Streaming parser state, owned by the byte consumer. */
struct foc_protocol_stream {
    /** Encoded bytes before the delimiter. */
    uint8_t data[FOC_PROTOCOL_WIRE_MAX - 1];
    /** Bytes currently buffered. */
    uint8_t used;
    /** Discard through the next delimiter after an oversize frame. */
    bool dropping;
};

/* Public Functions ------------------------------------------------------------- */

/** Read two little-endian bytes at a caller-validated offset. */
uint16_t foc_protocol_read_u16(const uint8_t *bytes);
/** Read four little-endian bytes at a caller-validated offset. */
uint32_t foc_protocol_read_u32(const uint8_t *bytes);

/** Write two little-endian bytes at a caller-validated offset. */
void foc_protocol_write_u16(uint8_t *bytes, uint16_t value);
/** Write four little-endian bytes at a caller-validated offset. */
void foc_protocol_write_u32(uint8_t *bytes, uint32_t value);

/** Compute CRC-16/CCITT-FALSE across the given byte range. */
uint16_t foc_protocol_crc(const uint8_t *bytes, size_t length);

/** Encode a frame with its delimiter. Return zero for invalid input/capacity. */
size_t foc_protocol_encode(const struct foc_protocol_frame *frame, uint8_t *wire, size_t capacity);

/**
 * Decode one encoded frame excluding its delimiter.
 * Input is borrowed for the call. The output is usable only for OK or
 * UNKNOWN_COMMAND. Other results leave the destination unchanged.
 */
enum foc_protocol_status foc_protocol_decode(const uint8_t *wire,
                                             size_t length,
                                             struct foc_protocol_frame *frame);

/**
 * Consume one byte in the serialized stream owner. MORE includes empty delimiters.
 * stream and frame must be non-NULL. Output validity follows decode().
 */
enum foc_protocol_status foc_protocol_feed(struct foc_protocol_stream *stream,
                                           uint8_t byte,
                                           struct foc_protocol_frame *frame);

/* ---------------------------------------- end of file ----------------------------------------- */
