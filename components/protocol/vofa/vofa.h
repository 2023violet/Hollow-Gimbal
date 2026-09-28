/**
 * @file    vofa.h
 * @brief   Reusable VOFA+ waveform framing component with selectable protocol
 * @version v2.0.0
 * @date    2026.03.28
 *
 * Design overview
 * ---------------
 * `vofa` reads caller-bound variables, converts them into protocol-specific
 * binary or textual channel samples, and emits one VOFA+ frame through a
 * caller-registered transport callback. The actual UART/DMA transport remains
 * outside this component.
 *
 * Usage example
 * -------------
 * vofa_init();
 * vofa_register_send_callback(uart_send_bytes);
 * vofa_bind(0U, &adc_value, VOFA_UINT32);
 * vofa_process_frame();
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - Bound variable pointers are borrowed and must remain valid until
 *   `vofa_unbind(idx)` or `vofa_init()` resets the binding table.
 * - Stack variables and temporary objects must not be bound.
 * - Protocol choice is compile-time via `VOFA_PROTOCOL`.
 * - This component owns one process-wide singleton binding table, so it is not
 *   thread-safe and callers must provide external serialization.
 * - `VOFA_PROTO_JUSTFLOAT` emits the host's IEEE-754 little-endian byte order
 *   exactly as stored in memory; use FireWater when cross-endian text transport
 *   is required.
 *
 * Blocking / allocation
 * ---------------------
 * - No allocation. Framing is performed into the component's internal TX
 *   buffers and emitted synchronously through the registered callback.
 *
 * Failure / recovery
 * ------------------
 * - `vofa_init()` restores the binding table and installs the no-op callback.
 * - Unbound channels contribute zero-valued samples.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/** Binary IEEE-754 float array followed by the JustFloat tail marker. */
#define VOFA_PROTO_JUSTFLOAT (0U)
/** ASCII FireWater line: `$val0,val1,...,valN;\r\n`. */
#define VOFA_PROTO_FIREWATER (1U)

/**
 * @brief  Active compile-time protocol selection.
 *
 * Default is JustFloat: the waveform lane runs on the shared console UART at
 * reduced frame rate, and JustFloat frames are what the host-side debug
 * scripts parse. FireWater remains selectable for text-coexistent lanes.
 */
#ifndef VOFA_PROTOCOL
#define VOFA_PROTOCOL VOFA_PROTO_JUSTFLOAT
#endif

/** Maximum number of simultaneously bound channels. */
#define VOFA_MAX_CHANNELS (10U)

/**
 * @brief  FireWater frame buffer size in bytes.
 *
 * This buffer is sized for the current `VOFA_MAX_CHANNELS == 10` configuration
 * plus separators, prefix, suffix, and a small safety margin.
 */
#define VOFA_FW_BUF_SIZE  (256U)

/* Public Types --------------------------------------------------------------------------------- */

/** Source data types accepted by `vofa_bind()`. */
enum vofa_data_type {
    VOFA_FLOAT = 0,
    VOFA_INT8,
    VOFA_UINT8,
    VOFA_INT16,
    VOFA_UINT16,
    VOFA_INT32,
    VOFA_UINT32,
};

/**
 * @brief  Transport send callback registered by the caller.
 *
 * @param[in] data  Pointer to the frame buffer.
 * @param[in] len   Frame length in bytes.
 */
typedef void (*vofa_send_callback_t)(uint8_t *data, uint16_t len);

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief Reset the binding table and install the no-op send callback.
 *
 * @warning This function is NOT thread-safe. Must be called during system
 *          initialization before any concurrent access to VOFA APIs.
 */
void vofa_init(void);

/**
 * @brief Bind one caller-owned variable to a VOFA channel slot.
 *
 * The filter records the pointer and type. The pointed-to variable is read
 * indirectly when `vofa_process_frame()` is called.
 *
 * @warning This function is NOT thread-safe.
 * @warning The caller must ensure `ptr` remains valid and properly aligned
 *          until `vofa_unbind()` is called or the module is re-initialized.
 *
 * @param[in] idx   Channel index in `[0, VOFA_MAX_CHANNELS - 1]`.
 * @param[in] ptr   Borrowed pointer to the source variable.
 * @param[in] type  Source variable type describing size and format.
 */
void vofa_bind(uint8_t idx, void *ptr, enum vofa_data_type type);

/**
 * @brief Unbind one channel slot and refresh the active-channel count.
 *
 * @warning This function is NOT thread-safe.
 *
 * @param[in] idx Channel index to unbind.
 */
void vofa_unbind(uint8_t idx);

/**
 * @brief Pack one frame from all active channels and send it through the
 *        registered callback.
 *
 * @warning This function is NOT thread-safe.
 * @note The blocking behavior depends entirely on the registered send callback.
 *       If the callback blocks (e.g., waits on a UART semaphore), this function
 *       will block.
 *
 * @note Do not call this function from an ISR context if the send callback
 *       might block.
 */
void vofa_process_frame(void);

/**
 * @brief Register the caller-owned transport send callback.
 *
 * @warning This function is NOT thread-safe. Call it during initialization.
 *
 * @param[in] callback Transport send function. The payload pointer and length
 *                   provided to this callback are valid only for the duration
 *                   of the callback invocation.
 */
void vofa_register_send_callback(vofa_send_callback_t callback);

/**
 * @brief Get the number of currently active bound channels.
 *
 * @return Active channel count in `[0, VOFA_MAX_CHANNELS]`.
 */
uint8_t vofa_get_active_channel_count(void);

/**
 * @brief Get the human-readable name of the compile-time selected protocol.
 *
 * @return Static string literal (e.g., "JustFloat", "FireWater").
 */
const char *vofa_get_protocol_name(void);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
