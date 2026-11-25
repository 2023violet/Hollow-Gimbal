/**
 * @file    cobs.h
 * @brief   Portable COBS encoder and decoder with explicit buffer contracts
 * @version v1.0.0
 * @date    2026.04.24
 *
 * Design overview
 * ---------------
 * `cobs` provides a portable Consistent Overhead Byte Stuffing codec for
 * framed byte streams. The API keeps buffer ownership with the caller and
 * rejects malformed frames before any out-of-bounds read or write occurs.
 *
 * Usage example
 * -------------
 * uint8_t tx_buf[COBS_ENCODE_MAX_LEN(sizeof(payload)) + 1U];
 * size_t  enc_len = 0U;
 *
 * cobs_encode(payload, payload_len, tx_buf, sizeof(tx_buf), &enc_len);
 * tx_buf[enc_len++] = 0x00U;
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - Source and destination buffers are caller-owned.
 * - The codec keeps no persistent state; each call is independent.
 * - Encode and decode do not permit overlapping source and destination buffers.
 * - @note Re-entrant, thread-safe, and ISR-safe when the caller provides
 *   valid non-overlapping caller-owned buffers.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no RTOS dependency.
 * - Runtime is linear in the caller-provided buffer length.
 *
 * Failure / recovery
 * ------------------
 * - Invalid pointer contracts fail with `-EINVAL`.
 * - Destination exhaustion fails with `-ENOSPC`.
 * - Malformed encoded frames fail with `-EBADMSG` before out-of-bounds access.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/**
 * @brief  Compute the maximum encoded length for a raw payload of @p N bytes.
 *
 * COBS overhead is at most one extra code byte per 254 payload bytes, plus
 * one leading code byte for the first run.
 *
 * @param  N  Raw payload length in bytes.
 *
 * @note   Macro contains division. Ensure N is a compile-time constant to
 *         allow compiler optimization.
 */
#define COBS_ENCODE_MAX_LEN(N) ((N) + ((N) / 254U) + 1U)

/**
 * @brief  Maximum value of a COBS code byte.
 */
#define COBS_MAX_CODE          (0xFFU)

/**
 * @brief  Number of data bytes described by a maximal code byte.
 */
#define COBS_MAX_RUN           (254U)

/* Public Types --------------------------------------------------------------------------------- */

/* None: the codec works on caller-owned uint8_t buffers and size_t lengths. */

/* Public Functions ----------------------------------------------------------------------------- */

/* Encoding ------------------------------------------------------------------------------------- */

/**
 * @brief  Encode a raw payload with COBS.
 *
 * The trailing `0x00` frame delimiter is not appended; callers add it
 * separately when needed.
 *
 * @param  src          Raw payload. May be NULL only when @p src_len is 0.
 * @param  src_len      Number of raw bytes in @p src.
 * @param  dst          Destination buffer. Must not overlap @p src.
 * @param  dst_max_len  Capacity of @p dst in bytes.
 * @param  out_len      Output number of bytes written. Set to 0 on error.
 *
 * @retval 0        Encode succeeded.
 * @retval -EINVAL  Invalid pointer contract.
 * @retval -ENOSPC  Destination buffer is too small.
 */
int cobs_encode(const uint8_t *src,
                size_t         src_len,
                uint8_t       *dst,
                size_t         dst_max_len,
                size_t        *out_len);

/* Decoding ------------------------------------------------------------------------------------- */

/**
 * @brief  Decode a COBS-encoded payload.
 *
 * The input buffer must not include the trailing `0x00` delimiter.
 *
 * @param  src          Encoded frame without the trailing delimiter. May be
 *                      NULL only when @p src_len is 0.
 * @param  src_len      Number of encoded bytes in @p src.
 * @param  dst          Destination buffer for decoded bytes.
 * @param  dst_max_len  Capacity of @p dst in bytes.
 * @param  out_len      Output number of bytes written. Set to 0 on error.
 *
 * @retval 0         Decode succeeded.
 * @retval -EINVAL   Invalid pointer contract.
 * @retval -ENOSPC   Decoded output exceeds @p dst_max_len.
 * @retval -EBADMSG  Encoded frame is malformed.
 */
int cobs_decode(const uint8_t *src,
                size_t         src_len,
                uint8_t       *dst,
                size_t         dst_max_len,
                size_t        *out_len);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
