/**
 * @file    foc_thermal.h
 * @brief   Thermal protection math: NTC conversion, filtered temperature, linear
 *          derating and the I²t overload state.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Design overview
 * ---------------
 * Pure calculations shared by the board and the Runtime. An NTC sensor turns a
 * divider ratio into degrees Celsius with the β equation, filters it in two
 * first-order stages, derates the current linearly from derate_c to limit_c and
 * trips FOC_THERMAL_TRIP_MARGIN_C above limit_c (or after FOC_THERMAL_INVALID_SAMPLES
 * unusable readings, a broken or shorted sensor). The trip releases below limit_c.
 * The I²t state θ follows dθ/dt = (i² / I_rated² − θ) / τ: a current held at the
 * rated value settles at θ = 1, the level where the caller folds the current back
 * to the rated value; FOC_THERMAL_I2T_TRIP marks an overload the fold-back did not
 * contain.
 *
 * Usage example
 * -------------
 * Board: validate the NTC model and sensor configuration, reset the sensor, then
 * call foc_thermal_sensor_update() at a fixed poll rate and pass
 * foc_thermal_derate() and the trip flag to the Runtime. Runtime: prepare the I²t
 * coefficients from the rated current and τ, then call foc_thermal_i2t_step() once
 * per control cycle with the squared current magnitude.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns configuration and state and serializes calls on one state.
 * Functions are non-blocking, allocate nothing and use no libm.
 *
 * Failure / recovery
 * ------------------
 * An unusable reading yields NaN and counts toward the trip; an enabled sensor
 * without a reading derates fully. A disabled sensor or I²t reports no limit.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros ---------------------------------------------------------------- */

/** Trip margin above a sensor's limit, in °C. */
#define FOC_THERMAL_TRIP_MARGIN_C   5.0f
/** Consecutive unusable readings of an enabled sensor that trip it. */
#define FOC_THERMAL_INVALID_SAMPLES 10U
/** θ at which the caller folds the current back to the rated value. */
#define FOC_THERMAL_I2T_FOLDBACK    1.0f
/** θ that latches an overload. */
#define FOC_THERMAL_I2T_TRIP        1.25f
/** Divider ratios outside [MIN, 1 − MIN] are unusable (open or shorted sensor). */
#define FOC_THERMAL_RATIO_MIN       0.005f

/* Public Types ----------------------------------------------------------------- */

/** Divider topology; the ratio is the ADC reading over full scale. */
enum foc_thermal_topology {
    /** Pull-up from the ADC reference, NTC to ground. */
    FOC_THERMAL_NTC_TO_GROUND = 0,
    /** NTC from the ADC reference, pull-down to ground. */
    FOC_THERMAL_NTC_TO_SUPPLY = 1,
};

/** NTC β model shared by the sensors of one board. */
struct foc_thermal_ntc {
    /** β in K, resistance at 25 °C and the fixed divider resistor in Ω. */
    float beta_k;
    float r25_ohm;
    float pullup_ohm;
    enum foc_thermal_topology topology;
};

/** One sensor's derating window in °C. */
struct foc_thermal_sensor_config {
    bool enabled;
    /** Derating starts at derate_c and reaches zero current at limit_c. */
    float derate_c;
    float limit_c;
};

/** Filter and trip state of one sensor. */
struct foc_thermal_sensor {
    float stage_c;
    float filtered_c;
    uint32_t invalid_samples;
    bool seeded;
    bool tripped;
};

/** Prepared I²t coefficients; a zero rate disables the model. */
struct foc_thermal_i2t {
    float rated_a;
    float inv_rated_sq;
    float rate_per_us;
};

/* Public Functions ------------------------------------------------------------- */

/** True for a β model with finite values in range and a known topology. */
bool foc_thermal_ntc_valid(const struct foc_thermal_ntc *ntc);

/** True for a disabled sensor or an ordered window within −40..150 °C. */
bool foc_thermal_sensor_config_valid(const struct foc_thermal_sensor_config *config);

/**
 * Convert a divider ratio to °C.
 *
 * @param ntc Valid β model.
 * @param ratio ADC reading over full scale.
 * @return Temperature in °C, or NaN for an unusable ratio.
 */
float foc_thermal_ntc_celsius(const struct foc_thermal_ntc *ntc, float ratio);

/** Forget the filter and the trip. */
void foc_thermal_sensor_reset(struct foc_thermal_sensor *sensor);

/**
 * Process one poll.
 *
 * @param config Valid sensor configuration.
 * @param ntc Valid β model.
 * @param sensor State, updated by this call; reset while the sensor is disabled.
 * @param sampled False when no reading was available this poll.
 * @param ratio ADC reading over full scale.
 * @param alpha Per-stage filter coefficient in (0, 1].
 * @return Filtered temperature in °C, or NaN without a usable reading.
 */
float foc_thermal_sensor_update(const struct foc_thermal_sensor_config *config,
                                const struct foc_thermal_ntc *ntc,
                                struct foc_thermal_sensor *sensor,
                                bool sampled,
                                float ratio,
                                float alpha);

/**
 * Share of the current to remove, 0 (none) to 1 (all), linear across the window.
 * An enabled sensor without a temperature (NaN) derates fully; a disabled one not at all.
 */
float foc_thermal_derate(const struct foc_thermal_sensor_config *config, float celsius);

/**
 * Prepare the I²t coefficients.
 *
 * @param i2t Coefficients, cleared when the model is off.
 * @param rated_a Rated current in A; 0 turns the model off.
 * @param tau_s Thermal time constant in s; 0 turns the model off.
 * @return True when the model is on.
 */
bool foc_thermal_i2t_prepare(struct foc_thermal_i2t *i2t, float rated_a, float tau_s);

/**
 * Advance θ by dt_us with the squared current magnitude in A².
 *
 * @return The new θ; 0 when the model is off.
 */
float foc_thermal_i2t_step(const struct foc_thermal_i2t *i2t,
                           float theta,
                           float current_sq,
                           uint32_t dt_us);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
