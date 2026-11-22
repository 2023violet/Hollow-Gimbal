/**
 * @file    foc_board_capture_boot_fixture.c
 * @brief   Host fixture: a formally selected Board1 boot written by the production SRAM recorder.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Test role
 * ---------
 * Select the startup configuration with the production foc_board_boot_select()
 * (no supply for "nominal", the synthetic verification supply for
 * "calibrated"), derive the Capture Profile from the effective Runtime/Encoder
 * through foc_board_boot_capture_profile(), and seal a complete image with the
 * production writer. Observation rows are synthetic Host data, producer HOST.
 *
 * Usage: foc-board-capture-boot-fixture <nominal|calibrated> <image.bin> [capture-id]
 *            [--adc A,B,C] [--noise counts] [--producer host|board]
 *
 * --adc sets the logical A/B/C (rank 2/3/4) means: each phase uses floor(x) plus
 * one count in the first round(frac(x) * 40) rows, so the 40-row mean is exactly
 * floor(x) + k/40. --noise adds +n/-n on alternating rows. --producer board marks
 * these synthetic bytes as BOARD for provenance-branch tests only. Without options
 * the rows are unchanged from NEXT-FW-INTEG-01R (2032/2063/2018).
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private Variables ------------------------------------------------------------ */

static struct foc_runtime runtime;
static struct foc_board_output output;
static struct foc_board_encoder encoder;
static struct foc_board_boot boot;
static struct foc_board_capture recorder;
static double adc_target[3] = {2032.0, 2063.0, 2018.0};
static uint32_t adc_noise;

/* Private Helpers -------------------------------------------------------------- */

static int fail(const char *message)
{
    fprintf(stderr, "fixture: %s\n", message);
    return 1;
}

/* Deterministic rank for row i: floor(target) + 1 in the first round(frac * 40) rows. */
static uint32_t adc_rank(unsigned int phase, uint32_t i)
{
    const double base = (double)(uint32_t)adc_target[phase];
    const uint32_t high = (uint32_t)((adc_target[phase] - base) * 40.0 + 0.5);
    const uint32_t value = (uint32_t)base + (i < high ? 1U : 0U);
    return (i & 1U) != 0U ? value + adc_noise : value - adc_noise;
}

static void fill_rows(uint32_t start)
{
    for (uint32_t i = 0U; i < 40U; ++i) {
        const uint32_t completion = start + 55U + 200U * i;
        struct foc_capture_adc *row = foc_board_capture_adc(&recorder, completion);
        row->slot = i + 100U;
        row->token_slot = row->slot;
        row->slot_start_us = completion - 55U;
        row->generation = i + 80U;
        row->requested_us = completion - 8U;
        /* Rank 1 VBUS, rank 2 driver IC = logical A, rank 3 IB = B, rank 4 IA = C. */
        row->ranks12 = 1241U | (adc_rank(0U, i) << 16);
        row->ranks34 = adc_rank(1U, i) | (adc_rank(2U, i) << 16);
        row->flags = FOC_CAPTURE_ADC_SLOT | FOC_CAPTURE_ADC_TOKEN | FOC_CAPTURE_ADC_VALID |
            FOC_CAPTURE_ADC_H0 | FOC_CAPTURE_ADC_CLEAR | FOC_CAPTURE_ADC_CONSUMED;
        row->h0_us = completion;
        row->h0_return_us = completion + 3U;
        row->safety = FOC_CAPTURE_OUTPUT_LOCKED | FOC_CAPTURE_OUTPUT_CONFIRMED;
    }
    for (uint32_t i = 0U; i < 16U; ++i) {
        const uint32_t completion = start + 70U + 500U * i;
        struct foc_capture_encoder *row = foc_board_capture_encoder(&recorder, completion);
        row->encoder_sequence = 200U + i;
        row->cs_us = completion - 70U;
        row->rx_us = completion - 45U;
        row->h1_return_us = completion + 3U;
        row->raw_word = i + 16384U;
        row->flags = FOC_CAPTURE_ENCODER_CS | FOC_CAPTURE_ENCODER_RAW |
            FOC_CAPTURE_ENCODER_POSITION | FOC_CAPTURE_ENCODER_SPEED | FOC_CAPTURE_ENCODER_H1;
    }
}

/* Test Entry ------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        return fail("usage: <nominal|calibrated> <image.bin> [capture-id] [options]");
    }
    int next = 3;
    uint32_t id = 0U;
    uint32_t producer = FOC_CAPTURE_HOST;
    if (argc > 3 && strncmp(argv[3], "--", 2) != 0) {
        id = (uint32_t)strtoul(argv[3], NULL, 0);
        next = 4;
    }
    for (; next < argc; next += 2) {
        if (next + 1 >= argc) {
            return fail("option without value");
        }
        if (strcmp(argv[next], "--adc") == 0) {
            char *cursor = argv[next + 1];
            for (unsigned int phase = 0U; phase < 3U; ++phase) {
                char *end = NULL;
                adc_target[phase] = strtod(cursor, &end);
                if (end == cursor || *end != (phase < 2U ? ',' : '\0')) {
                    return fail("--adc needs A,B,C");
                }
                cursor = end + 1;
            }
        } else if (strcmp(argv[next], "--noise") == 0) {
            adc_noise = (uint32_t)strtoul(argv[next + 1], NULL, 0);
        } else if (strcmp(argv[next], "--producer") == 0) {
            producer = strcmp(argv[next + 1], "board") == 0 ? FOC_CAPTURE_BOARD : FOC_CAPTURE_HOST;
        } else {
            return fail("unknown option");
        }
    }
    for (unsigned int phase = 0U; phase < 3U; ++phase) {
        if (!(adc_target[phase] >= 64.0 && adc_target[phase] <= 4000.0) || adc_noise > 60U) {
            return fail("ADC target outside 64..4000 or noise above 60");
        }
    }
    const bool calibrated = strcmp(argv[1], "calibrated") == 0;
    if (!calibrated && strcmp(argv[1], "nominal") != 0) {
        return fail("mode must be nominal or calibrated");
    }
    const struct foc_board_calibration_expectation expected = foc_board_calibration_expectation();
    if (foc_board_boot_select(
            &boot,
            &runtime,
            &output,
            &encoder,
            calibrated ? foc_board_calibration_supply() : NULL,
            calibrated ? &expected : NULL,
            0U
        ) != FOC_RUNTIME_OK) {
        return fail("boot selection failed");
    }
    if (boot.configuration !=
        (calibrated ? FOC_BOARD_CALIBRATED_CONFIGURATION : FOC_BOARD_NOMINAL_CONFIGURATION)) {
        return fail("unexpected effective configuration");
    }
    struct foc_capture_profile profile;
    if (!foc_board_boot_capture_profile(&boot, 0U, &profile)) {
        return fail("no capture profile");
    }
    if (id == 0U) {
        id = calibrated ? 34U : 33U;
    }
    const uint32_t start = UINT32_MAX - 4000U;
    if (!foc_board_capture_start(&recorder, id, producer, start, &profile)) {
        return fail("recorder did not start");
    }
    fill_rows(start);
    for (unsigned int n = 0U; n < 100U; ++n) {
        foc_board_capture_poll(&recorder, start + FOC_CAPTURE_WINDOW_US, 0U);
    }
    if (atomic_load(&recorder.state) != FOC_CAPTURE_READY) {
        return fail("recorder not sealed");
    }
    FILE *file = fopen(argv[2], "wb");
    if (file == NULL || fwrite(&recorder.image, sizeof recorder.image, 1U, file) != 1U ||
        fclose(file) != 0) {
        return fail("cannot write image");
    }
    printf(
        "%s capture %lu: cpr %lu direction %ld pole pairs %lu phase order %lu crc 0x%08lx\n",
        argv[1],
        (unsigned long)id,
        (unsigned long)recorder.image.header.encoder_cpr,
        (long)recorder.image.header.encoder_direction,
        (unsigned long)recorder.image.header.pole_pairs,
        (unsigned long)recorder.image.header.phase_order,
        (unsigned long)recorder.image.header.crc32
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
