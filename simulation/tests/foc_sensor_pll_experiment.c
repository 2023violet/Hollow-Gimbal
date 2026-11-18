/**
 * @file    foc_sensor_pll_experiment.c
 * @brief   Local phase/velocity PLL with backward-Euler pole placement.
 * @version v0.1.0
 * @date    2026.11.27
 *
 * Implementation role
 * -------------------
 * Test a critically damped PLL on the accepted Sensor increment. The
 * measured angle is linear over the previous interval. No accumulated
 * mechanical float or estimated phase is published to the controller.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_sensor_pll_experiment.h"

#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

#define PLL_PHASE_PI (3.14159265358979323846f)

/* Public Functions ------------------------------------------------------------- */

bool foc_sensor_pll_experiment_init(
    struct foc_sensor_pll_experiment *pll,
    float omega_rad_s,
    float max_speed_rad_s
)
{
    if (pll == NULL || !isfinite(omega_rad_s) || omega_rad_s < 100.0f || omega_rad_s > 2000.0f ||
        !isfinite(max_speed_rad_s) || max_speed_rad_s <= 0.0f || max_speed_rad_s > 4000.0f) {
        return false;
    }
    *pll = (struct foc_sensor_pll_experiment){
        .omega_rad_s = omega_rad_s,
        .max_speed_rad_s = max_speed_rad_s,
    };
    return true;
}

bool foc_sensor_pll_experiment_step(
    struct foc_sensor_pll_experiment *pll,
    const struct foc_encoder_feedback *sample
)
{
    if (pll == NULL || sample == NULL || !sample->valid || !sample->speed_valid ||
        sample->continuity_lost || sample->quality != FOC_ENCODER_QUALITY_VALID ||
        !isfinite(sample->dt_s) || sample->dt_s <= 0.0f || !isfinite(sample->delta_rad) ||
        fabsf(sample->delta_rad) >= PLL_PHASE_PI || !isfinite(sample->raw_velocity_rad_s) ||
        fabsf(sample->raw_velocity_rad_s) > pll->max_speed_rad_s || pll->omega_rad_s < 100.0f ||
        pll->max_speed_rad_s * sample->dt_s >= PLL_PHASE_PI) {
        return false;
    }
    const float x = pll->omega_rad_s * sample->dt_s;
    if (!isfinite(x) || x > 2.5f) {
        return false;
    }
    if (!pll->seeded) {
        pll->velocity_rad_s = sample->raw_velocity_rad_s;
        pll->phase_error_rad = 0.0f;
        pll->captured_us = sample->captured_us;
        pll->seeded = true;
        return true;
    }
    const uint32_t elapsed_us = sample->captured_us - pll->captured_us;
    const float linked_dt = (float)elapsed_us * 1.0e-6f;
    if (elapsed_us == 0U || elapsed_us >= UINT32_C(0x80000000) || linked_dt != sample->dt_s) {
        return false;
    }
    /* Encoder already selected the unwrapped short arc. Use that branch,
     * retaining the PLL phase only as a relative offset to this endpoint. */
    const float projected =
        pll->phase_error_rad + (pll->velocity_rad_s - sample->raw_velocity_rad_s) * sample->dt_s;
    if (!isfinite(projected) || fabsf(projected) >= PLL_PHASE_PI) {
        return false;
    }
    const float pole = 1.0f / (1.0f + x);
    const float phase = projected * pole * pole;
    const float velocity = pll->velocity_rad_s - pll->omega_rad_s * x * phase;
    if (!isfinite(velocity)) {
        return false;
    }
    const float limit = pll->max_speed_rad_s;
    pll->phase_error_rad = phase;
    pll->captured_us = sample->captured_us;
    pll->velocity_rad_s = velocity > limit ? limit : velocity < -limit ? -limit : velocity;
    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
