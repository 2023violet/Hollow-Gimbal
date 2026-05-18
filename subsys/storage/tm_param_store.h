/**
 * @file    tm_param_store.h
 * @brief   Themis A/B persistent parameter store above tiny_param and tm_flash_device
 * @version v1.1.0
 * @date    2026.04.07
 *
 * Design overview
 * ---------------
 * `tm_param_store` is the framework-owned persistence bridge for application
 * configuration records:
 * - southbound storage comes from one `tm_flash_device`
 * - per-slot framing, versioning, and CRC stay delegated to `tiny_param`
 * - this layer adds portable A/B slot selection, sequence handling, and
 *   write-avoidance
 * - if the flash device explicitly exposes readonly mapping, this layer will
 *   let `tiny_param` use that fast path; otherwise it stays on the portable
 *   `tm_flash_read()` fallback
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns `struct tm_param_store` storage and decides which flash
 *   instance becomes the persistent owner via explicit init.
 * - This layer pins one typed `tm_flash_device` for the full lifetime of the
 *   initialized store and releases that registry reference only through
 *   `tm_param_store_deinit()`.
 * - The store does not create a second storage topology or hidden background
 *   task.
 * - Pair metadata and A/B selection stay local to this module; product code
 *   only supplies record layout and slot geometry.
 * - All public entry points are task-context only and rely on the caller to
 *   serialize concurrent access to the same store object.
 *
 * Blocking / resources / diagnostics
 * ----------------------------------
 * - This layer performs bounded flash open/read/write/erase work inline on the
 *   caller path; it introduces no heap, queue, or RTOS owner thread.
 * - `tm_param_store_pair_diag` is the formal evidence surface for active slot,
 *   write slot, current sequence, and per-slot validation status.
 * - Readonly flash mapping is opportunistic only; the portable read path
 *   remains the default fallback when mapping is unavailable.
 *
 * Failure / recovery
 * ------------------
 * - Init/load/save/check/erase failures surface through explicit negative
 *   `-errno` or `tiny_param` status returns; this module does not silently
 *   rewrite caller data or hide slot corruption.
 * - A/B selection stays deterministic even when one slot is empty or invalid,
 *   so recovery evidence remains inspectable through diagnostics.
 * - Physical flash recovery, wear policy, and board-specific storage
 *   availability remain owned by the flash/device owner below this layer.
 *
 * Record contract
 * ---------------
 * Callers define one persisted record struct per parameter group. The record
 * must begin with `struct tm_param_store_record_prefix` at offset 0 so this
 * layer can manage the monotonic A/B sequence counter.
 *
 * Usage example
 * -------------
 * struct app_record {
 *     struct tm_param_store_record_prefix prefix;
 *     uint32_t                            target_temp_millic;
 * };
 *
 * tm_param_store_init_by_name(&store, "config");
 * tm_param_store_load_pair(&store, &pair, &record, sizeof(record), &diag);
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "dev_flash.h"
#include "tiny_param.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types --------------------------------------------------------------------------------- */

struct tm_param_store_record_prefix {
    /** Monotonic sequence written by the store into the active record slot. */
    uint32_t sequence;
};

struct tm_param_store_slot_pair {
    /** Flash offset for slot A payload storage. */
    uint32_t slot_a_offset;
    /** Flash offset for slot B payload storage. */
    uint32_t slot_b_offset;
    /** Size in bytes of each slot region. */
    uint32_t slot_size;
    /** Record magic forwarded to `tiny_param` for slot validation. */
    uint32_t magic;
    /** Record version forwarded to `tiny_param` for compatibility checks. */
    uint32_t version;
};

struct tm_param_store_slot_diag {
    /** Absolute flash address of this slot. */
    uint32_t            addr;
    /** Parsed monotonic sequence when present and valid. */
    uint32_t            sequence;
    /** `true` when `sequence` came from a valid decoded record prefix. */
    bool                has_sequence;
    /** Raw `tiny_param` slot validation result for this slot. */
    struct tp_slot_info slot;
};

struct tm_param_store_pair_diag {
    /** Last pair operation status for the inspected slot pair. */
    int                             status;
    /** `true` when one active slot was selected successfully. */
    bool                            active_valid;
    /** Active slot index (`0` for A, `1` for B) when valid. */
    uint8_t                         active_slot;
    /** Next slot index selected for save/erase work. */
    uint8_t                         write_slot;
    /** Highest active sequence currently visible across both slots. */
    uint32_t                        current_sequence;
    /** Per-slot validation evidence in A/B order. */
    struct tm_param_store_slot_diag slots[2];
};

struct tm_param_store {
    /** Borrowed typed flash device that owns the physical persistence path. */
    struct tm_flash_device *flash;
    /** Cached flash geometry copied from the borrowed device during init. */
    struct tm_flash_info    flash_info;
    /** Cached flash device name copied at init so no private object field leaks upward. */
    char                    flash_name[TM_NAME_MAX];
    /** `true` after init successfully bound a flash owner. */
    bool                    initialized;
};

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief Bind one caller-owned store object to an already-openable flash device.
 *
 * @param  store  Caller-owned store object.
 * @param  flash  Borrowed typed flash device.
 *
 * @return `0` on success, or negative `-errno` / `tiny_param` status on
 *         failure.
 */
int  tm_param_store_init(struct tm_param_store *store, struct tm_flash_device *flash);

/**
 * @brief Release the flash pin owned by one initialized store and clear it.
 *
 * @param  store  Caller-owned store object.
 */
void tm_param_store_deinit(struct tm_param_store *store);

/**
 * @brief Bind one caller-owned store object by typed flash device name lookup.
 *
 * @param  store       Caller-owned store object.
 * @param  flash_name  Typed flash device name to pin for the store lifetime.
 *
 * @return `0` on success, or negative `-errno` / `tiny_param` status on
 *         failure.
 */
int  tm_param_store_init_by_name(struct tm_param_store *store, const char *flash_name);

/**
 * @brief Report whether the store has completed a valid flash binding.
 *
 * @param  store  Store object to inspect.
 *
 * @return `true` when the store is ready for pair operations.
 */
bool tm_param_store_is_ready(const struct tm_param_store *store);

/**
 * @brief Inspect one A/B slot pair without mutating caller record data.
 *
 * @param  store       Initialized store owner.
 * @param  pair        Slot geometry and record schema contract.
 * @param  record_len  Total caller record size including the prefix.
 * @param  diag        Optional output evidence surface.
 *
 * @return `0` on success, or negative `-errno` / `tiny_param` status on
 *         failure.
 */
int tm_param_store_check_pair(struct tm_param_store                 *store,
                              const struct tm_param_store_slot_pair *pair,
                              uint32_t                               record_len,
                              struct tm_param_store_pair_diag       *diag);

/**
 * @brief Save one caller record into the selected A/B slot pair.
 *
 * @param  store       Initialized store owner.
 * @param  pair        Slot geometry and record schema contract.
 * @param  record      In/out caller record starting with the required prefix.
 * @param  record_len  Total caller record size including the prefix.
 * @param  diag        Optional output evidence surface.
 *
 * @return `0` on success, or negative `-errno` / `tiny_param` status on
 *         failure.
 */
int tm_param_store_save_pair(struct tm_param_store                 *store,
                             const struct tm_param_store_slot_pair *pair,
                             void                                  *record,
                             uint32_t                               record_len,
                             struct tm_param_store_pair_diag       *diag);

/**
 * @brief Load the active record from one A/B slot pair into caller memory.
 *
 * @param  store       Initialized store owner.
 * @param  pair        Slot geometry and record schema contract.
 * @param  record      Caller-owned output buffer starting with the prefix.
 * @param  record_len  Total caller record size including the prefix.
 * @param  diag        Optional output evidence surface.
 *
 * @return `0` on success, or negative `-errno` / `tiny_param` status on
 *         failure.
 */
int tm_param_store_load_pair(struct tm_param_store                 *store,
                             const struct tm_param_store_slot_pair *pair,
                             void                                  *record,
                             uint32_t                               record_len,
                             struct tm_param_store_pair_diag       *diag);

/**
 * @brief Erase both slots in one configured A/B pair.
 *
 * @param  store  Initialized store owner.
 * @param  pair   Slot geometry and record schema contract.
 * @param  diag   Optional output evidence surface.
 *
 * @return `0` on success, or negative `-errno` / `tiny_param` status on
 *         failure.
 */
int tm_param_store_erase_pair(struct tm_param_store                 *store,
                              const struct tm_param_store_slot_pair *pair,
                              struct tm_param_store_pair_diag       *diag);

/**
 * @brief Convert one store/tiny_param status code into a stable short string.
 *
 * @param  rc  Status code returned by a `tm_param_store_*()` API.
 *
 * @return Stable short error string.
 */
const char *tm_param_store_strerror(int rc);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
