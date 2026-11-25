/**
 * @file    cobs.c
 * @brief   Themis COBS Encoder/Decoder - Buffer-Safe, Malformed-Frame-Immune
 * @version v1.0.0
 * @date    2026.03.02
 *
 * Implementation role
 * -------------------
 * - pairs with `cobs.h`
 * - implements the byte-walk encode/decode loops and every boundary check
 * - keeps malformed-frame rejection local to the codec instead of pushing it
 *   to transport-specific callers
 *
 * Key collaborators
 * -----------------
 * - `cobs.h`: caller-facing buffer and error contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "cobs.h"
#include <errno.h>
#include <stdbool.h>

/* Private Helpers ------------------------------------------------------------------------------ */

/* None: each codec loop keeps its boundary checks inline. */

/* Public Functions ----------------------------------------------------------------------------- */

int cobs_encode(const uint8_t *src,
                size_t         src_len,
                uint8_t       *dst,
                size_t         dst_max_len,
                size_t        *out_len)
{
    /* Validate required source, destination, and result storage. */
    if (dst == NULL || out_len == NULL) {
        return -EINVAL;
    }
    if (src_len > 0U && src == NULL) {
        return -EINVAL;
    }

    *out_len = 0U;

    /* Reserve one byte for the first run-length code. */
    if (dst_max_len == 0U) {
        return -ENOSPC;
    }

    size_t  dst_pos  = 0U; /* Next write index in dst              */
    size_t  code_ptr = 0U; /* Index of the code byte being built   */
    uint8_t code     = 1U; /* (run_length + 1); starts at 1        */

    dst_pos = 1U; /* Reserve dst[0] for the first code    */

    for (size_t i = 0U; i < src_len; i++) {
        uint8_t byte = src[i];

        if (byte == 0x00U) {
            /*
             * Found a zero byte in the source.
             * Flush the current run: write the accumulated code into the
             * reserved code slot, then reserve a new code slot.
             */
            dst[code_ptr] = code;

            /* Reserve the next code slot before continuing the stream walk. */
            if (dst_pos >= dst_max_len) {
                *out_len = 0U;
                return -ENOSPC;
            }
            code_ptr = dst_pos;
            dst_pos++;
            code = 1U;

        } else {
            /*
             * Non-zero byte: append it to the current run.
             */
            if (dst_pos >= dst_max_len) {
                *out_len = 0U;
                return -ENOSPC;
            }
            dst[dst_pos++] = byte;
            code++;

            /*
             * If the run has reached the maximum length (254 data bytes,
             * code == 0xFF == 255), flush it immediately without emitting
             * an implicit zero.  The code value 0xFF signals "no zero in
             * this run" to the decoder.
             */
            if (code == COBS_MAX_CODE) {
                dst[code_ptr] = COBS_MAX_CODE;

                /* Reserve the next code slot for the next maximal run. */
                if (dst_pos >= dst_max_len) {
                    *out_len = 0U;
                    return -ENOSPC;
                }
                code_ptr = dst_pos;
                dst_pos++;
                code = 1U;
            }
        }
    }

    /* Flush the final (possibly partial) run. */
    dst[code_ptr] = code;

    *out_len = dst_pos;
    return 0;
}

int cobs_decode(const uint8_t *src,
                size_t         src_len,
                uint8_t       *dst,
                size_t         dst_max_len,
                size_t        *out_len)
{
    /* Validate required source, destination, and result storage. */
    if (dst == NULL || out_len == NULL) {
        return -EINVAL;
    }
    if (src_len > 0U && src == NULL) {
        return -EINVAL;
    }

    *out_len = 0U;

    /* Empty frame: valid, produces zero output bytes. */
    if (src_len == 0U) {
        return 0;
    }

    size_t src_pos = 0U; /* Next read index in src  */
    size_t dst_pos = 0U; /* Next write index in dst */

    while (src_pos < src_len) {
        uint8_t code = src[src_pos];

        /* A zero code byte inside the frame means corruption or wrong framing. */
        if (code == 0x00U) {
            *out_len = 0U;
            return -EBADMSG;
        }

        /* Reject any code byte that would jump past the encoded frame. */
        size_t next_code_pos = src_pos + (size_t)code;

        if (next_code_pos > src_len) {
            *out_len = 0U;
            return -EBADMSG;
        }

        src_pos++; /* Advance past the code byte; data bytes follow. */

        /* Copy the run payload and validate every emitted byte. */
        while (src_pos < next_code_pos) {
            uint8_t data = src[src_pos];

            /* Zero cannot appear in the data region of a valid encoded run. */
            if (data == 0x00U) {
                *out_len = 0U;
                return -EBADMSG;
            }

            /* Check destination capacity before each decoded byte write. */
            if (dst_pos >= dst_max_len) {
                *out_len = 0U;
                return -ENOSPC;
            }

            dst[dst_pos++] = data;
            src_pos++;
        }

        /* Emit the implicit zero unless the run explicitly suppresses it. */
        bool is_max_run  = (code == COBS_MAX_CODE);
        bool is_last_run = (next_code_pos == src_len);

        if (!is_max_run && !is_last_run) {
            if (dst_pos >= dst_max_len) {
                *out_len = 0U;
                return -ENOSPC;
            }
            dst[dst_pos++] = 0x00U;
        }
    }

    *out_len = dst_pos;
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
