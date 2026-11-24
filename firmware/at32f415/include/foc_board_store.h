/**
 * @file    foc_board_store.h
 * @brief   Board1 persistent settings: two-slot record, boot restore, UART save/erase.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Design overview
 * ---------------
 * One fixed "FBP1" record holds the M1 controller tuning and the UART RAM
 * caps, bound to the motor configuration it was saved under. Two slots use
 * the configuration store transaction: erase the inactive slot, program and
 * verify the body, then program and verify a separate commit word. Load picks
 * the newer committed, CRC-valid record by modulo sequence.
 *
 * Usage example
 * -------------
 * After foc_board_boot_select() and foc_serial_init(), and before any
 * producer starts: foc_board_persistence_restore(), then attach
 * foc_board_persistence_serial() to the UART adapter.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Foreground only. The driver serializes its own device. Saving reads the
 * live tuning, so the UART adapter calls it only while DISABLED.
 *
 * Failure / recovery
 * ------------------
 * A rejected or unreadable record leaves the boot configuration in place and
 * is reported through the UART readback. A torn save leaves the previous
 * committed record selected.
 */
#pragma once

#include "foc_board_boot.h"
#include "foc_serial.h"

#include <stddef.h>
#include <stdint.h>

/* Public Types ---------------------------------------------------------------- */

enum foc_board_store_limits {
    /** Bytes reserved per slot; the device erase unit must fit inside. */
    FOC_BOARD_STORE_SLOT_BYTES = 2048,
    /** Encoded record size and programming unit. */
    /** Format 3 record; formats 1 (108 bytes) and 2 (140 bytes) still decode. */
    FOC_BOARD_STORE_RECORD_BYTES = 236,
    FOC_BOARD_STORE_PROGRAM_BYTES = 4,
};

/** Driver over two disjoint slots of erased 0xFF bytes. Callbacks return zero on success.
 * Program writes aligned multiples of PROGRAM_BYTES and may only clear bits. */
struct foc_board_store_driver {
    void *context;
    int (*read)(void *context, unsigned int slot, size_t offset, uint8_t *bytes, size_t size);
    int (*erase)(void *context, unsigned int slot);
    int (*program)(void *context,
                   unsigned int slot,
                   size_t offset,
                   const uint8_t *bytes,
                   size_t size);
};

/** Motor configuration a record belongs to, from the effective boot configuration. */
struct foc_board_store_identity {
    uint32_t configuration;
    uint32_t pole_pairs;
    uint32_t encoder_counts_per_revolution;
    int32_t runtime_encoder_direction;
    float electrical_offset_rad;
    float phase_offset_count[3];
};

/** Everything a record restores. */
struct foc_board_settings {
    struct foc_runtime_tuning tuning;
    uint32_t rx_budget;
    float remote_current_cap_a;
    float remote_position_cap_rad;
    /** Format 4: output-position zero offset in output rad (PARAM 124); 0 before. */
    float output_zero_rad;
    /** Format 2: an applied on-MCU rotor calibration (CAL-1/CAL-2) to restore first. */
    bool calibration_present;
    struct foc_calibration_snapshot calibration;
    /** Format 3 or 4: tuning.profile and tuning.trips came from the record. Otherwise the
     * restore keeps their boot values. */
    bool extended;
    /** Format 6: velocity bandwidth (ID 108) and inertia travel (ID 109) came from the
     * record; otherwise the restore keeps their boot values. */
    bool mechanical;
};

/** Wire values of UART PARAM_READ ID 48. */
enum foc_board_store_state {
    /** This image has no persistent storage. */
    FOC_BOARD_STORE_UNAVAILABLE = 0,
    /** Storage is erased: the boot configuration is in use. */
    FOC_BOARD_STORE_EMPTY = 1,
    /** A record was loaded and applied at boot. */
    FOC_BOARD_STORE_LOADED = 2,
    /** No committed CRC-valid record; boot configuration in use. */
    FOC_BOARD_STORE_INVALID_RECORD = 3,
    /** The record belongs to a different motor configuration; not applied. */
    FOC_BOARD_STORE_IDENTITY_MISMATCH = 4,
    /** The record's values failed the Runtime checks; not applied. */
    FOC_BOARD_STORE_VALUE_REJECTED = 5,
    /** The device could not be read. */
    FOC_BOARD_STORE_IO_ERROR = 6,
};

enum foc_board_store_status {
    FOC_BOARD_STORE_OK = 0,
    FOC_BOARD_STORE_ERR_EMPTY = -1,
    FOC_BOARD_STORE_ERR_RECORD = -2,
    FOC_BOARD_STORE_ERR_IO = -3,
    FOC_BOARD_STORE_ERR_VERIFY = -4,
    FOC_BOARD_STORE_ERR_INPUT = -5,
};

/** Store binding and the boot/save outcome reported over UART. */
struct foc_board_persistence {
    struct foc_board_store_driver driver;
    struct foc_board_store_identity identity;
    enum foc_board_store_state state;
    /** Sequence of the record loaded at boot, zero when none. */
    uint32_t loaded_sequence;
    /** Sequence of the newest committed record, after boot or the last save. */
    uint32_t stored_sequence;
    bool stored;
    /** Applied calibration a save writes (the calibration manager's snapshot); may be NULL. */
    const struct foc_calibration_snapshot *calibration_source;
    /** True when boot restore re-initialized the instance with a stored calibration. */
    bool calibration_restored;
    struct foc_calibration_snapshot restored_calibration;
};

/* Public Functions ------------------------------------------------------------- */

/** Identity of the effective boot configuration. False before boot is ready. */
bool foc_board_store_identity_from_boot(const struct foc_board_boot *boot,
                                        struct foc_board_store_identity *identity);

/** Encode a record with an erased commit word. */
void foc_board_store_encode(const struct foc_board_store_identity *identity,
                            const struct foc_board_settings *settings,
                            uint32_t sequence,
                            uint8_t record[FOC_BOARD_STORE_RECORD_BYTES]);

/** Decode a committed, CRC-valid record. */
int foc_board_store_decode(const uint8_t record[FOC_BOARD_STORE_RECORD_BYTES],
                           struct foc_board_store_identity *identity,
                           struct foc_board_settings *settings,
                           uint32_t *sequence);

/** Select and decode the newest committed record. ERR_EMPTY when both slots are erased. */
int foc_board_store_load(const struct foc_board_store_driver *driver,
                         struct foc_board_store_identity *identity,
                         struct foc_board_settings *settings,
                         uint32_t *sequence,
                         unsigned int *slot);

/** Save a new record into the inactive slot with a separately committed word. */
int foc_board_store_save(const struct foc_board_store_driver *driver,
                         const struct foc_board_store_identity *identity,
                         const struct foc_board_settings *settings,
                         uint32_t *sequence);

/** Erase both slots: the next boot uses the boot configuration. */
int foc_board_store_erase(const struct foc_board_store_driver *driver);

/**
 * Boot restore, before any producer. Load the newest record; apply it only when
 * its identity equals the effective boot configuration and every value passes
 * the Runtime and UART checks. A stored calibration re-initializes the instance
 * first (foc_board_reconfigure), then the tuning is applied; on any refusal
 * nothing changes. The outcome is recorded in persistence->state.
 */
void foc_board_persistence_restore(struct foc_board_persistence *persistence,
                                   const struct foc_board_boot *boot,
                                   struct foc_runtime *runtime,
                                   struct foc_board_output *output,
                                   struct foc_board_encoder *encoder,
                                   struct foc_serial *serial);

/** UART adapter callbacks bound to this persistence instance. */
struct foc_serial_persistence foc_board_persistence_serial(
    struct foc_board_persistence *persistence);

/* ---------------------------------------- end of file ----------------------------------------- */
