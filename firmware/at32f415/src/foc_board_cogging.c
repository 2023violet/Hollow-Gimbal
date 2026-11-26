/**
 * @file    foc_board_cogging.c
 * @brief   CAL-8 sweep reference, per-direction angle bins and the cogging table fit.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_board_cogging.h`
 * - bins hold running means in 0.1 mA; INT16_MIN marks a bin without samples
 * - the fit works in place on the bins, without large stack buffers
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_cogging.h"

#include "foc_trig.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define COGGING_BINS        FOC_RUNTIME_COGGING_BINS
#define COGGING_EMPTY       INT16_MIN
#define COGGING_TWO_PI      6.28318530717958647692f
#define COGGING_UNIT_A      1.0e-4f
/* A peak below this many amperes, or this share of the q limit, is not compensated. */
#define COGGING_FLOOR_A     0.01f
#define COGGING_FLOOR_SHARE 0.01f

/* Public Variables ------------------------------------------------------------- */

/* Zero-initialized: an empty table, no result; the bins are cleared when a sweep begins. */
struct foc_board_cogging foc_board_cogging_state;

/* Private Helpers -------------------------------------------------------------- */

static int32_t cogging_round(float value)
{
    return (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static unsigned int cogging_bin(float mechanical_rad)
{
    const float turns = mechanical_rad * (1.0f / COGGING_TWO_PI);
    float fraction = turns - (float)(int32_t)turns;
    if (fraction < 0.0f) {
        fraction += 1.0f;
    }
    /* Nearest bin: bin k holds samples around k * 2π / BINS, where the Runtime reads it. */
    return (unsigned int)(fraction * (float)COGGING_BINS + 0.5f) % COGGING_BINS;
}

static void cogging_clear_bins(struct foc_board_cal_bins *bins)
{
    for (unsigned int bin = 0U; bin < COGGING_BINS; ++bin) {
        bins->mean[0][bin] = COGGING_EMPTY;
        bins->mean[1][bin] = COGGING_EMPTY;
    }
    memset(bins->count, 0, sizeof bins->count);
}

uint32_t foc_board_cogging_empty_bins(const int16_t *bins)
{
    uint32_t empty = 0U;
    for (unsigned int bin = 0U; bin < COGGING_BINS; ++bin) {
        empty += bins[bin] == COGGING_EMPTY ? 1U : 0U;
    }
    return empty;
}

/* Fill empty bins by circular linear interpolation between the nearest filled ones. */
bool foc_board_cogging_fill_bins(int16_t *bins)
{
    unsigned int first = COGGING_BINS;
    for (unsigned int bin = 0U; bin < COGGING_BINS; ++bin) {
        if (bins[bin] != COGGING_EMPTY) {
            first = bin;
            break;
        }
    }
    if (first == COGGING_BINS) {
        return false;
    }
    unsigned int previous = first;
    for (unsigned int step = 1U; step <= COGGING_BINS; ++step) {
        const unsigned int bin = (first + step) % COGGING_BINS;
        if (bins[bin] == COGGING_EMPTY) {
            continue;
        }
        const unsigned int gap = (bin + COGGING_BINS - previous) % COGGING_BINS;
        const unsigned int span = gap == 0U ? COGGING_BINS : gap;
        for (unsigned int offset = 1U; offset < span; ++offset) {
            const float blend = (float)offset / (float)span;
            bins[(previous + offset) % COGGING_BINS] = (int16_t)cogging_round(
                (1.0f - blend) * (float)bins[previous] + blend * (float)bins[bin]
            );
        }
        previous = bin;
    }
    return true;
}

static float cogging_mean(const int16_t *bins)
{
    float sum = 0.0f;
    for (unsigned int bin = 0U; bin < COGGING_BINS; ++bin) {
        sum += (float)bins[bin];
    }
    return sum / (float)COGGING_BINS;
}

/* Public Functions ------------------------------------------------------------- */

void foc_board_cogging_init(struct foc_board_cogging *cogging)
{
    struct foc_board_cal_bins *const bins = cogging->bins;
    memset(cogging, 0, sizeof *cogging);
    cogging->bins = bins;
}

void foc_board_cogging_begin(struct foc_board_cogging *cogging, float origin_rad, float speed_rad_s)
{
    if (cogging->bins != NULL) {
        cogging_clear_bins(cogging->bins);
    }
    cogging->empty[0] = 0U;
    cogging->empty[1] = 0U;
    cogging->phase = FOC_BOARD_COGGING_LEAD;
    cogging->origin_rad = origin_rad;
    cogging->speed_rad_s = speed_rad_s;
    cogging->travel_rad = 0.0f;
    cogging->sensor_seen = false;
    cogging->status = FOC_BOARD_COGGING_NONE;
    cogging->peak_a = 0.0f;
    cogging->rms_a = 0.0f;
    cogging->friction_a = 0.0f;
    cogging->consistency = 0.0f;
}

int foc_board_cogging_step(
    struct foc_board_cogging *cogging,
    uint32_t elapsed_us,
    float *reference_rad
)
{
    const float lead = FOC_BOARD_COGGING_LEAD_RAD;
    const float turn = COGGING_TWO_PI + FOC_BOARD_COGGING_MARGIN_RAD;
    const float forward_end = lead + turn;
    const float turn_end = forward_end + 2.0f * lead;
    const float backward_end = turn_end + turn;
    if (cogging->phase != FOC_BOARD_COGGING_IDLE && cogging->phase != FOC_BOARD_COGGING_END) {
        cogging->travel_rad = cogging->speed_rad_s * ((float)elapsed_us * 1.0e-6f);
    }
    const float s = cogging->travel_rad;
    const enum foc_board_cogging_phase previous = cogging->phase;
    int direction = -1;
    float offset;
    if (previous == FOC_BOARD_COGGING_IDLE) {
        offset = 0.0f;
    } else if (s < lead) {
        cogging->phase = FOC_BOARD_COGGING_LEAD;
        offset = s;
    } else if (s < forward_end) {
        cogging->phase = FOC_BOARD_COGGING_FORWARD;
        offset = s;
        direction = 0;
    } else if (s < turn_end) {
        cogging->phase = FOC_BOARD_COGGING_TURN;
        offset = forward_end + lead - fabsf(s - (forward_end + lead));
    } else if (s < backward_end) {
        cogging->phase = FOC_BOARD_COGGING_BACKWARD;
        offset = forward_end - (s - turn_end);
        direction = 1;
    } else {
        cogging->phase = FOC_BOARD_COGGING_END;
        offset = lead;
    }
    struct foc_board_cal_bins *const bins = cogging->bins;
    if (bins != NULL && previous == FOC_BOARD_COGGING_FORWARD &&
        cogging->phase != FOC_BOARD_COGGING_FORWARD) {
        cogging->empty[0] = foc_board_cogging_empty_bins(bins->mean[0]);
        memset(bins->count, 0, sizeof bins->count);
    }
    if (bins != NULL && previous == FOC_BOARD_COGGING_BACKWARD &&
        cogging->phase == FOC_BOARD_COGGING_END) {
        cogging->empty[1] = foc_board_cogging_empty_bins(bins->mean[1]);
    }
    *reference_rad = cogging->origin_rad + offset;
    return direction;
}

void foc_board_cogging_sample(
    struct foc_board_cogging *cogging,
    int direction,
    uint32_t sensor_us,
    float mechanical_rad,
    float iq_a
)
{
    struct foc_board_cal_bins *const bins = cogging->bins;
    if (bins == NULL || (direction != 0 && direction != 1) || !isfinite(mechanical_rad) ||
        !isfinite(iq_a) || (cogging->sensor_seen && sensor_us == cogging->last_sensor_us)) {
        return;
    }
    cogging->sensor_seen = true;
    cogging->last_sensor_us = sensor_us;
    const unsigned int bin = cogging_bin(mechanical_rad);
    const float scaled = fmaxf(-32767.0f, fminf(32767.0f, iq_a / COGGING_UNIT_A));
    int16_t *mean = &bins->mean[direction][bin];
    if (bins->count[bin] < UINT8_MAX) {
        bins->count[bin]++;
    }
    if (*mean == COGGING_EMPTY) {
        *mean = (int16_t)cogging_round(scaled);
    } else {
        const float updated = (float)*mean + (scaled - (float)*mean) / (float)bins->count[bin];
        *mean = (int16_t)cogging_round(updated);
    }
}

enum foc_board_cogging_status foc_board_cogging_finish(
    struct foc_board_cogging *cogging,
    bool keep_first_harmonic,
    float iq_limit_a
)
{
    if (cogging->bins == NULL) {
        cogging->status = FOC_BOARD_COGGING_FAILED;
        return cogging->status;
    }
    int16_t *forward = cogging->bins->mean[0];
    int16_t *backward = cogging->bins->mean[1];
    const uint32_t limit = (uint32_t)(FOC_BOARD_COGGING_MAX_EMPTY * (float)COGGING_BINS);
    cogging->empty[0] = foc_board_cogging_empty_bins(forward);
    cogging->empty[1] = foc_board_cogging_empty_bins(backward);
    if (cogging->empty[0] > limit || cogging->empty[1] > limit ||
        !foc_board_cogging_fill_bins(forward) || !foc_board_cogging_fill_bins(backward)) {
        cogging->status = FOC_BOARD_COGGING_SAMPLES;
        return cogging->status;
    }
    const float forward_mean = cogging_mean(forward);
    const float backward_mean = cogging_mean(backward);
    cogging->friction_a = 0.5f * (forward_mean - backward_mean) * COGGING_UNIT_A;
    float cross = 0.0f;
    float forward_power = 0.0f;
    float backward_power = 0.0f;
    for (unsigned int bin = 0U; bin < COGGING_BINS; ++bin) {
        const float f = (float)forward[bin] - forward_mean;
        const float b = (float)backward[bin] - backward_mean;
        cross += f * b;
        forward_power += f * f;
        backward_power += b * b;
        /* The average of both directions, without its mean, in 0.1 mA (kept in forward). */
        forward[bin] = (int16_t)cogging_round(0.5f * (f + b));
    }
    cogging->consistency = forward_power > 0.0f && backward_power > 0.0f
        ? cross / sqrtf(forward_power * backward_power)
        : 0.0f;
    if (!keep_first_harmonic) {
        float a = 0.0f;
        float b = 0.0f;
        for (unsigned int bin = 0U; bin < COGGING_BINS; ++bin) {
            float s;
            float c;
            foc_sincosf((float)bin * (COGGING_TWO_PI / (float)COGGING_BINS), &s, &c);
            a += (float)forward[bin] * c;
            b += (float)forward[bin] * s;
        }
        a *= 2.0f / (float)COGGING_BINS;
        b *= 2.0f / (float)COGGING_BINS;
        for (unsigned int bin = 0U; bin < COGGING_BINS; ++bin) {
            float s;
            float c;
            foc_sincosf((float)bin * (COGGING_TWO_PI / (float)COGGING_BINS), &s, &c);
            forward[bin] = (int16_t)cogging_round((float)forward[bin] - a * c - b * s);
        }
    }
    float peak = 0.0f;
    float power = 0.0f;
    for (unsigned int bin = 0U; bin < COGGING_BINS; ++bin) {
        const float value = (float)forward[bin] * COGGING_UNIT_A;
        peak = fmaxf(peak, fabsf(value));
        power += value * value;
    }
    cogging->peak_a = peak;
    cogging->rms_a = sqrtf(power / (float)COGGING_BINS);
    if (peak < fmaxf(COGGING_FLOOR_A, COGGING_FLOOR_SHARE * iq_limit_a)) {
        cogging->table.scale_a = 0.0f;
        memset(cogging->table.code, 0, sizeof cogging->table.code);
        cogging->dirty = true;
        cogging->status = FOC_BOARD_COGGING_NEGLIGIBLE;
    } else if (cogging->consistency < FOC_BOARD_COGGING_MIN_CONSISTENCY) {
        cogging->status = FOC_BOARD_COGGING_INCONSISTENT;
    } else if (!(peak <= 0.5f * iq_limit_a)) {
        cogging->status = FOC_BOARD_COGGING_RANGE;
    } else {
        cogging->table.scale_a = peak / 127.0f;
        for (unsigned int bin = 0U; bin < COGGING_BINS; ++bin) {
            const float code = (float)forward[bin] * COGGING_UNIT_A / cogging->table.scale_a;
            cogging->table.code[bin] = (int8_t)cogging_round(fmaxf(-127.0f, fminf(127.0f, code)));
        }
        cogging->dirty = true;
        cogging->status = FOC_BOARD_COGGING_VALID;
    }
    return cogging->status;
}

/* ---------------------------------------- end of file ----------------------------------------- */
