/**
 * @file    foc_board_capture.c
 * @brief   One-shot SRAM reservation and foreground integrity publication.
 * @version v1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * Keep producer paths constant-size. The consumer exports a sealed wire image.
 */

/* Includes --------------------------------------------------------------------- */
#include "foc_board_capture.h"
#include "foc_board.h"

#include <stddef.h>

_Static_assert(sizeof(struct foc_capture_header) == 128U, "wire header");
_Static_assert(sizeof(struct foc_capture_adc) == 100U, "wire ADC record");
_Static_assert(sizeof(struct foc_capture_encoder) == 64U, "wire encoder record");
_Static_assert(sizeof(struct foc_capture_image) == 5480U, "wire image");
_Static_assert(offsetof(struct foc_board_capture, state) == 5480U, "SWD state address");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "nonblocking acquisition state");
#if defined(__BYTE_ORDER__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "wire byte order");
#endif

/* Private Helpers -------------------------------------------------------------- */
static bool active_at(const struct foc_board_capture *capture, uint32_t now_us)
{
    return atomic_load_explicit(&capture->state, memory_order_acquire) == FOC_CAPTURE_ACTIVE &&
        now_us - capture->image.header.start_us < FOC_CAPTURE_WINDOW_US;
}

static void increment_overflow(uint32_t *count)
{
    if (*count != UINT32_MAX) {
        ++*count;
    }
}

/* Public Functions ------------------------------------------------------------- */
bool foc_board_capture_start(
    struct foc_board_capture *capture,
    uint32_t id,
    uint32_t producer,
    uint32_t start_us,
    const struct foc_capture_profile *profile
)
{
    if (id == 0U || (producer != FOC_CAPTURE_BOARD && producer != FOC_CAPTURE_HOST) ||
        profile == NULL ||
        (profile->configuration != FOC_CAPTURE_CONFIGURATION_NOMINAL &&
         profile->configuration != FOC_CAPTURE_CONFIGURATION_CALIBRATED) ||
        atomic_load_explicit(&capture->state, memory_order_relaxed) != FOC_CAPTURE_IDLE) {
        return false;
    }
    capture->image.header = (struct foc_capture_header){
        .magic = UINT32_C(0x31434246),
        .version = 1U,
        .header_bytes = sizeof capture->image.header,
        .total_bytes = sizeof capture->image,
        .id = id,
        .producer = producer,
        .build_tag = profile->build_tag,
        .clock = FOC_CAPTURE_CLOCK,
        .start_us = start_us,
        .end_us = start_us + FOC_CAPTURE_WINDOW_US,
        .adc_period_us = FOC_BOARD_H0_US,
        .encoder_period_us = FOC_BOARD_H1_US,
        .ranks = FOC_CAPTURE_RANKS,
        .logical_channels = FOC_CAPTURE_LOGICAL_CHANNELS,
        .encoder_cpr = profile->encoder_cpr,
        .encoder_direction = profile->encoder_direction,
        .pole_pairs = profile->pole_pairs,
        .phase_order = profile->phase_order,
        .adc_record_bytes = sizeof(struct foc_capture_adc),
        .encoder_record_bytes = sizeof(struct foc_capture_encoder),
        .adc_capacity = FOC_CAPTURE_ADC_CAPACITY,
        .encoder_capacity = FOC_CAPTURE_ENCODER_CAPACITY,
        .board_profile = 1U,
        .encoder_profile = profile->configuration,
        .clock_hz = FOC_BOARD_US_HZ,
    };
    capture->crc = UINT32_MAX;
    atomic_store_explicit(&capture->state, FOC_CAPTURE_ACTIVE, memory_order_release);
    return true;
}

struct foc_capture_adc *foc_board_capture_adc(struct foc_board_capture *capture, uint32_t done_us)
{
    if (!active_at(capture, done_us)) {
        return NULL;
    }
    struct foc_capture_header *header = &capture->image.header;
    if (header->adc_count == FOC_CAPTURE_ADC_CAPACITY) {
        increment_overflow(&header->adc_overflow);
        return NULL;
    }
    const uint32_t index = header->adc_count++;
    struct foc_capture_adc *row = &capture->image.adc[index];
    row->id = header->id;
    row->clock = header->clock;
    row->sequence = index;
    row->completion_us = done_us;
    return row;
}

struct foc_capture_encoder *
foc_board_capture_encoder(struct foc_board_capture *capture, uint32_t done_us)
{
    if (!active_at(capture, done_us)) {
        return NULL;
    }
    struct foc_capture_header *header = &capture->image.header;
    if (header->encoder_count == FOC_CAPTURE_ENCODER_CAPACITY) {
        increment_overflow(&header->encoder_overflow);
        return NULL;
    }
    const uint32_t index = header->encoder_count++;
    struct foc_capture_encoder *row = &capture->image.encoder[index];
    row->id = header->id;
    row->clock = header->clock;
    row->sequence = index;
    row->close_us = done_us;
    return row;
}

void foc_board_capture_poll(struct foc_board_capture *capture, uint32_t now_us, uint32_t cancel_id)
{
    const unsigned int state = atomic_load_explicit(&capture->state, memory_order_relaxed);
    struct foc_capture_header *header = &capture->image.header;
    if (state == FOC_CAPTURE_ACTIVE) {
        const uint32_t elapsed = now_us - header->start_us;
        const bool elapsed_ordered = elapsed < UINT32_C(0x80000000);
        const bool ended = elapsed_ordered && elapsed >= FOC_CAPTURE_WINDOW_US;
        const bool cancelled = cancel_id == header->id && !ended;
        if (!ended && !cancelled) {
            return;
        }
        atomic_store_explicit(&capture->state, FOC_CAPTURE_SEALING, memory_order_release);
        header->disposition = cancelled ? FOC_CAPTURE_CANCELLED : FOC_CAPTURE_COMPLETE;
        header->freeze_us = now_us;
    } else if (state != FOC_CAPTURE_SEALING) {
        return;
    }

    const unsigned char *bytes = (const unsigned char *)&capture->image;
    /* IEEE CRC32 over the fixed image with the checksum field still zero.
     * At most 64 bytes per foreground pass. */
    for (unsigned int n = 0U; n < 64U && capture->cursor < sizeof capture->image; ++n) {
        capture->crc ^= bytes[capture->cursor++];
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            const uint32_t taps = (capture->crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U;
            capture->crc = (capture->crc >> 1) ^ taps;
        }
    }
    if (capture->cursor == sizeof capture->image) {
        header->crc32 = capture->crc ^ UINT32_MAX;
        atomic_store_explicit(&capture->state, FOC_CAPTURE_READY, memory_order_release);
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
