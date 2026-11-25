/**
 * @file    crc.h
 * @brief   Streaming CRC helpers with explicit variant contracts
 * @version v1.1.0
 * @date    2026.04.04
 *
 * Design overview
 * ---------------
 * `crc` exposes explicit streaming helpers for CRC-8/MAXIM,
 * CRC-16/MODBUS, and CRC-32/IEEE 802.3. The caller owns the running CRC
 * state and may continue a checksum across disjoint buffers without copying
 * them into a temporary linear block.
 *
 * Usage example
 * -------------
 * uint32_t crc = CRC32_IEEE_INIT;
 *
 * crc = crc32_ieee(crc, header, header_len);
 * crc = crc32_ieee(crc, payload, payload_len);
 * crc = CRC32_IEEE_FINALIZE(crc);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - The API is stateless: no hidden context object and no global mutable
 *   runtime state.
 * - Callers choose the variant explicitly and keep the running checksum value.
 * - Check values in this header use the ASCII string `"123456789"`.
 * - @note Re-entrant, thread-safe, and ISR-safe when the caller provides
 *   valid caller-owned input buffers.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no callback or RTOS dependency.
 * - Runtime is linear in the input length supplied to each call.
 *
 * Failure / recovery
 * ------------------
 * - These helpers do not return error codes; NULL data with zero length is a
 *   no-op that returns the incoming CRC state unchanged.
 * - `crc32_ieee()` returns the internal running register state; callers apply
 *   `CRC32_IEEE_FINALIZE()` exactly once when a wire value is needed.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros - Initial Values ---------------------------------------------------------------- */

/**
 * @brief  Initial CRC value for CRC-8/MAXIM.
 */
#define CRC8_MAXIM_INIT          (0x00U)

/**
 * @brief  Initial CRC value for CRC-16/MODBUS.
 */
#define CRC16_MODBUS_INIT        (0xFFFFU)

/**
 * @brief  Initial internal CRC state for CRC-32/IEEE 802.3.
 */
#define CRC32_IEEE_INIT          (0xFFFFFFFFU)

/* Public Macros - Check Values ------------------------------------------------------------------ */

/**
 * @brief  Expected result of crc8_maxim(CRC8_MAXIM_INIT, "123456789", 9).
 */
#define CRC8_MAXIM_CHECK         (0xA1U)

/**
 * @brief  Expected result of crc16_modbus(CRC16_MODBUS_INIT, "123456789", 9).
 */
#define CRC16_MODBUS_CHECK       (0x4B37U)

/**
 * @brief  Expected wire value of CRC-32/IEEE for "123456789".
 */
#define CRC32_IEEE_CHECK         (0xCBF43926U)

/**
 * @brief  Convert an internal CRC-32/IEEE state to the wire value.
 */
#define CRC32_IEEE_FINALIZE(crc) ((uint32_t)(crc) ^ 0xFFFFFFFFU)

/* Public Types --------------------------------------------------------------------------------- */

/* None: every variant works on plain uint8_t, uint16_t and uint32_t values. */

/* Public Functions ----------------------------------------------------------------------------- */

/* Streaming Steps ------------------------------------------------------------------------------ */

/**
 * @brief  Compute or continue a CRC-8/MAXIM checksum.
 *
 * @param  current_crc  Running CRC state. Use CRC8_MAXIM_INIT for the first
 *                      chunk and the previous return value for later chunks.
 * @param  data         Data buffer. May be NULL iff len == 0.
 * @param  len          Number of bytes to process.
 *
 * @return Updated CRC-8 value.
 */
uint8_t crc8_maxim(uint8_t current_crc, const uint8_t *data, size_t len);

/**
 * @brief  Compute or continue a CRC-16/MODBUS checksum.
 *
 * @param  current_crc  Running CRC state. Use CRC16_MODBUS_INIT for the first
 *                      chunk and the previous return value for later chunks.
 * @param  data         Data buffer. May be NULL iff len == 0.
 * @param  len          Number of bytes to process.
 *
 * @return Updated CRC-16 value.
 */
uint16_t crc16_modbus(uint16_t current_crc, const uint8_t *data, size_t len);

/**
 * @brief  Compute or continue a CRC-32/IEEE 802.3 checksum.
 *
 * @details
 * This function returns the internal CRC register state. Call
 * CRC32_IEEE_FINALIZE() after the final chunk if a wire value is needed for
 * transmission, storage, or comparison against CRC32_IEEE_CHECK.
 *
 * @param  current_crc  Running internal CRC state. Use CRC32_IEEE_INIT for the
 *                      first chunk and the previous return value thereafter.
 * @param  data         Data buffer. May be NULL iff len == 0.
 * @param  len          Number of bytes to process.
 *
 * @return Updated CRC-32 internal state.
 */
uint32_t crc32_ieee(uint32_t current_crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
