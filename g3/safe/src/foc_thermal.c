/**
 * @file    foc_thermal.c
 * @brief   NTC conversion, temperature filter, derating and I²t state.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_thermal.h` and owns only filter and trip state
 * - converts with the β equation and a libm-free natural logarithm
 * - never latches faults or touches registers
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_thermal.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define FOC_THERMAL_KELVIN_0C     273.15f
#define FOC_THERMAL_KELVIN_25C    298.15f
#define FOC_THERMAL_CONFIG_MIN_C  (-40.0f)
#define FOC_THERMAL_CONFIG_MAX_C  150.0f
/* Converted temperatures outside this range are treated as unusable readings. */
#define FOC_THERMAL_READING_MIN_C (-60.0f)
#define FOC_THERMAL_READING_MAX_C 250.0f

/* Private Helpers -------------------------------------------------------------- */

static bool foc_thermal_within(float value, float low, float high)
{
    return isfinite(value) && value >= low && value <= high;
}

/* Natural logarithm of a positive normal float: exponent split, then the atanh series
 * on a mantissa in [√½, √2); error below 1e-7. */
static float foc_thermal_ln(float x)
{
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    int32_t exponent = (int32_t)((bits >> 23) & 0xFFU) - 127;
    bits = (bits & 0x007FFFFFU) | 0x3F800000U;
    float mantissa;
    memcpy(&mantissa, &bits, sizeof(mantissa));
    if (mantissa > 1.41421356f) {
        mantissa *= 0.5f;
        exponent++;
    }
    const float s = (mantissa - 1.0f) / (mantissa + 1.0f);
    const float s2 = s * s;
    const float series = s * (2.0f + s2 * (0.66666667f + s2 * (0.4f + s2 * 0.28571429f)));
    return (float)exponent * 0.69314718f + series;
}

/* Public Functions ------------------------------------------------------------- */

bool foc_thermal_ntc_valid(const struct foc_thermal_ntc *ntc)
{
    return ntc != NULL && foc_thermal_within(ntc->beta_k, 1000.0f, 10000.0f) &&
        foc_thermal_within(ntc->r25_ohm, 100.0f, 1.0e6f) &&
        foc_thermal_within(ntc->pullup_ohm, 100.0f, 1.0e6f) &&
        (ntc->topology == FOC_THERMAL_NTC_TO_GROUND || ntc->topology == FOC_THERMAL_NTC_TO_SUPPLY);
}

bool foc_thermal_sensor_config_valid(const struct foc_thermal_sensor_config *config)
{
    if (config == NULL) {
        return false;
    }
    return !config->enabled ||
        (foc_thermal_within(config->derate_c, FOC_THERMAL_CONFIG_MIN_C, FOC_THERMAL_CONFIG_MAX_C) &&
         foc_thermal_within(config->limit_c, FOC_THERMAL_CONFIG_MIN_C, FOC_THERMAL_CONFIG_MAX_C) &&
         config->derate_c < config->limit_c);
}

float foc_thermal_ntc_celsius(const struct foc_thermal_ntc *ntc, float ratio)
{
    if (!foc_thermal_within(ratio, FOC_THERMAL_RATIO_MIN, 1.0f - FOC_THERMAL_RATIO_MIN)) {
        return NAN;
    }
    const float to_ground = ratio / (1.0f - ratio);
    const float resistance = ntc->pullup_ohm *
        (ntc->topology == FOC_THERMAL_NTC_TO_GROUND ? to_ground : 1.0f / to_ground);
    const float inverse_k =
        1.0f / FOC_THERMAL_KELVIN_25C + foc_thermal_ln(resistance / ntc->r25_ohm) / ntc->beta_k;
    const float celsius = 1.0f / inverse_k - FOC_THERMAL_KELVIN_0C;
    return inverse_k > 0.0f &&
            foc_thermal_within(celsius, FOC_THERMAL_READING_MIN_C, FOC_THERMAL_READING_MAX_C)
        ? celsius
        : NAN;
}

void foc_thermal_sensor_reset(struct foc_thermal_sensor *sensor)
{
    if (sensor != NULL) {
        *sensor = (struct foc_thermal_sensor){0};
    }
}

float foc_thermal_sensor_update(
    const struct foc_thermal_sensor_config *config,
    const struct foc_thermal_ntc *ntc,
    struct foc_thermal_sensor *sensor,
    bool sampled,
    float ratio,
    float alpha
)
{
    if (!config->enabled) {
        foc_thermal_sensor_reset(sensor);
        return NAN;
    }
    const float celsius = sampled ? foc_thermal_ntc_celsius(ntc, ratio) : NAN;
    if (!isfinite(celsius)) {
        sensor->seeded = false;
        if (sensor->invalid_samples < FOC_THERMAL_INVALID_SAMPLES) {
            sensor->invalid_samples++;
        }
        if (sensor->invalid_samples >= FOC_THERMAL_INVALID_SAMPLES) {
            sensor->tripped = true;
        }
        return NAN;
    }
    sensor->invalid_samples = 0U;
    if (!sensor->seeded) {
        sensor->stage_c = celsius;
        sensor->filtered_c = celsius;
        sensor->seeded = true;
    } else {
        sensor->stage_c += alpha * (celsius - sensor->stage_c);
        sensor->filtered_c += alpha * (sensor->stage_c - sensor->filtered_c);
    }
    if (sensor->filtered_c >= config->limit_c + FOC_THERMAL_TRIP_MARGIN_C) {
        sensor->tripped = true;
    } else if (sensor->filtered_c < config->limit_c) {
        sensor->tripped = false;
    }
    return sensor->filtered_c;
}

float foc_thermal_derate(const struct foc_thermal_sensor_config *config, float celsius)
{
    if (!config->enabled) {
        return 0.0f;
    }
    if (!isfinite(celsius) || celsius >= config->limit_c) {
        return 1.0f;
    }
    if (celsius <= config->derate_c) {
        return 0.0f;
    }
    return (celsius - config->derate_c) / (config->limit_c - config->derate_c);
}

bool foc_thermal_i2t_prepare(struct foc_thermal_i2t *i2t, float rated_a, float tau_s)
{
    *i2t = (struct foc_thermal_i2t){0};
    if (!isfinite(rated_a) || !(rated_a > 0.0f) || !isfinite(tau_s) || !(tau_s > 0.0f)) {
        return false;
    }
    i2t->rated_a = rated_a;
    i2t->inv_rated_sq = 1.0f / (rated_a * rated_a);
    i2t->rate_per_us = 1.0e-6f / tau_s;
    return true;
}

float foc_thermal_i2t_step(
    const struct foc_thermal_i2t *i2t,
    float theta,
    float current_sq,
    uint32_t dt_us
)
{
    if (!(i2t->rate_per_us > 0.0f)) {
        return 0.0f;
    }
    if (!isfinite(current_sq)) {
        return theta;
    }
    float gain = i2t->rate_per_us * (float)dt_us;
    if (gain > 1.0f) {
        gain = 1.0f;
    }
    return theta + gain * (current_sq * i2t->inv_rated_sq - theta);
}

/* ---------------------------------------- end of file ----------------------------------------- */
