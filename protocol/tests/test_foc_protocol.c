/**
 * @file    test_foc_protocol.c
 * @brief   Verify FalconFOC V1 framing, integrity, and shared vectors.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Compare C wire bytes to fixed vectors also consumed by the Python tool.
 */

/* Includes --------------------------------------------------------------------- */

#define _CRT_SECURE_NO_WARNINGS
#include "foc_protocol.h"
#include "cobs.h"

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

/* Private Helpers -------------------------------------------------------------- */

static size_t from_hex(const char *hex, uint8_t *out)
{
    size_t n = 0;
    while (hex[0] != '\0' && hex[0] != '-') {
        unsigned int byte;
        if (sscanf(hex, "%2x", &byte) != 1) {
            return 0;
        }
        out[n++] = (uint8_t)byte;
        hex += 2;
    }
    return n;
}

static int test_vectors(const char *path)
{
    FILE *file = fopen(path, "r");
    CHECK(file != NULL);
    unsigned int type, flags, sequence, session, count = 0U;
    char payload[193], vector[233];
    while (fscanf(
               file,
               "%x %x %x %x %192s %232s",
               &type,
               &flags,
               &sequence,
               &session,
               payload,
               vector
           ) == 6) {
        struct foc_protocol_frame frame =
            {.type = type, .flags = flags, .sequence = sequence, .session = session};
        frame.length = from_hex(payload, frame.payload);
        uint8_t expected[FOC_PROTOCOL_WIRE_MAX], wire[FOC_PROTOCOL_WIRE_MAX];
        const size_t size = from_hex(vector, expected);
        CHECK(foc_protocol_encode(&frame, wire, sizeof wire) == size);
        CHECK(memcmp(wire, expected, size) == 0);
        struct foc_protocol_stream stream = {0};
        struct foc_protocol_frame decoded;
        for (unsigned int copy = 0; copy < 2; ++copy) {
            for (size_t i = 0; i + 1 < size; ++i) {
                CHECK(foc_protocol_feed(&stream, wire[i], &decoded) == FOC_PROTO_MORE);
            }
            CHECK(foc_protocol_feed(&stream, wire[size - 1], &decoded) == FOC_PROTO_OK);
            CHECK(
                decoded.sequence == sequence && decoded.session == session &&
                decoded.type == type && decoded.flags == flags && decoded.length == frame.length
            );
            CHECK(memcmp(decoded.payload, frame.payload, frame.length) == 0);
        }
        ++count;
    }
    CHECK(fclose(file) == 0);
    CHECK(count >= 16U);
    return 0;
}

static size_t pack(uint8_t *raw, size_t size, uint8_t *wire)
{
    size_t encoded = 0;
    (void)cobs_encode(raw, size, wire, FOC_PROTOCOL_WIRE_MAX - 1U, &encoded);
    return encoded;
}

static int test_rejection(void)
{
    uint8_t raw[FOC_PROTOCOL_RAW_MAX] = {0x46, 0x46, 1, FOC_MSG_SETPOINT, 1};
    raw[14] = 1;
    const size_t size = FOC_PROTOCOL_HEADER + 3U;
    raw[16] = 0x32;
    foc_protocol_write_u16(raw + 17, foc_protocol_crc(raw, 17));
    struct foc_protocol_frame frame;
    uint8_t wire[FOC_PROTOCOL_WIRE_MAX];
    for (size_t offset = 0; offset < size; ++offset) {
        for (unsigned int bit = 0; bit < 8; ++bit) {
            raw[offset] ^= (uint8_t)(1U << bit);
            CHECK(foc_protocol_decode(wire, pack(raw, size, wire), &frame) == FOC_PROTO_BAD_CRC);
            raw[offset] ^= (uint8_t)(1U << bit);
        }
    }
    const size_t offsets[] = {0, 2, 3, 4, 5, 14};
    const int expected[] = {
        FOC_PROTO_BAD_FRAME,
        FOC_PROTO_UNSUPPORTED_VERSION,
        FOC_PROTO_UNKNOWN_COMMAND,
        FOC_PROTO_BAD_FRAME,
        FOC_PROTO_BAD_FRAME,
        FOC_PROTO_BAD_FRAME
    };
    for (unsigned int i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        const uint8_t saved = raw[offsets[i]];
        raw[offsets[i]] = 0x7f;
        foc_protocol_write_u16(raw + 17, foc_protocol_crc(raw, 17));
        CHECK(foc_protocol_decode(wire, pack(raw, size, wire), &frame) == expected[i]);
        raw[offsets[i]] = saved;
    }
    return 0;
}

static int test_stream(void)
{
    struct foc_protocol_stream stream = {0};
    struct foc_protocol_frame frame = {
        .type = FOC_MSG_SETPOINT,
        .flags = FOC_PROTOCOL_REQUEST,
        .length = FOC_PROTOCOL_PAYLOAD_MAX
    };
    uint8_t wire[FOC_PROTOCOL_WIRE_MAX];
    memset(frame.payload, 0xab, sizeof frame.payload);
    frame.payload[10] = 0U;
    CHECK(foc_protocol_encode(&frame, wire, sizeof wire - 1U) == 0U);
    CHECK(foc_protocol_encode(&frame, wire, sizeof wire) == sizeof wire);
    CHECK(foc_protocol_feed(&stream, 0, &frame) == FOC_PROTO_MORE);
    for (size_t i = 0; i < sizeof wire; ++i) {
        CHECK(foc_protocol_feed(&stream, 0xff, &frame) == FOC_PROTO_MORE);
    }
    CHECK(foc_protocol_feed(&stream, 0, &frame) == FOC_PROTO_BAD_FRAME);
    CHECK(foc_protocol_feed(&stream, 2, &frame) == FOC_PROTO_MORE);
    CHECK(foc_protocol_feed(&stream, 0, &frame) == FOC_PROTO_BAD_FRAME);
    for (size_t i = 0; i < sizeof wire - 1U; ++i) {
        CHECK(foc_protocol_feed(&stream, wire[i], &frame) == FOC_PROTO_MORE);
    }
    CHECK(foc_protocol_feed(&stream, 0, &frame) == FOC_PROTO_OK);
    CHECK(frame.length == FOC_PROTOCOL_PAYLOAD_MAX && frame.payload[10] == 0U);
    return 0;
}

static uint32_t next_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static int test_random_inputs(void)
{
    struct foc_protocol_stream stream = {0};
    struct foc_protocol_frame frame;
    uint8_t bytes[192];
    uint32_t state = UINT32_C(0x5eeda11);
    for (unsigned int iteration = 0; iteration < 10000U; ++iteration) {
        const size_t length = next_random(&state) % sizeof bytes;
        for (size_t index = 0; index < length; ++index) {
            bytes[index] = (uint8_t)next_random(&state);
            (void)foc_protocol_feed(&stream, bytes[index], &frame);
        }
        (void)foc_protocol_feed(&stream, 0U, &frame);
        struct foc_protocol_frame generated = {
            .type = (uint8_t)(next_random(&state) & 0x7fU),
            .flags = (uint8_t)((next_random(&state) % 4U) + 1U),
            .sequence = next_random(&state),
            .session = next_random(&state),
            .length = (uint16_t)(next_random(&state) % (FOC_PROTOCOL_PAYLOAD_MAX + 1U)),
        };
        for (unsigned int index = 0; index < generated.length; ++index) {
            generated.payload[index] = (uint8_t)next_random(&state);
        }
        if (generated.flags <= FOC_PROTOCOL_EVENT) {
            uint8_t wire[FOC_PROTOCOL_WIRE_MAX];
            const size_t wire_length = foc_protocol_encode(&generated, wire, sizeof wire);
            CHECK(wire_length != 0U);
            for (size_t index = 0; index < wire_length; ++index) {
                const int status = foc_protocol_feed(&stream, wire[index], &frame);
                if (index + 1U == wire_length) {
                    CHECK(status == FOC_PROTO_OK || status == FOC_PROTO_UNKNOWN_COMMAND);
                }
            }
        }
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(foc_protocol_crc((const uint8_t *)"123456789", 9U) == 0x29b1U);
    return test_vectors(argv[1]) || test_rejection() || test_stream() || test_random_inputs();
}

/* ---------------------------------------- end of file ----------------------------------------- */
