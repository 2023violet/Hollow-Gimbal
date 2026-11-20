/**
 * @file    foc_board_capture.h
 * @brief   Locked Board1 one-shot SRAM capture image and ownership contract.
 * @version v1.0
 * @date    2026.11.19
 *
 * Design overview
 * ---------------
 * Two serialized producer lanes reserve in-place records. Foreground seals
 * the immutable little-endian image in bounded increments.
 *
 * Usage example
 * -------------
 * Zero-initialize a recorder, start it once, then poll until READY.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * H0 owns ADC records, H1 owns Encoder records, foreground owns lifecycle.
 * A reserved row is filled in the same ISR. SWD exports only after READY.
 *
 * Failure / recovery
 * ------------------
 * Full lanes count overflow and preserve earlier rows. A sealed instance is
 * immutable for the boot. A cancelled window is diagnostic evidence only.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Public Types ---------------------------------------------------------------- */
enum {
    FOC_CAPTURE_WINDOW_US = 8000U,
    FOC_CAPTURE_ADC_CAPACITY = 42U,
    FOC_CAPTURE_ENCODER_CAPACITY = 18U,
    FOC_CAPTURE_BOARD = 1U,
    FOC_CAPTURE_HOST = 2U,
    FOC_CAPTURE_CLOCK = 1U,
    FOC_CAPTURE_COMPLETE = 1U,
    FOC_CAPTURE_CANCELLED = 2U,
    FOC_CAPTURE_RANKS = 0x06050401U,
    FOC_CAPTURE_LOGICAL_CHANNELS = 0x00060504U,
};

enum foc_capture_state {
    FOC_CAPTURE_IDLE,
    FOC_CAPTURE_ACTIVE,
    FOC_CAPTURE_SEALING,
    FOC_CAPTURE_READY,
};

enum foc_capture_adc_flags {
    FOC_CAPTURE_ADC_SLOT = 1U,
    FOC_CAPTURE_ADC_TOKEN = 2U,
    FOC_CAPTURE_ADC_VALID = 4U,
    FOC_CAPTURE_ADC_H0 = 8U,
    FOC_CAPTURE_ADC_SENSOR = 16U,
    FOC_CAPTURE_ADC_COMMAND = 32U,
    FOC_CAPTURE_ADC_DWT = 64U,
    FOC_CAPTURE_ADC_CLEAR = 128U,
    FOC_CAPTURE_ADC_CONSUMED = 256U,
};

enum foc_capture_encoder_flags {
    FOC_CAPTURE_ENCODER_CS = 1U,
    FOC_CAPTURE_ENCODER_RAW = 2U,
    FOC_CAPTURE_ENCODER_POSITION = 4U,
    FOC_CAPTURE_ENCODER_SPEED = 8U,
    FOC_CAPTURE_ENCODER_H1 = 16U,
    FOC_CAPTURE_ENCODER_CONTINUITY_LOST = 32U,
    FOC_CAPTURE_ENCODER_DWT = 64U,
};

/** Safety packs action in bits 0–3, Safety state in 4–7 and observations above. */
enum {
    FOC_CAPTURE_OUTPUT_LOCKED = 1U << 8,
    FOC_CAPTURE_OUTPUT_CONFIRMED = 1U << 9,
    FOC_CAPTURE_STOP_PENDING = 1U << 10,
};

struct foc_capture_profile {
    uint32_t build_tag;
    uint32_t encoder_cpr;
    int32_t encoder_direction;
    uint32_t pole_pairs;
    uint32_t phase_order;
};

struct foc_capture_header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t total_bytes;
    uint32_t disposition;
    uint32_t id;
    uint32_t producer;
    uint32_t build_tag;
    uint32_t clock;
    uint32_t start_us;
    uint32_t end_us;
    uint32_t adc_count;
    uint32_t encoder_count;
    uint32_t adc_overflow;
    uint32_t encoder_overflow;
    uint32_t adc_period_us;
    uint32_t encoder_period_us;
    uint32_t ranks;
    uint32_t logical_channels;
    uint32_t encoder_cpr;
    int32_t encoder_direction;
    uint32_t pole_pairs;
    uint32_t phase_order;
    uint32_t adc_record_bytes;
    uint32_t encoder_record_bytes;
    uint32_t adc_capacity;
    uint32_t encoder_capacity;
    uint32_t board_profile;
    uint32_t encoder_profile;
    uint32_t clock_hz;
    uint32_t freeze_us;
    uint32_t crc32;
};

struct foc_capture_adc {
    uint32_t id;
    uint32_t clock;
    uint32_t sequence;
    uint32_t completion_us;
    uint32_t slot;
    uint32_t generation;
    uint32_t requested_us;
    uint32_t ranks12;
    uint32_t ranks34;
    uint32_t flags;
    uint32_t rejection;
    uint32_t missing_slots;
    uint32_t late_count;
    uint32_t sensor_sequence;
    uint32_t sensor_us;
    uint32_t h0_us;
    uint32_t session;
    uint32_t command_sequence;
    uint32_t faults;
    uint32_t safety;
    int32_t h0_result;
    uint32_t h0_cycles;
    uint32_t slot_start_us;
    uint32_t token_slot;
    uint32_t h0_return_us;
};

struct foc_capture_encoder {
    uint32_t id;
    uint32_t clock;
    uint32_t sequence;
    uint32_t encoder_sequence;
    uint32_t cs_us;
    uint32_t rx_us;
    uint32_t close_us;
    uint32_t h1_return_us;
    uint32_t raw_word;
    uint32_t flags;
    uint32_t transport;
    uint32_t result;
    uint32_t losses;
    int32_t h1_status;
    uint32_t spi_status;
    uint32_t h1_cycles;
};

struct foc_capture_image {
    struct foc_capture_header header;
    struct foc_capture_adc adc[FOC_CAPTURE_ADC_CAPACITY];
    struct foc_capture_encoder encoder[FOC_CAPTURE_ENCODER_CAPACITY];
};

struct foc_board_capture {
    struct foc_capture_image image;
    atomic_uint state;
    uint32_t crc;
    uint32_t cursor;
};

/* Public Functions ------------------------------------------------------------- */
#if defined(FALCONFOC_BOARD_CAPTURE)
/** Immutable acquisition facts of the default locked Board1 binding. */
struct foc_capture_profile foc_board_capture_default_profile(void);
#endif

bool foc_board_capture_start(struct foc_board_capture *capture,
                             uint32_t id,
                             uint32_t producer,
                             uint32_t start_us,
                             const struct foc_capture_profile *profile);

/** Reserve and initialize the identity of a row. NULL means skip or overflow. */
struct foc_capture_adc *foc_board_capture_adc(struct foc_board_capture *capture, uint32_t done_us);
struct foc_capture_encoder *foc_board_capture_encoder(struct foc_board_capture *capture,
                                                      uint32_t done_us);

/** Foreground. Matching cancel_id freezes an incomplete capture. */
void foc_board_capture_poll(struct foc_board_capture *capture, uint32_t now_us, uint32_t cancel_id);

/* ---------------------------------------- end of file ----------------------------------------- */
