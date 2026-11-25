/**
 * @file    test_foc_thermal.c
 * @brief   Tests for the NTC conversion, temperature filter, derating and I²t state.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Test role
 * ---------
 * This executable checks the β conversion against libm for both divider
 * topologies, the filter seeding and reset, the trip hysteresis and broken-sensor
 * trip, the derating window and the I²t settling, fold-back level and decay.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_thermal.h"

#include <math.h>
#include <stdio.h>

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static const struct foc_thermal_ntc ntc_ground =
    {3950.0f, 10000.0f, 10000.0f, FOC_THERMAL_NTC_TO_GROUND};
static const struct foc_thermal_ntc ntc_supply =
    {3950.0f, 10000.0f, 4700.0f, FOC_THERMAL_NTC_TO_SUPPLY};

/* Divider ratio the β model gives at a temperature, computed with libm. */
static float ratio_at(const struct foc_thermal_ntc *ntc, double celsius)
{
    const double r = ntc->r25_ohm * exp(ntc->beta_k * (1.0 / (celsius + 273.15) - 1.0 / 298.15));
    return ntc->topology == FOC_THERMAL_NTC_TO_GROUND
        ? (float)(r / (r + ntc->pullup_ohm))
        : (float)(ntc->pullup_ohm / (r + ntc->pullup_ohm));
}

static int test_conversion(void)
{
    TEST_ASSERT(foc_thermal_ntc_valid(&ntc_ground));
    TEST_ASSERT(foc_thermal_ntc_valid(&ntc_supply));
    TEST_ASSERT(fabsf(foc_thermal_ntc_celsius(&ntc_ground, 0.5f) - 25.0f) < 0.01f);
    for (int celsius = -20; celsius <= 140; celsius += 10) {
        const float ground = foc_thermal_ntc_celsius(&ntc_ground, ratio_at(&ntc_ground, celsius));
        const float supply = foc_thermal_ntc_celsius(&ntc_supply, ratio_at(&ntc_supply, celsius));
        TEST_ASSERT(fabsf(ground - (float)celsius) < 0.02f);
        TEST_ASSERT(fabsf(supply - (float)celsius) < 0.02f);
    }
    /* Open and shorted sensors give unusable ratios. */
    TEST_ASSERT(isnan(foc_thermal_ntc_celsius(&ntc_ground, 0.0f)));
    TEST_ASSERT(isnan(foc_thermal_ntc_celsius(&ntc_ground, 1.0f)));
    TEST_ASSERT(isnan(foc_thermal_ntc_celsius(&ntc_ground, NAN)));

    struct foc_thermal_ntc bad = ntc_ground;
    bad.beta_k = 0.0f;
    TEST_ASSERT(!foc_thermal_ntc_valid(&bad));
    bad = ntc_ground;
    bad.topology = (enum foc_thermal_topology)2;
    TEST_ASSERT(!foc_thermal_ntc_valid(&bad));
    return 0;
}

static int test_sensor(void)
{
    const struct foc_thermal_sensor_config config = {true, 75.0f, 95.0f};
    struct foc_thermal_sensor_config bad = config;
    bad.derate_c = 95.0f;
    TEST_ASSERT(foc_thermal_sensor_config_valid(&config));
    TEST_ASSERT(!foc_thermal_sensor_config_valid(&bad));
    bad.enabled = false;
    TEST_ASSERT(foc_thermal_sensor_config_valid(&bad));

    struct foc_thermal_sensor sensor;
    foc_thermal_sensor_reset(&sensor);
    /* The first reading seeds both stages, later readings are filtered. */
    float celsius = foc_thermal_sensor_update(
        &config,
        &ntc_ground,
        &sensor,
        true,
        ratio_at(&ntc_ground, 40.0),
        0.1f
    );
    TEST_ASSERT(fabsf(celsius - 40.0f) < 0.02f);
    celsius = foc_thermal_sensor_update(
        &config,
        &ntc_ground,
        &sensor,
        true,
        ratio_at(&ntc_ground, 100.0),
        0.1f
    );
    TEST_ASSERT(celsius > 40.0f && celsius < 42.0f && !sensor.tripped);
    for (int i = 0; i < 300; ++i) {
        celsius = foc_thermal_sensor_update(
            &config,
            &ntc_ground,
            &sensor,
            true,
            ratio_at(&ntc_ground, 102.0),
            0.1f
        );
    }
    TEST_ASSERT(fabsf(celsius - 102.0f) < 0.1f && sensor.tripped);
    /* Inside the derating window the trip holds; below the limit it releases. */
    for (int i = 0; i < 300; ++i) {
        celsius = foc_thermal_sensor_update(
            &config,
            &ntc_ground,
            &sensor,
            true,
            ratio_at(&ntc_ground, 97.0),
            0.1f
        );
    }
    TEST_ASSERT(sensor.tripped);
    for (int i = 0; i < 300; ++i) {
        celsius = foc_thermal_sensor_update(
            &config,
            &ntc_ground,
            &sensor,
            true,
            ratio_at(&ntc_ground, 90.0),
            0.1f
        );
    }
    TEST_ASSERT(!sensor.tripped && fabsf(foc_thermal_derate(&config, celsius) - 0.75f) < 0.01f);

    /* A broken sensor trips after FOC_THERMAL_INVALID_SAMPLES polls and re-seeds. */
    for (uint32_t i = 1U; i <= FOC_THERMAL_INVALID_SAMPLES; ++i) {
        celsius = foc_thermal_sensor_update(&config, &ntc_ground, &sensor, true, 1.0f, 0.1f);
        TEST_ASSERT(isnan(celsius));
        TEST_ASSERT(sensor.tripped == (i == FOC_THERMAL_INVALID_SAMPLES));
    }
    celsius = foc_thermal_sensor_update(&config, &ntc_ground, &sensor, false, 0.5f, 0.1f);
    TEST_ASSERT(isnan(celsius) && sensor.tripped);
    celsius = foc_thermal_sensor_update(
        &config,
        &ntc_ground,
        &sensor,
        true,
        ratio_at(&ntc_ground, 30.0),
        0.1f
    );
    TEST_ASSERT(fabsf(celsius - 30.0f) < 0.02f && !sensor.tripped && sensor.invalid_samples == 0U);

    /* Disabling resets the state. */
    const struct foc_thermal_sensor_config off = {false, 75.0f, 95.0f};
    TEST_ASSERT(isnan(foc_thermal_sensor_update(&off, &ntc_ground, &sensor, true, 0.5f, 0.1f)));
    TEST_ASSERT(!sensor.seeded && !sensor.tripped);
    return 0;
}

static int test_derate(void)
{
    const struct foc_thermal_sensor_config config = {true, 75.0f, 95.0f};
    const struct foc_thermal_sensor_config off = {false, 75.0f, 95.0f};
    TEST_ASSERT(foc_thermal_derate(&config, 20.0f) == 0.0f);
    TEST_ASSERT(foc_thermal_derate(&config, 75.0f) == 0.0f);
    TEST_ASSERT(fabsf(foc_thermal_derate(&config, 85.0f) - 0.5f) < 1e-6f);
    TEST_ASSERT(foc_thermal_derate(&config, 95.0f) == 1.0f);
    TEST_ASSERT(foc_thermal_derate(&config, 120.0f) == 1.0f);
    TEST_ASSERT(foc_thermal_derate(&config, NAN) == 1.0f);
    TEST_ASSERT(foc_thermal_derate(&off, NAN) == 0.0f);
    TEST_ASSERT(foc_thermal_derate(&off, 200.0f) == 0.0f);
    return 0;
}

static int test_i2t(void)
{
    struct foc_thermal_i2t i2t;
    TEST_ASSERT(!foc_thermal_i2t_prepare(&i2t, 0.0f, 30.0f));
    TEST_ASSERT(foc_thermal_i2t_step(&i2t, 0.7f, 4.0f, 200U) == 0.0f);
    TEST_ASSERT(!foc_thermal_i2t_prepare(&i2t, 1.0f, 0.0f));
    TEST_ASSERT(!foc_thermal_i2t_prepare(&i2t, NAN, 30.0f));
    TEST_ASSERT(foc_thermal_i2t_prepare(&i2t, 0.5f, 2.0f));

    /* Twice the rated current: θ(t) = 4 (1 − e^(−t/τ)) reaches 1 at τ ln(4/3). */
    float theta = 0.0f;
    uint32_t t_us = 0U;
    while (theta < FOC_THERMAL_I2T_FOLDBACK) {
        theta = foc_thermal_i2t_step(&i2t, theta, 1.0f, 200U);
        t_us += 200U;
    }
    const float expected_us = 2.0e6f * logf(4.0f / 3.0f);
    TEST_ASSERT(fabsf((float)t_us - expected_us) < 0.01f * expected_us);

    /* Held at the rated current θ settles at 1. */
    for (int i = 0; i < 100000; ++i) {
        theta = foc_thermal_i2t_step(&i2t, theta, 0.25f, 200U);
    }
    TEST_ASSERT(fabsf(theta - 1.0f) < 1e-3f);

    /* Without current it decays by e after τ. */
    for (int i = 0; i < 10000; ++i) {
        theta = foc_thermal_i2t_step(&i2t, theta, 0.0f, 200U);
    }
    TEST_ASSERT(fabsf(theta - expf(-1.0f)) < 0.01f);

    /* A long gap saturates the gain, a non-finite current keeps θ. */
    TEST_ASSERT(foc_thermal_i2t_step(&i2t, 0.3f, 0.0f, 10000000U) == 0.0f);
    TEST_ASSERT(foc_thermal_i2t_step(&i2t, 0.3f, INFINITY, 200U) == 0.3f);
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    if (test_conversion() != 0 || test_sensor() != 0 || test_derate() != 0 || test_i2t() != 0) {
        return 1;
    }
    puts("test-foc-thermal: PASS");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
