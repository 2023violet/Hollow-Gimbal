/**
 * @file    foc_test_motor.h
 * @brief   Shared fixed-rotor R/L fixture for the Pipeline and Runtime tests.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Uses the SW01A fixed-angle ACB fixture and independent zero-order-hold
 * equations. ADC and PWM quantization stay on the test path.
 *
 * Usage example
 * -------------
 * Initialize the fixture, encode the current ADC ranks, run a control step,
 * and apply all three compare values only after a confirmed UPDATE.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The test owns each model instance. These inline functions are test-only.
 *
 * Failure / recovery
 * ------------------
 * Stop the model on rejection or INHIBIT. The fixture models no bridge-off
 * current response. Only accepted compares in 0 through 6000 are inputs.
 */
#pragma once

#include "foc_device.h"

#include <math.h>

/* Public Types ---------------------------------------------------------------- */

/** Fixed-angle plant with a 100 µs update interval. */
struct foc_test_motor {
    /** d-axis model current in amperes. */
    double id;
    /** q-axis model current in amperes. */
    double iq;
    /** Cosine of the fixed electrical angle of 0.37 rad. */
    double cosine;
    /** Sine of the fixed electrical angle of 0.37 rad. */
    double sine;
    /** Zero-order-hold decay for Rs=0.5 ohm and Ld=1 mH. */
    double decay_d;
    /** Zero-order-hold decay for Rs=0.5 ohm and Lq=1.5 mH. */
    double decay_q;
};

/* Public Functions ------------------------------------------------------------- */

/** Create the SW01A plant with zero initial currents. */
static inline struct foc_test_motor foc_test_motor_init(void)
{
    return (struct foc_test_motor){
        .cosine = cos(0.37),
        .sine = sin(0.37),
        .decay_d = exp(-0.5 * 0.0001 / 0.001),
        .decay_q = exp(-0.5 * 0.0001 / 0.0015),
    };
}

/** Encode ACB current ranks with a 2048-count offset and 1 mA/count. */
static inline void foc_test_motor_encode(const struct foc_test_motor *motor,
                                         struct foc_device_adc_raw *raw)
{
    const double sqrt3 = 1.73205080756887729353;
    const double alpha_i = motor->cosine * motor->id - motor->sine * motor->iq;
    const double beta_i = motor->sine * motor->id + motor->cosine * motor->iq;
    raw->rank2_raw = (uint16_t)lround(2048.0 + alpha_i * 1000.0);
    raw->rank3_raw = (uint16_t)lround(2048.0 + (-alpha_i - sqrt3 * beta_i) * 500.0);
    raw->rank4_raw = (uint16_t)lround(2048.0 + (-alpha_i + sqrt3 * beta_i) * 500.0);
}

/** Apply accepted CH1/2/3 compares with ACB mapping and return voltage magnitude in V. */
static inline double foc_test_motor_apply(struct foc_test_motor *motor, const uint32_t compare[3])
{
    const double sqrt3 = 1.73205080756887729353;
    const double a = compare[2] / 6000.0;
    const double b = compare[0] / 6000.0;
    const double c = compare[1] / 6000.0;
    const double alpha_v = (a - (a + b + c) / 3.0) * 12.0;
    const double beta_v = (b - c) * 12.0 / sqrt3;
    motor->id = motor->decay_d * motor->id +
        (1.0 - motor->decay_d) * (motor->cosine * alpha_v + motor->sine * beta_v) / 0.5;
    motor->iq = motor->decay_q * motor->iq +
        (1.0 - motor->decay_q) * (motor->cosine * beta_v - motor->sine * alpha_v) / 0.5;
    return hypot(alpha_v, beta_v);
}

/* ---------------------------------------- end of file ----------------------------------------- */
