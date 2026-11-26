/**
 * @file    test_foc_board_encoder_lut.c
 * @brief   CAL-9 fit against a synthetic encoder dragged by a voltage vector.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Test role
 * ---------
 * The rotor lags the field reference by a friction term (opposite in each direction) and
 * by a cogging term at 84 per turn (12 x 7 pole pairs); the encoder adds 1st, 2nd and 3rd
 * harmonic errors. The fit must return the negative encoder error, ignore the lags, and
 * report NEGLIGIBLE, INCONSISTENT, RANGE and FAILED for the matching inputs.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_encoder_lut.h"

#include <math.h>
#include <stdio.h>

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define TWO_PI 6.283185307179586
#define CPR    65536U

/* Private Types ---------------------------------------------------------------- */

struct encoder {
    double e1;
    double e2;
    double e3;
    double lag;
    double cogging;
    double noise;
    uint32_t state;
};

/* Private Helpers -------------------------------------------------------------- */

static double error_at(const struct encoder *encoder, double angle)
{
    return encoder->e1 * sin(angle + 0.4) + encoder->e2 * sin(2.0 * angle - 1.0) +
        encoder->e3 * sin(3.0 * angle + 2.0);
}

static double noise(struct encoder *encoder)
{
    encoder->state ^= encoder->state << 13;
    encoder->state ^= encoder->state >> 17;
    encoder->state ^= encoder->state << 5;
    return ((double)(encoder->state & 0xFFFFU) / 65535.0 - 0.5) * 2.0 * encoder->noise;
}

/* Forward and backward sweep at 1 rad/s with 500 µs encoder samples, then the fit. */
static enum foc_board_encoder_lut_status sweep(
    struct foc_board_encoder_lut *table,
    struct foc_board_cogging *cogging,
    struct encoder *encoder,
    uint32_t pole_pairs
)
{
    foc_board_cogging_begin(cogging, 0.7f, 1.0f);
    uint32_t t = 0U;
    float reference = 0.0f;
    for (unsigned int guard = 0U; guard < 1000000U; ++guard) {
        t += 500U;
        const int direction = foc_board_cogging_step(cogging, t, &reference);
        if (cogging->phase == FOC_BOARD_COGGING_END) {
            break;
        }
        const double sign = direction == 1 ? -1.0 : 1.0;
        const double truth = (double)reference - sign * encoder->lag +
            encoder->cogging * sin(84.0 * (double)reference + 0.2);
        const double measured = truth + error_at(encoder, truth) + noise(encoder);
        foc_board_cogging_sample(
            cogging,
            direction,
            t,
            (float)measured,
            (float)((double)reference - measured)
        );
    }
    return foc_board_encoder_lut_fit(table, cogging->bins, pole_pairs, CPR);
}

static double worst_knot(const struct foc_board_encoder_lut *table, const struct encoder *encoder)
{
    double worst = 0.0;
    for (unsigned int knot = 0U; knot < FOC_BOARD_ENCODER_LUT_KNOTS; ++knot) {
        const double angle = TWO_PI * knot / FOC_BOARD_ENCODER_LUT_KNOTS;
        const double expected = -error_at(encoder, angle) * CPR / TWO_PI;
        worst = fmax(worst, fabs((double)table->offsets[knot] - expected));
    }
    return worst;
}

/* Test Cases ------------------------------------------------------------------- */

static int test_fit(void)
{
    static struct foc_board_cal_bins bins;
    static struct foc_board_cogging cogging = {.bins = &bins};
    static struct foc_board_encoder_lut table;
    struct encoder encoder = {0.01, 0.004, 0.002, 0.003, 0.002, 0.0005, 1U};
    foc_board_cogging_init(&cogging);
    CHECK(sweep(&table, &cogging, &encoder, 7U) == FOC_BOARD_ENCODER_LUT_VALID);
    /* 0.01 rad is 104 counts: within 3 counts despite the lags and the noise (the encoder
     * error read at the reading instead of the true angle and rounding leave about 2). */
    CHECK(worst_knot(&table, &encoder) < 3.0);
    CHECK(table.counts_per_revolution == CPR && table.dirty && table.consistency > 0.9f);
    CHECK(fabsf(table.peak_rad - 0.0127f) < 0.002f);
    struct foc_encoder_lut descriptor;
    CHECK(foc_board_encoder_lut_descriptor(&table, CPR, &descriptor));
    CHECK(
        descriptor.length == FOC_BOARD_ENCODER_LUT_KNOTS &&
        descriptor.offset_counts == table.offsets
    );
    CHECK(!foc_board_encoder_lut_descriptor(&table, 4096U, &descriptor));
    return 0;
}

static int test_refusals(void)
{
    static struct foc_board_cal_bins bins;
    static struct foc_board_cogging cogging = {.bins = &bins};
    static struct foc_board_encoder_lut table;
    foc_board_cogging_init(&cogging);
    struct encoder encoder = {0.01, 0.0, 0.0, 0.003, 0.0, 0.0, 1U};
    CHECK(sweep(&table, &cogging, &encoder, 7U) == FOC_BOARD_ENCODER_LUT_VALID);
    const int16_t first = table.offsets[0];
    table.dirty = false;

    /* Noise only: inconsistent, the table stays. */
    encoder = (struct encoder){0.0, 0.0, 0.0, 0.003, 0.0, 0.01, 9U};
    CHECK(sweep(&table, &cogging, &encoder, 7U) == FOC_BOARD_ENCODER_LUT_INCONSISTENT);
    CHECK(table.offsets[0] == first && !table.dirty);

    /* A correction steeper than the sensor contract allows. */
    encoder = (struct encoder){0.0, 0.0, 0.25, 0.0, 0.0, 0.0, 1U};
    CHECK(sweep(&table, &cogging, &encoder, 7U) == FOC_BOARD_ENCODER_LUT_RANGE);

    /* One pole pair would remove every harmonic. */
    encoder = (struct encoder){0.01, 0.0, 0.0, 0.0, 0.0, 0.0, 1U};
    CHECK(sweep(&table, &cogging, &encoder, 1U) == FOC_BOARD_ENCODER_LUT_FAILED);
    CHECK(foc_board_encoder_lut_fit(&table, NULL, 7U, CPR) == FOC_BOARD_ENCODER_LUT_FAILED);

    /* A linear encoder: negligible, the table is cleared. */
    encoder = (struct encoder){0.0, 0.0, 0.0, 0.003, 0.002, 0.0, 1U};
    CHECK(sweep(&table, &cogging, &encoder, 7U) == FOC_BOARD_ENCODER_LUT_NEGLIGIBLE);
    CHECK(table.counts_per_revolution == 0U && table.dirty);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    if (test_fit() != 0 || test_refusals() != 0) {
        return 1;
    }
    puts("test-foc-board-encoder-lut: PASS");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
