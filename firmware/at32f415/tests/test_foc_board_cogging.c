/**
 * @file    test_foc_board_cogging.c
 * @brief   CAL-8 sweep, binning and fit against a synthetic rotor.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Test role
 * ---------
 * The rotor follows the reference exactly while the q current carries an 84-cycle cogging
 * term, Coulomb friction, a constant load and a once-per-turn imbalance. The fit must
 * recover the cogging term and the friction, drop the imbalance unless asked to keep it,
 * and report NEGLIGIBLE, INCONSISTENT, RANGE and SAMPLES for the matching inputs.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_cogging.h"

#include <math.h>
#include <stdio.h>

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Types ---------------------------------------------------------------- */

struct plant {
    double cogging_a;
    double friction_a;
    double load_a;
    double imbalance_a;
    double noise_a;
    bool stuck;
    uint32_t noise_state;
};

/* Private Helpers -------------------------------------------------------------- */

static double cogging_at(const struct plant *plant, double angle)
{
    return plant->cogging_a * sin(84.0 * angle + 0.3);
}

static double noise(struct plant *plant)
{
    plant->noise_state ^= plant->noise_state << 13;
    plant->noise_state ^= plant->noise_state >> 17;
    plant->noise_state ^= plant->noise_state << 5;
    return ((double)(plant->noise_state & 0xFFFFU) / 65535.0 - 0.5) * 2.0 * plant->noise_a;
}

/* Run a full sweep at 0.25 rad/s with 500 µs sensor samples from origin 1.0 rad. */
static enum foc_board_cogging_status
sweep(struct foc_board_cogging *cogging, struct plant *plant, bool keep_first)
{
    foc_board_cogging_begin(cogging, 1.0f, 0.25f);
    uint32_t t = 0U;
    float reference = 1.0f;
    for (unsigned int guard = 0U; guard < 1000000U; ++guard) {
        t += 500U;
        const int direction = foc_board_cogging_step(cogging, t, &reference);
        if (cogging->phase == FOC_BOARD_COGGING_END) {
            break;
        }
        const double angle = plant->stuck ? 1.0 : (double)reference;
        const double sign = direction == 1 ? -1.0 : 1.0;
        const double iq = cogging_at(plant, angle) + sign * plant->friction_a + plant->load_a +
            plant->imbalance_a * cos(angle) + noise(plant);
        foc_board_cogging_sample(cogging, direction, t, (float)angle, (float)iq);
        foc_board_cogging_sample(cogging, direction, t, (float)angle, 99.0f); /* repeat */
    }
    return foc_board_cogging_finish(cogging, keep_first, 2.5f);
}

/* Largest table error against the cogging term (plus the imbalance when kept), in A. */
static double
table_error(const struct foc_board_cogging *cogging, const struct plant *plant, bool with_imbalance)
{
    double worst = 0.0;
    for (unsigned int bin = 0U; bin < FOC_RUNTIME_COGGING_BINS; ++bin) {
        const double angle = (double)bin * 6.283185307179586 / FOC_RUNTIME_COGGING_BINS;
        const double truth =
            cogging_at(plant, angle) + (with_imbalance ? plant->imbalance_a * cos(angle) : 0.0);
        const double value = (double)cogging->table.scale_a * (double)cogging->table.code[bin];
        worst = fmax(worst, fabs(value - truth));
    }
    return worst;
}

/* Test Cases ------------------------------------------------------------------- */

static int test_fit(void)
{
    static struct foc_board_cal_bins bins;
    static struct foc_board_cogging cogging = {.bins = &bins};
    struct plant plant = {0.2, 0.05, 0.3, 0.1, 0.005, false, 1U};
    foc_board_cogging_init(&cogging);
    CHECK(cogging.table.scale_a == 0.0f && cogging.status == FOC_BOARD_COGGING_NONE);
    CHECK(sweep(&cogging, &plant, false) == FOC_BOARD_COGGING_VALID);
    CHECK(cogging.empty[0] == 0U && cogging.empty[1] == 0U && cogging.dirty);
    CHECK(fabsf(cogging.friction_a - 0.05f) < 0.003f);
    CHECK(fabsf(cogging.peak_a - 0.2f) < 0.01f && fabsf(cogging.rms_a - 0.1414f) < 0.005f);
    CHECK(cogging.consistency > 0.99f);
    CHECK(table_error(&cogging, &plant, false) < 0.012);

    /* Keeping the once-per-turn component puts the imbalance in the table. */
    CHECK(sweep(&cogging, &plant, true) == FOC_BOARD_COGGING_VALID);
    CHECK(table_error(&cogging, &plant, true) < 0.015);
    return 0;
}

static int test_refusals(void)
{
    static struct foc_board_cal_bins bins;
    static struct foc_board_cogging cogging = {.bins = &bins};
    foc_board_cogging_init(&cogging);
    struct plant plant = {0.2, 0.05, 0.3, 0.0, 0.0, false, 1U};
    CHECK(sweep(&cogging, &plant, false) == FOC_BOARD_COGGING_VALID);
    const float scale = cogging.table.scale_a;
    cogging.dirty = false;

    /* Noise alone: the shapes disagree; the table stays. */
    plant = (struct plant){0.0, 0.05, 0.3, 0.0, 0.2, false, 7U};
    CHECK(sweep(&cogging, &plant, false) == FOC_BOARD_COGGING_INCONSISTENT);
    CHECK(cogging.table.scale_a == scale && !cogging.dirty);

    /* Above half the q limit: refused. */
    plant = (struct plant){1.5, 0.0, 0.0, 0.0, 0.0, false, 1U};
    CHECK(sweep(&cogging, &plant, false) == FOC_BOARD_COGGING_RANGE);
    CHECK(cogging.table.scale_a == scale);

    /* A rotor that does not turn leaves bins empty. */
    plant = (struct plant){0.2, 0.0, 0.0, 0.0, 0.0, true, 1U};
    CHECK(sweep(&cogging, &plant, false) == FOC_BOARD_COGGING_SAMPLES);
    CHECK(cogging.empty[0] > FOC_RUNTIME_COGGING_BINS / 2U);

    /* No cogging: negligible, and the table is cleared. */
    plant = (struct plant){0.002, 0.05, 0.3, 0.0, 0.0, false, 1U};
    CHECK(sweep(&cogging, &plant, false) == FOC_BOARD_COGGING_NEGLIGIBLE);
    CHECK(cogging.table.scale_a == 0.0f && cogging.dirty);
    return 0;
}

static int test_reference(void)
{
    static struct foc_board_cal_bins bins;
    static struct foc_board_cogging cogging = {.bins = &bins};
    foc_board_cogging_init(&cogging);
    foc_board_cogging_begin(&cogging, 2.0f, 1.0f);
    float reference = 0.0f;
    float highest = 0.0f;
    float lowest = 100.0f;
    unsigned int forward = 0U;
    unsigned int backward = 0U;
    uint32_t elapsed_us = 0U;
    while (cogging.phase != FOC_BOARD_COGGING_END) {
        elapsed_us += 1000U;
        const int direction = foc_board_cogging_step(&cogging, elapsed_us, &reference);
        forward += direction == 0 ? 1U : 0U;
        backward += direction == 1 ? 1U : 0U;
        highest = fmaxf(highest, reference);
        lowest = fminf(lowest, reference);
    }
    const float turn = 6.2831853f + FOC_BOARD_COGGING_MARGIN_RAD;
    CHECK(fabsf(highest - (2.0f + 2.0f * FOC_BOARD_COGGING_LEAD_RAD + turn)) < 0.002f);
    CHECK(fabsf(lowest - 2.0f) < 0.002f);
    CHECK(fabsf(reference - (2.0f + FOC_BOARD_COGGING_LEAD_RAD)) < 1.0e-5f);
    CHECK(fabsf((float)forward * 0.001f - turn) < 0.002f);
    CHECK(fabsf((float)backward * 0.001f - turn) < 0.002f);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

/* The bins are borrowed: init keeps the pointer and leaves them alone, and without bins
 * the acquisition refuses instead of writing anywhere. */
static int test_borrowed_bins(void)
{
    static struct foc_board_cal_bins bins;
    static struct foc_board_cogging cogging = {.bins = &bins};
    bins.mean[1][7] = 1234;
    foc_board_cogging_init(&cogging);
    CHECK(cogging.bins == &bins && bins.mean[1][7] == 1234);
    foc_board_cogging_begin(&cogging, 0.0f, 1.0f);
    CHECK(bins.mean[1][7] == INT16_MIN && bins.count[7] == 0U);

    static struct foc_board_cogging detached;
    foc_board_cogging_init(&detached);
    CHECK(detached.bins == NULL);
    foc_board_cogging_begin(&detached, 0.0f, 1.0f);
    float reference = 0.0f;
    const int direction = foc_board_cogging_step(&detached, 1000000U, &reference);
    CHECK(direction == 0);
    foc_board_cogging_sample(&detached, direction, 1U, 1.0f, 0.1f);
    CHECK(foc_board_cogging_finish(&detached, false, 2.5f) == FOC_BOARD_COGGING_FAILED);
    CHECK(detached.table.scale_a == 0.0f && !detached.dirty);
    return 0;
}

int main(void)
{
    if (test_reference() != 0 || test_fit() != 0 || test_refusals() != 0 ||
        test_borrowed_bins() != 0) {
        return 1;
    }
    puts("test-foc-board-cogging: PASS");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
