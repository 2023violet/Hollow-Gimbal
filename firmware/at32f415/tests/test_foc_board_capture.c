/**
 * @file    test_foc_board_capture.c
 * @brief   Exercise the production SRAM recorder and emit a Host fixture.
 * @version v1.0
 * @date    2026.11.19
 *
 * Test role
 * ---------
 * Reserve records exactly as the Board IRQ binding does. Write the frozen
 * wire image to a Host file when requested by the importer test.
 */

/* Includes --------------------------------------------------------------------- */
#include "foc_board_capture.h"

#include <stdio.h>

/* Private Helpers -------------------------------------------------------------- */
#define CHECK(condition)                                            \
    do {                                                            \
        if (!(condition)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            return 1;                                               \
        }                                                           \
    } while (0)

static int test_cancellation(const struct foc_capture_profile *profile)
{
    struct foc_board_capture capture = {0};
    CHECK(foc_board_capture_start(&capture, 4U, FOC_CAPTURE_HOST, 123U, profile));
    for (unsigned int i = 0U; i < FOC_CAPTURE_ADC_CAPACITY; ++i) {
        struct foc_capture_adc *row = foc_board_capture_adc(&capture, 125U);
        CHECK(row != NULL);
        row->slot = i + 7U;
    }
    for (unsigned int i = 0U; i < FOC_CAPTURE_ENCODER_CAPACITY; ++i) {
        CHECK(foc_board_capture_encoder(&capture, 125U) != NULL);
    }
    CHECK(foc_board_capture_adc(&capture, 125U) == NULL);
    CHECK(foc_board_capture_encoder(&capture, 125U) == NULL);
    CHECK(capture.image.header.adc_overflow == 1U);
    CHECK(capture.image.header.encoder_overflow == 1U);
    CHECK(capture.image.adc[0].slot == 7U);
    foc_board_capture_poll(&capture, 124U, 5U);
    CHECK(atomic_load(&capture.state) == FOC_CAPTURE_ACTIVE);
    foc_board_capture_poll(&capture, 124U, 4U);
    CHECK(atomic_load(&capture.state) == FOC_CAPTURE_SEALING);
    CHECK(foc_board_capture_adc(&capture, 125U) == NULL);
    for (unsigned int i = 0U; i < 100U; ++i) {
        foc_board_capture_poll(&capture, 0U, 0U);
    }
    CHECK(atomic_load(&capture.state) == FOC_CAPTURE_READY);
    CHECK(capture.image.header.disposition == FOC_CAPTURE_CANCELLED);
    CHECK(capture.image.header.freeze_us == 124U);
    CHECK(capture.image.header.end_us == 123U + FOC_CAPTURE_WINDOW_US);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    struct foc_board_capture recorder = {0};
    const uint32_t start = UINT32_MAX - 4000U;
    const struct foc_capture_profile profile = {
        .build_tag = 0U,
        .encoder_cpr = 65536U,
        .encoder_direction = -1,
        .pole_pairs = 7U,
        .phase_order = 0U,
    };
    CHECK(foc_board_capture_start(&recorder, 17U, FOC_CAPTURE_HOST, start, &profile));
    CHECK(foc_board_capture_adc(&recorder, start - 1U) == NULL);
    CHECK(!foc_board_capture_start(&recorder, 18U, FOC_CAPTURE_HOST, start, &profile));
    for (uint32_t i = 0U; i < 40U; ++i) {
        const uint32_t completion = start + 55U + 200U * i;
        struct foc_capture_adc *row = foc_board_capture_adc(&recorder, completion);
        CHECK(row != NULL && row->id == 17U && row->sequence == i);
        row->slot = i + 100U;
        row->token_slot = row->slot;
        row->slot_start_us = completion - 55U;
        row->generation = i + 80U;
        row->requested_us = completion - 8U;
        row->ranks12 = 1241U | (2052U << 16);
        row->ranks34 = 2048U | (2042U << 16);
        row->flags = FOC_CAPTURE_ADC_SLOT | FOC_CAPTURE_ADC_TOKEN | FOC_CAPTURE_ADC_VALID |
            FOC_CAPTURE_ADC_H0 | FOC_CAPTURE_ADC_CLEAR | FOC_CAPTURE_ADC_CONSUMED;
        row->h0_us = completion;
        row->h0_return_us = completion + 3U;
        row->safety = FOC_CAPTURE_OUTPUT_LOCKED | FOC_CAPTURE_OUTPUT_CONFIRMED;
    }
    for (uint32_t i = 0U; i < 16U; ++i) {
        const uint32_t completion = start + 70U + 500U * i;
        struct foc_capture_encoder *row = foc_board_capture_encoder(&recorder, completion);
        CHECK(row != NULL && row->sequence == i);
        row->encoder_sequence = 200U + i;
        row->cs_us = completion - 70U;
        row->rx_us = completion - 45U;
        row->h1_return_us = completion + 3U;
        row->raw_word = i + 16384U;
        row->flags = FOC_CAPTURE_ENCODER_CS | FOC_CAPTURE_ENCODER_RAW |
            FOC_CAPTURE_ENCODER_POSITION | FOC_CAPTURE_ENCODER_SPEED | FOC_CAPTURE_ENCODER_H1;
    }
    CHECK(foc_board_capture_adc(&recorder, start + FOC_CAPTURE_WINDOW_US) == NULL);
    for (unsigned int n = 0U; n < 100U; ++n) {
        foc_board_capture_poll(&recorder, start + FOC_CAPTURE_WINDOW_US, 0U);
    }
    CHECK(atomic_load(&recorder.state) == FOC_CAPTURE_READY);
    CHECK(recorder.image.header.adc_count == 40U && recorder.image.header.encoder_count == 16U);
    CHECK(recorder.image.header.crc32 != 0U);
    CHECK(foc_board_capture_adc(&recorder, start + 55U) == NULL);
    if (argc == 2) {
        FILE *file = fopen(argv[1], "wb");
        CHECK(file != NULL);
        CHECK(fwrite(&recorder.image, sizeof recorder.image, 1U, file) == 1U);
        CHECK(fclose(file) == 0);
    }
    CHECK(test_cancellation(&profile) == 0);
    puts("frozen production SRAM image: 40 ADC, 16 encoder, uint32 wrap");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
