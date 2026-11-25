/**
 * @file    foc_board_encoder_lut.c
 * @brief   CAL-9 harmonic fit of the encoder correction and Board1's table.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_board_encoder_lut.h`
 * - borrows the CAL-8 bins (1e-4 rad per unit here) and works in place
 * - harmonics by a rotating phasor, without per-bin trigonometry
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_encoder_lut.h"

#include "foc_trig.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define LUT_BINS        FOC_RUNTIME_COGGING_BINS
#define LUT_TWO_PI      6.28318530717958647692f
#define LUT_UNIT_RAD    1.0e-4f
/* Below this many raw counts of peak correction the encoder is left as it is. */
#define LUT_FLOOR_COUNT 4.0f

/* Public Variables ------------------------------------------------------------- */

struct foc_board_encoder_lut foc_board_encoder_lut_state;

/* Private Helpers -------------------------------------------------------------- */

static int32_t lut_round(float value)
{
    return (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static float lut_mean(const int16_t *bins)
{
    float sum = 0.0f;
    for (unsigned int bin = 0U; bin < LUT_BINS; ++bin) {
        sum += (float)bins[bin];
    }
    return sum / (float)LUT_BINS;
}

/* Fourier coefficients of one harmonic of the bins (in bin units). */
static void lut_harmonic(const int16_t *bins, unsigned int order, float *a, float *b)
{
    float step_sin;
    float step_cos;
    foc_sincosf((float)order * (LUT_TWO_PI / (float)LUT_BINS), &step_sin, &step_cos);
    float c = 1.0f;
    float s = 0.0f;
    float sum_c = 0.0f;
    float sum_s = 0.0f;
    for (unsigned int bin = 0U; bin < LUT_BINS; ++bin) {
        sum_c += (float)bins[bin] * c;
        sum_s += (float)bins[bin] * s;
        const float next_c = c * step_cos - s * step_sin;
        s = s * step_cos + c * step_sin;
        c = next_c;
    }
    *a = sum_c * (2.0f / (float)LUT_BINS);
    *b = sum_s * (2.0f / (float)LUT_BINS);
}

/* Public Functions ------------------------------------------------------------- */

bool foc_board_encoder_lut_descriptor(
    const struct foc_board_encoder_lut *table,
    uint32_t counts_per_revolution,
    struct foc_encoder_lut *descriptor
)
{
    if (table == NULL || descriptor == NULL || table->counts_per_revolution == 0U ||
        table->counts_per_revolution != counts_per_revolution) {
        return false;
    }
    *descriptor = (struct foc_encoder_lut){
        .offset_counts = table->offsets,
        .length = FOC_BOARD_ENCODER_LUT_KNOTS,
        .counts_per_revolution = counts_per_revolution,
        .encoder_identity = FOC_BOARD_ENCODER_LUT_IDENTITY,
    };
    return foc_encoder_lut_validate(
               descriptor,
               counts_per_revolution,
               FOC_BOARD_ENCODER_LUT_IDENTITY
           ) == FOC_ENCODER_STATUS_OK;
}

enum foc_board_encoder_lut_status foc_board_encoder_lut_fit(
    struct foc_board_encoder_lut *table,
    struct foc_board_cogging *bins,
    uint32_t pole_pairs,
    uint32_t counts_per_revolution
)
{
    int16_t *forward = bins->mean[0];
    int16_t *backward = bins->mean[1];
    const uint32_t limit = (uint32_t)(FOC_BOARD_COGGING_MAX_EMPTY * (float)LUT_BINS);
    if (pole_pairs < 2U || counts_per_revolution < FOC_BOARD_ENCODER_LUT_KNOTS) {
        table->status = FOC_BOARD_ENCODER_LUT_FAILED;
        return table->status;
    }
    if (foc_board_cogging_empty_bins(forward) > limit ||
        foc_board_cogging_empty_bins(backward) > limit || !foc_board_cogging_fill_bins(forward) ||
        !foc_board_cogging_fill_bins(backward)) {
        table->status = FOC_BOARD_ENCODER_LUT_SAMPLES;
        return table->status;
    }
    const float forward_mean = lut_mean(forward);
    const float backward_mean = lut_mean(backward);
    float cross = 0.0f;
    float forward_power = 0.0f;
    float backward_power = 0.0f;
    for (unsigned int bin = 0U; bin < LUT_BINS; ++bin) {
        const float f = (float)forward[bin] - forward_mean;
        const float b = (float)backward[bin] - backward_mean;
        cross += f * b;
        forward_power += f * f;
        backward_power += b * b;
        forward[bin] = (int16_t)lut_round(0.5f * (f + b));
    }
    table->consistency = forward_power > 0.0f && backward_power > 0.0f
        ? cross / sqrtf(forward_power * backward_power)
        : 0.0f;
    /* Harmonics 1..16 that are not multiples of the pole pairs, in bin units. */
    float a[FOC_BOARD_ENCODER_LUT_HARMONICS + 1U] = {0.0f};
    float b[FOC_BOARD_ENCODER_LUT_HARMONICS + 1U] = {0.0f};
    for (unsigned int order = 1U; order <= FOC_BOARD_ENCODER_LUT_HARMONICS; ++order) {
        if (order % pole_pairs != 0U) {
            lut_harmonic(forward, order, &a[order], &b[order]);
        }
    }
    /* Synthesize the correction at the knots, in raw counts. */
    const float counts_per_unit = LUT_UNIT_RAD * (float)counts_per_revolution / LUT_TWO_PI;
    int16_t candidate[FOC_BOARD_ENCODER_LUT_KNOTS];
    float peak = 0.0f;
    float power = 0.0f;
    for (unsigned int knot = 0U; knot < FOC_BOARD_ENCODER_LUT_KNOTS; ++knot) {
        float value = 0.0f;
        for (unsigned int order = 1U; order <= FOC_BOARD_ENCODER_LUT_HARMONICS; ++order) {
            if (a[order] == 0.0f && b[order] == 0.0f) {
                continue;
            }
            float s;
            float c;
            foc_sincosf(
                (float)(order * knot) * (LUT_TWO_PI / (float)FOC_BOARD_ENCODER_LUT_KNOTS),
                &s,
                &c
            );
            value += a[order] * c + b[order] * s;
        }
        value *= counts_per_unit;
        peak = fmaxf(peak, fabsf(value));
        power += value * value;
        const float clamped = fmaxf(-(float)INT16_MAX, fminf((float)INT16_MAX, value));
        candidate[knot] = (int16_t)lut_round(clamped);
    }
    const float rad_per_count = LUT_TWO_PI / (float)counts_per_revolution;
    table->peak_rad = peak * rad_per_count;
    table->rms_rad = sqrtf(power / (float)FOC_BOARD_ENCODER_LUT_KNOTS) * rad_per_count;
    if (peak < LUT_FLOOR_COUNT) {
        memset(table->offsets, 0, sizeof table->offsets);
        table->counts_per_revolution = 0U;
        table->dirty = true;
        table->status = FOC_BOARD_ENCODER_LUT_NEGLIGIBLE;
        return table->status;
    }
    if (table->consistency < FOC_BOARD_COGGING_MIN_CONSISTENCY) {
        table->status = FOC_BOARD_ENCODER_LUT_INCONSISTENT;
        return table->status;
    }
    const struct foc_encoder_lut descriptor = {
        .offset_counts = candidate,
        .length = FOC_BOARD_ENCODER_LUT_KNOTS,
        .counts_per_revolution = counts_per_revolution,
        .encoder_identity = FOC_BOARD_ENCODER_LUT_IDENTITY,
    };
    if (foc_encoder_lut_validate(
            &descriptor,
            counts_per_revolution,
            FOC_BOARD_ENCODER_LUT_IDENTITY
        ) != FOC_ENCODER_STATUS_OK) {
        table->status = FOC_BOARD_ENCODER_LUT_RANGE;
        return table->status;
    }
    memcpy(table->offsets, candidate, sizeof table->offsets);
    table->counts_per_revolution = counts_per_revolution;
    table->dirty = true;
    table->status = FOC_BOARD_ENCODER_LUT_VALID;
    return table->status;
}

/* ---------------------------------------- end of file ----------------------------------------- */
