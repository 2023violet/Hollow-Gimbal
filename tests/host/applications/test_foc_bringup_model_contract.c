/**
 * @file    test_foc_bringup_model_contract.c
 * @brief   Host-only contracts for the FOC bring-up timing and ADC model
 * @version v1.0.0
 */

#include <stdio.h>
#include <stdlib.h>

#include "foc_config.h"

#define TEST_ASSERT(expr)                                                       \
    do {                                                                        \
        if (!(expr)) {                                                          \
            fprintf(stderr,                                                     \
                    "[test_foc_bringup_model_contract] assertion failed: %s\n", \
                    #expr);                                                     \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static void test_current_measurement_filter_has_loop_bandwidth_headroom(void)
{
    float filter_to_loop_ratio = DEFAULT_CUTOFF_HZ / DEFAULT_CURR_BW_HZ;

    TEST_ASSERT(filter_to_loop_ratio >= 4.0f);
    TEST_ASSERT(filter_to_loop_ratio <= 10.0f);
}

static void test_filter_sample_rate_matches_the_foc_step(void)
{
    TEST_ASSERT(CURR_LOOP_FREQ_HZ == 10000U);
    TEST_ASSERT(DEFAULT_SAMPLING_HZ == (float)CURR_LOOP_FREQ_HZ);
}

static void test_nominal_adc_scales_match_board_divider_and_gain(void)
{
    /* Hand-derived from 100 kΩ / 10 kΩ VBUS divider and 0.5 V/A CSA gain. */
    TEST_ASSERT((FAC_VBUS(1353.719f) > 11.99f) &&
                (FAC_VBUS(1353.719f) < 12.01f));
    TEST_ASSERT((FAC_CURR(124.0909f) > 0.199f) &&
                (FAC_CURR(124.0909f) < 0.201f));
}

static float abs_float(float value)
{
    return (value < 0.0f) ? -value : value;
}

static float simulate_current_step_peak(float target_a, float *final_a)
{
    const float two_pi = 6.283185307179586f;
    const float rs = 2.55f;
    const float ls = 0.00086f;
    const float dt = CURR_LOOP_TS;
    const float wc = two_pi * DEFAULT_CURR_BW_HZ;
    const float kp = ls * wc;
    const float ki = rs * wc;
    const float v_limit = DEFAULT_VOLT_RATED * 0.95f * FOC_INV_SQRT3;
    const float filter_wc = two_pi * DEFAULT_CUTOFF_HZ;
    const float alpha = filter_wc / (filter_wc + DEFAULT_SAMPLING_HZ);
    const uint32_t steps = 1000U; /* 100 ms at the 10 kHz current-loop step. */
    float current_a = 0.0f;
    float filtered_a = 0.0f;
    float integral_v = 0.0f;
    float peak_a = 0.0f;

    for (uint32_t step = 0U; step < steps; ++step) {
        float error_a = target_a - filtered_a;
        float voltage_v;

        integral_v += ki * error_a * dt;
        voltage_v = kp * error_a + integral_v;
        if (voltage_v > v_limit) {
            voltage_v = v_limit;
        } else if (voltage_v < -v_limit) {
            voltage_v = -v_limit;
        }

        /* First-order RL plant; this is a control-parameter stress model, not
         * a claim about the complete motor's mechanical dynamics. */
        current_a += ((voltage_v - rs * current_a) / ls) * dt;
        filtered_a += alpha * (current_a - filtered_a);

        if (abs_float(current_a) > peak_a) {
            peak_a = abs_float(current_a);
        }
    }

    *final_a = filtered_a;
    return peak_a;
}

static void test_default_filter_does_not_overdrive_a_small_current_step(void)
{
    float final_a = 0.0f;
    float peak_a = simulate_current_step_peak(0.10f, &final_a);

    /* The old 25 Hz filter made the same 0.10 A command overshoot to about
     * 0.33 A in this deterministic RL + PI + LPF stress model. */
    TEST_ASSERT(peak_a <= 0.15f);
    TEST_ASSERT((final_a > 0.09f) && (final_a < 0.11f));
}

int main(void)
{
    test_current_measurement_filter_has_loop_bandwidth_headroom();
    test_filter_sample_rate_matches_the_foc_step();
    test_nominal_adc_scales_match_board_divider_and_gain();
    test_default_filter_does_not_overdrive_a_small_current_step();

    puts("[test_foc_bringup_model_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
