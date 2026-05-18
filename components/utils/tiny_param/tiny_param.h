/**
 * @file    tiny_param.h
 * @brief   Portable flash-slot parameter storage with version and CRC checks
 * @version v1.3.0
 * @date    2026.04.07
 *
 * ============================================================================
 * Design philosophy: portability-first with explicit fast paths
 * ============================================================================
 *
 * tiny_param stores one caller-defined payload into one caller-defined flash
 * slot. The component intentionally stays below any BSP, RTOS, or vendor HAL:
 *
 * - flash access comes only from caller-provided hooks
 * - no heap allocation and no hidden global storage
 * - CRC verification always has a streamable fallback, so memory-mapped flash
 *   is never required
 * - backends that explicitly expose a readonly mapping hook may still get a
 *   zero-copy fast path for read-side CRC checks
 *
 * This keeps tiny_param suitable for:
 * - internal MCU flash
 * - external SPI/QSPI NOR backends
 * - host-side fake flash used by tests and migration tooling
 *
 * ============================================================================
 * Frozen slot format
 * ============================================================================
 *
 * Offset  0 : magic   (uint32_t) - slot identity tag; 0xFFFFFFFF means blank
 * Offset  4 : version (uint32_t) - payload schema version
 * Offset  8 : len     (uint32_t) - payload byte count
 * Offset 12 : crc     (uint32_t) - stored CRC-32 internal state of
 *                                  [magic + version + len + payload]
 * Offset 16 : payload (len bytes)
 *
 * The CRC field follows crc32_ieee()'s internal-state contract. If a standard
 * wire value is needed for diagnostics, finalize it with CRC32_IEEE_FINALIZE().
 *
 * ============================================================================
 * Ownership / lifecycle / context
 * ============================================================================
 *
 * - `struct tp_slot` is caller-owned immutable slot metadata.
 * - `struct tp_flash_ops` / `struct tp_flash_backend` are caller-owned backend
 *   glue borrowed for the duration of one call.
 * - Payload buffers are caller-owned.
 * - All APIs are task-context storage operations; do not call erase/write from
 *   ISR context.
 * - `tp_load_raw()` is the intended migration path after `tp_load()` returns
 *   `-EPROTO`.
 * - read-only operations (`tp_load`, `tp_load_raw`, `tp_check`) require only
 *   the backend read hook; save/erase operations require write and/or erase as
 *   documented per function below.
 * - `map_readonly()` is an optional optimization contract. If absent, or if it
 *   returns NULL for a given range, tiny_param falls back to `read()`.
 * - @note Not thread-safe. Caller must ensure external serialization around a
 *   shared slot/backend pair. Not ISR-safe.
 *
 * ============================================================================
 * Slot encoding contract
 * ============================================================================
 *
 * The frozen slot header is serialized little-endian byte-by-byte, independent
 * of host CPU endianness. Payload length may be any non-zero value up to
 * `slot->max_len`; backend write granularity remains a backend responsibility,
 * not a payload protocol rule. A slot image with zero length or oversized
 * length is treated as protocol-invalid (`-EPROTO`).
 *
 * ============================================================================
 * Usage example
 * -------------
 *
 * @code
 * static const struct tp_slot g_slot = {
 *     .addr    = 0x0800F800U,
 *     .max_len = 64U,
 *     .magic   = 0x50415241U,
 *     .version = 2U,
 * };
 *
 * struct app_cfg cfg;
 *
 * if (tp_load(&g_slot, &g_flash_ops, &cfg, sizeof(cfg)) == TP_EMPTY) {
 *     app_cfg_defaults(&cfg);
 *     (void)tp_save(&g_slot, &g_flash_ops, &cfg, sizeof(cfg));
 * }
 * @endcode
 *
 * Failure / recovery
 * ------------------
 * - Blank storage returns `TP_EMPTY`, which is an explicit non-success status.
 * - Invalid contracts fail with `-EINVAL`.
 * - Format/version mismatches fail with `-EPROTO`.
 * - CRC mismatches fail with `-EILSEQ`.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Constants ----------------------------------------------------------------------------- */

/**
 * @brief  Returned when the slot has never been written (all-0xFF).
 *
 * Blank-slot discovery is explicit and non-successful. Callers should apply
 * hard-coded defaults and call tp_save() to initialize the slot for the first
 * time.
 */
#define TP_EMPTY       (-ENOENT)

/**
 * @brief  Size of the serialized header tiny_param prepends to every slot.
 *
 * Payload capacity of a sector = sector_size - TP_HEADER_SIZE.
 * Callers should assert: sizeof(my_payload) <= sector_size - TP_HEADER_SIZE.
 */
#define TP_HEADER_SIZE (16U)

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief  Slot descriptor for one named flash storage block.
 *
 * Declare one const instance per parameter group and pass it to every tp_*
 * call. Each field is contract-bearing and maps directly to on-flash layout.
 */
struct tp_slot {
    /** Base flash address of this slot, in bytes. */
    uint32_t addr;
    /** Maximum payload capacity in bytes, excluding TP_HEADER_SIZE. */
    uint32_t max_len;
    /** Slot identity tag written into the header magic field. */
    uint32_t magic;
    /** Payload schema version. Increment when the payload layout changes. */
    uint32_t version;
};

/**
 * @brief  Flash hardware abstraction used by tiny_param.
 *
 * Return convention:
 *   >= 0  success
 *   <  0  backend failure
 *
 * The exact positive value is ignored by tiny_param. Backends may return the
 * transferred byte count or simply return 0 on success.
 *
 * Erase contract:
 * - tiny_param asks @c erase() to cover at least the bytes it is about to
 *   rewrite.
 * - Backends whose hardware erases in larger sectors/pages must round or
 *   expand that request internally. tiny_param does not perform erase-
 *   granularity alignment on behalf of the backend.
 *
 * Readonly mapping contract:
 * - `map_readonly()` is optional.
 * - If it returns a non-NULL pointer for `[addr, addr + size)`, tiny_param may
 *   read that storage window directly during `tp_load`, `tp_load_raw`, and
 *   `tp_check`.
 * - Returning NULL means "mapping unavailable for this request"; it is not an
 *   error, and tiny_param will fall back to `read()`.
 * - The mapped bytes must stay stable for the duration of the call.
 */
struct tp_flash_ops {
    /** Read @p size bytes from flash address @p addr into @p buf. */
    int (*read)(uint32_t addr, uint8_t *buf, uint32_t size);
    /** Write @p size bytes from @p buf to flash address @p addr. */
    int (*write)(uint32_t addr, const uint8_t *buf, uint32_t size);
    /** Erase at least @p size bytes starting at flash address @p addr. */
    int (*erase)(uint32_t addr, uint32_t size);
    /** Optional read-only mapping hook for the `[addr, addr + size)` window. */
    const void *(*map_readonly)(uint32_t addr, uint32_t size);
};

/**
 * @brief  Context-aware flash backend hooks for framework-level adapters.
 *
 * This additive contract keeps tiny_param portable while allowing higher
 * layers such as device-model bridges to carry one borrowed user context
 * instead of relying on global state.
 */
struct tp_flash_backend_ops {
    /** Read @p size bytes from flash address @p addr into @p buf. */
    int (*read)(void *user_ctx, uint32_t addr, uint8_t *buf, uint32_t size);
    /** Write @p size bytes from @p buf to flash address @p addr. */
    int (*write)(void *user_ctx, uint32_t addr, const uint8_t *buf, uint32_t size);
    /** Erase at least @p size bytes starting at flash address @p addr. */
    int (*erase)(void *user_ctx, uint32_t addr, uint32_t size);
    /** Optional read-only mapping hook for the `[addr, addr + size)` window. */
    const void *(*map_readonly)(void *user_ctx, uint32_t addr, uint32_t size);
};

/**
 * @brief  One borrowed context-aware flash backend instance.
 *
 * The backend object itself is caller-owned and only borrowed for the duration
 * of one tp_* call.
 */
struct tp_flash_backend {
    /** Backend hook table. */
    const struct tp_flash_backend_ops *ops;
    /** Borrowed caller context forwarded to every hook invocation. */
    void                              *user_ctx;
};

/**
 * @brief  Diagnostic snapshot returned by tp_check().
 */
struct tp_slot_info {
    /** Slot address reported by tp_check(). */
    uint32_t addr;
    /** Stored magic value from the header. */
    uint32_t magic;
    /** Stored schema version from the header. */
    uint32_t version;
    /** Stored payload length from the header. */
    uint32_t data_len;
    /** Stored CRC field from the header. */
    uint32_t stored_crc;
    /** Diagnostic status returned by the checker. */
    int      status;
};

/* Public Function Declarations ----------------------------------------------------------------- */

/**
 * @brief  Save a payload to a flash slot.
 *
 * Sequence: validate, build header, erase, write header, write payload.
 *
 * @note   Flash erase/write is task-context work. Do not call from ISR context.
 * @param  slot         Slot descriptor.
 * @param  ops          Flash backend hooks.
 * @param  payload      Data to save.
 * @param  payload_len  Byte count (> 0, <= slot->max_len).
 *
 * @return 0 on success, negative errno on failure.
 */
int tp_save(const struct tp_slot      *slot,
            const struct tp_flash_ops *ops,
            const void                *payload,
            uint32_t                   payload_len);

/**
 * @brief  Save a payload through a context-aware flash backend.
 *
 * This is the preferred entry for framework adapters that already own a flash
 * device handle or other backend context.
 *
 * @param  slot         Slot descriptor.
 * @param  backend      Borrowed backend instance with ops and user_ctx.
 * @param  payload      Data to save.
 * @param  payload_len  Byte count (> 0, <= slot->max_len).
 *
 * @return 0 on success, negative errno on failure.
 */
int tp_save_backend(const struct tp_slot          *slot,
                    const struct tp_flash_backend *backend,
                    const void                    *payload,
                    uint32_t                       payload_len);

/**
 * @brief  Load a payload from a flash slot with strict version and CRC checks.
 *
 * tiny_param first tries the optional mapped-read hook. If unavailable, it
 * falls back to `read()`.
 *
 * @param  slot         Slot descriptor.
 * @param  ops          Flash backend hooks. Only the read hook is required.
 * @param  payload      Caller buffer.
 * @param  payload_len  Expected payload size. Must match the stored header.
 *
 * @retval 0         Load succeeded.
 * @retval TP_EMPTY  The slot is blank.
 * @retval <0        Validation, transport, or payload integrity failed.
 */
int tp_load(const struct tp_slot      *slot,
            const struct tp_flash_ops *ops,
            void                      *payload,
            uint32_t                   payload_len);

/**
 * @brief  Load a payload through a context-aware flash backend.
 *
 * @param  slot         Slot descriptor.
 * @param  backend      Borrowed backend instance with ops and user_ctx.
 * @param  payload      Caller buffer.
 * @param  payload_len  Expected payload size. Must match the stored header.
 *
 * @retval 0         Load succeeded.
 * @retval TP_EMPTY  The slot is blank.
 * @retval <0        Validation, transport, or payload integrity failed.
 */
int tp_load_backend(const struct tp_slot          *slot,
                    const struct tp_flash_backend *backend,
                    void                          *payload,
                    uint32_t                       payload_len);

/**
 * @brief  Load a payload without enforcing version or size matches.
 *
 * Intended for migration code after tp_load() returns -EPROTO. The function
 * still validates blank state, magic, and CRC using the stored header fields.
 *
 * Reads min(stored_len, buf_len) bytes and zero-fills any remaining caller
 * buffer space when buf_len exceeds the stored payload size.
 *
 * tiny_param first tries the optional mapped-read hook. If unavailable, it
 * falls back to `read()` plus streamed CRC verification.
 *
 * @param  slot     Slot descriptor. Only slot->magic is used for validation.
 * @param  ops      Flash backend hooks. Only the read hook is required.
 * @param  payload  Caller buffer.
 * @param  buf_len  Size of the caller buffer.
 *
 * @retval 0         Load succeeded.
 * @retval TP_EMPTY  The slot is blank.
 * @retval <0        Validation, transport, or payload integrity failed.
 */
int tp_load_raw(const struct tp_slot      *slot,
                const struct tp_flash_ops *ops,
                void                      *payload,
                uint32_t                   buf_len);

/**
 * @brief  Load a payload through a context-aware backend without size/version enforcement.
 *
 * @param  slot     Slot descriptor. Only slot->magic is used for validation.
 * @param  backend  Borrowed backend instance with ops and user_ctx.
 * @param  payload  Caller buffer.
 * @param  buf_len  Size of the caller buffer.
 *
 * @retval 0         Load succeeded.
 * @retval TP_EMPTY  The slot is blank.
 * @retval <0        Validation, transport, or payload integrity failed.
 */
int tp_load_raw_backend(const struct tp_slot          *slot,
                        const struct tp_flash_backend *backend,
                        void                          *payload,
                        uint32_t                       buf_len);

/**
 * @brief  Check a slot header and CRC without modifying caller-owned payload state.
 *
 * tiny_param first tries the optional mapped-read hook. If unavailable, it
 * falls back to streamed CRC verification through `read()`, so memory-mapped
 * flash is never a requirement.
 *
 * @param  slot  Slot descriptor.
 * @param  ops   Flash backend hooks. Only the read hook is required.
 * @param  info  Optional output snapshot. Pass NULL if only the status matters.
 *
 * @return 0 on success, TP_EMPTY if blank, otherwise negative errno.
 */
int tp_check(const struct tp_slot *slot, const struct tp_flash_ops *ops, struct tp_slot_info *info);

/**
 * @brief  Check a slot through a context-aware backend.
 *
 * @param  slot    Slot descriptor.
 * @param  backend Borrowed backend instance with ops and user_ctx.
 * @param  info    Optional output snapshot. Pass NULL if only the status matters.
 *
 * @return 0 on success, TP_EMPTY if blank, otherwise negative errno.
 */
int tp_check_backend(const struct tp_slot          *slot,
                     const struct tp_flash_backend *backend,
                     struct tp_slot_info           *info);

/**
 * @brief  Erase the flash region occupied by a slot.
 *
 * After this call tp_check() should return TP_EMPTY for the slot.
 *
 * @param  slot  Slot descriptor.
 * @param  ops   Flash backend hooks. Only the erase hook is required.
 *
 * @return 0 on success, negative errno on failure.
 */
int tp_erase(const struct tp_slot *slot, const struct tp_flash_ops *ops);

/**
 * @brief  Erase a slot through a context-aware backend.
 *
 * @param  slot     Slot descriptor.
 * @param  backend  Borrowed backend instance with ops and user_ctx.
 *
 * @return 0 on success, negative errno on failure.
 */
int tp_erase_backend(const struct tp_slot *slot, const struct tp_flash_backend *backend);

/**
 * @brief  Compute the CRC-32 state that tp_save() would store for a payload.
 *
 * The return value uses the same internal-state convention as crc32_ieee() and
 * matches the value written into the slot header.
 *
 * Save-only write-alignment is not enforced here. This helper is intended for
 * write-avoidance and diagnostics, not for validating whether a payload may be
 * written by the current backend.
 *
 * @param  slot         Slot descriptor.
 * @param  payload      Pending payload data.
 * @param  payload_len  Byte count.
 *
 * @return Stored CRC-32 internal state. Returns 0 if arguments are invalid,
 *         the payload length is zero, or the payload length exceeds the slot
 *         capacity.
 */
uint32_t tp_calc_crc(const struct tp_slot *slot, const void *payload, uint32_t payload_len);

/**
 * @brief  Translate a tiny_param return code to a human-readable string.
 *
 * @param  rc  Return value from any tp_* function.
 * @return Static string literal, never NULL.
 */
const char *tp_strerr(int rc);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
