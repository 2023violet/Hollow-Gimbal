/**
 * @file    test_foc_voltage_openloop_model_contract.c
 * @brief   Host-only voltage open-loop and SVPWM model contracts
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../applications/algo/foc_config.h"
#include "../../../applications/periph/foc_ctrl.h"
#include "foc_core.h"

#define TEST_ASSERT(expr)                                                               \
    do {                                                                                \
        if (!(expr)) {                                                                  \
            fprintf(stderr,                                                             \
                    "[test_foc_voltage_openloop_model_contract] assertion failed: %s\n", \
                    #expr);                                                             \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

static uint32_t g_pwm_set_count     = 0U;
static uint32_t g_pwm_disable_count = 0U;
static uint32_t g_sqrt_calls        = 0U;

static float test_sqrt_reference(float value)
{
    double guess = (value > 1.0f) ? (double)value : 1.0;

    if (value == 0.0f) {
        return 0.0f;
    }

    for (uint32_t i = 0U; i < 16U; ++i) {
        guess = 0.5 * (guess + ((double)value / guess));
    }

    return (float)guess;
}

float sqrtf(float value)
{
    g_sqrt_calls++;
    return test_sqrt_reference(value);
}

void pwm_out_set_duty(struct motor_dev *motor)
{
    (void)motor;
    g_pwm_set_count++;
}

void pwm_out_disable(void)
{
    g_pwm_disable_count++;
}

static void motor_model_init(struct motor_dev *motor, struct encoder_config *enc_cfg)
{
    struct pid_config pid_cfg = {
        .mode           = PID_POSITION,
        .kp             = 1.0f,
        .ki             = 0.0f,
        .kd             = 0.0f,
        .max_output     = 10.0f,
        .integral_limit = 10.0f,
        .dead_band      = 0.0f,
        .output_lpf_hz  = 0.0f,
        .deriv_lpf_hz   = 0.0f,
        .improve        = PID_INTEGRAL_LIMIT,
    };

    memset(motor, 0, sizeof(*motor));
    memset(enc_cfg, 0, sizeof(*enc_cfg));
    enc_cfg->pole_pairs = DEFAULT_POLE_PAIRS;
    motor->enc_pri.config = enc_cfg;
    motor->hw_config.gear_ratio = 1.0f;
    motor->prot_config.i_rated = DEFAULT_CURR_RATED;
    motor->meas_data.v_bus = DEFAULT_VOLT_RATED;
    motor->ctrl_mode = CTRL_MODE_VOLT;
    motor->ctrl_state.vd_set = 1.0f;
    motor->ctrl_state.vq_set = 0.0f;
    motor->ctrl_state.if_enable = 0U;
    motor->ctrl_state.id_filter.alpha = 0.1f;
    motor->ctrl_state.iq_filter.alpha = 0.1f;

    TEST_ASSERT(pid_init(&motor->ctrl_config.id_pid, &pid_cfg) == 0);
    TEST_ASSERT(pid_init(&motor->ctrl_config.iq_pid, &pid_cfg) == 0);
    motor->ctrl_config.id_pid.output = 12.345f;
    motor->ctrl_config.iq_pid.output = -6.789f;
}

#if (!defined(TM_BENCH_FIXED_DUTY_TEST) || (TM_BENCH_FIXED_DUTY_TEST == 0U)) && \
    (!defined(TM_BENCH_RAW_SIXSTEP_TEST) || (TM_BENCH_RAW_SIXSTEP_TEST == 0U)) && \
    (!defined(TM_BENCH_RAW_SINE_TEST) || (TM_BENCH_RAW_SINE_TEST == 0U)) && \
    (!defined(TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST) || \
     (TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST == 0U)) && \
    (!defined(TM_BENCH_RAW_SINE_20_TEST) || (TM_BENCH_RAW_SINE_20_TEST == 0U))
static void test_voltage_mode_zero_speed_holds_angle(void)
{
    struct motor_dev      motor;
    struct encoder_config enc_cfg;

    motor_model_init(&motor, &enc_cfg);
    motor.ctrl_state.theta_sim = 1.2345f;
    motor.ctrl_state.we_volt_ramp = 0.0f;

    foc_current_loop_step(&motor);

    TEST_ASSERT(fabsf(motor.ctrl_state.theta_sim - 1.2345f) < 1.0e-6f);
    TEST_ASSERT(motor.ctrl_state.we_volt_now == 0.0f);
}

static void test_voltage_mode_ramps_angle_and_bypasses_current_pid(void)
{
    struct motor_dev      motor;
    struct encoder_config enc_cfg;
    const float            target_we = 62.83f;
    /* Derive the ramp step count from the active acceleration constant so the
     * contract tracks DEFAULT_VOLT_WE_ACCEL instead of a stale hardcoded
     * step count. +200 steps of margin lets the ramp fully settle. */
    const uint32_t         steps =
        (uint32_t)((target_we / DEFAULT_VOLT_WE_ACCEL) * 10000.0f) + 200U;

    motor_model_init(&motor, &enc_cfg);
    g_pwm_set_count = 0U;
    motor.ctrl_state.we_volt_ramp = target_we;

    for (uint32_t i = 0U; i < steps; ++i) {
        foc_current_loop_step(&motor);
        TEST_ASSERT(isfinite(motor.ctrl_state.theta_sim));
        TEST_ASSERT(motor.ctrl_state.theta_sim >= 0.0f);
        TEST_ASSERT(motor.ctrl_state.theta_sim < FOC_2PI);
        TEST_ASSERT(isfinite(motor.foc_calc.dtc_a));
        TEST_ASSERT(isfinite(motor.foc_calc.dtc_b));
        TEST_ASSERT(isfinite(motor.foc_calc.dtc_c));
        TEST_ASSERT(motor.foc_calc.dtc_a >= 0.0f && motor.foc_calc.dtc_a <= 1.0f);
        TEST_ASSERT(motor.foc_calc.dtc_b >= 0.0f && motor.foc_calc.dtc_b <= 1.0f);
        TEST_ASSERT(motor.foc_calc.dtc_c >= 0.0f && motor.foc_calc.dtc_c <= 1.0f);
    }

    TEST_ASSERT(fabsf(motor.ctrl_state.we_volt_now - target_we) < 0.01f);
    TEST_ASSERT(motor.ctrl_config.id_pid.output == 12.345f);
    TEST_ASSERT(motor.ctrl_config.iq_pid.output == -6.789f);
    TEST_ASSERT(g_pwm_set_count == steps);
}

static void test_i_f_mode_uses_the_same_simulated_angle_and_inner_loop(void)
{
    struct motor_dev      motor;
    struct encoder_config enc_cfg;

    motor_model_init(&motor, &enc_cfg);
    motor.ctrl_state.if_enable = 1U;
    motor.ctrl_state.if_iq_target = 0.05f;
    motor.ctrl_state.we_volt_ramp = 0.0f;
    motor.ctrl_config.id_pid.output = 0.0f;
    motor.ctrl_config.iq_pid.output = 0.0f;

    foc_current_loop_step(&motor);

    TEST_ASSERT(motor.foc_calc.theta == motor.ctrl_state.theta_sim);
    TEST_ASSERT(motor.ctrl_config.iq_pid.output > 0.0f);
    TEST_ASSERT(g_pwm_set_count > 0U);
}

static void test_invalid_vbus_disables_voltage_output(void)
{
    struct motor_dev      motor;
    struct encoder_config enc_cfg;

    motor_model_init(&motor, &enc_cfg);
    motor.meas_data.v_bus = 0.0f;
    g_pwm_disable_count = 0U;

    foc_current_loop_step(&motor);

    TEST_ASSERT(motor.foc_calc.inv_vbus == 0.0f);
    TEST_ASSERT(motor.foc_calc.dtc_a == 0.0f);
    TEST_ASSERT(motor.foc_calc.dtc_b == 0.0f);
    TEST_ASSERT(motor.foc_calc.dtc_c == 0.0f);
    TEST_ASSERT(g_pwm_disable_count == 1U);
}

static void test_svpwm_clamps_a_large_valid_vector(void)
{
    struct foc_vars foc = {
        .v_alpha  = 20.0f,
        .v_beta   = -10.0f,
        .inv_vbus = 1.0f / DEFAULT_VOLT_RATED,
    };

    TEST_ASSERT(foc_svpwm(&foc) == 0);
    TEST_ASSERT(isfinite(foc.dtc_a) && isfinite(foc.dtc_b) && isfinite(foc.dtc_c));
    TEST_ASSERT(foc.dtc_a >= 0.0f && foc.dtc_a <= 1.0f);
    TEST_ASSERT(foc.dtc_b >= 0.0f && foc.dtc_b <= 1.0f);
    TEST_ASSERT(foc.dtc_c >= 0.0f && foc.dtc_c <= 1.0f);
    TEST_ASSERT((foc.v_alpha * foc.v_alpha + foc.v_beta * foc.v_beta) <=
                (FOC_TWO_THIRDS * FOC_TWO_THIRDS + 1.0e-5f));
}

static void test_svpwm_skips_sqrt_for_a_low_unsaturated_vector(void)
{
    struct foc_vars foc = {
        .v_alpha  = 0.10f,
        .v_beta   = -0.05f,
        .inv_vbus = 1.0f / DEFAULT_VOLT_RATED,
    };

    g_sqrt_calls = 0U;
    TEST_ASSERT(foc_svpwm(&foc) == 0);
    TEST_ASSERT(g_sqrt_calls == 0U);
}

static void test_svpwm_preserves_alpha_beta_vector(void)
{
    static const float vectors[][2] = {
        { 0.20f,  0.05f},
        { 0.05f,  0.20f},
        {-0.20f,  0.05f},
        {-0.20f, -0.05f},
        {-0.05f, -0.20f},
        { 0.20f, -0.05f},
    };

    for (size_t i = 0U; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        struct foc_vars foc = {
            .v_alpha  = vectors[i][0],
            .v_beta   = vectors[i][1],
            .inv_vbus = 1.0f,
        };
        float output_alpha;
        float output_beta;

        TEST_ASSERT(foc_svpwm(&foc) == 0);

        output_alpha = FOC_TWO_THIRDS *
                       (foc.dtc_a - 0.5f * (foc.dtc_b + foc.dtc_c));
        output_beta  = (foc.dtc_b - foc.dtc_c) * FOC_INV_SQRT3;

        TEST_ASSERT(fabsf(output_alpha - vectors[i][0]) < 1.0e-5f);
        TEST_ASSERT(fabsf(output_beta - vectors[i][1]) < 1.0e-5f);
    }
}
#endif

#if defined(TM_BENCH_FIXED_DUTY_TEST) && (TM_BENCH_FIXED_DUTY_TEST != 0U)
static void test_bench_fixed_duty_bypasses_all_control_paths(void)
{
    struct motor_dev      motor;
    struct encoder_config enc_cfg;

    motor_model_init(&motor, &enc_cfg);
    motor.ctrl_state.theta_sim    = 2.345f;
    motor.ctrl_state.we_volt_ramp = 123.0f;
    motor.ctrl_state.vd_set       = 4.0f;
    motor.ctrl_state.vq_set       = -4.0f;
    g_pwm_set_count = 0U;

    foc_current_loop_step(&motor);

    TEST_ASSERT(motor.foc_calc.dtc_a == 0.40f);
    TEST_ASSERT(motor.foc_calc.dtc_b == 0.50f);
    TEST_ASSERT(motor.foc_calc.dtc_c == 0.60f);
    TEST_ASSERT(g_pwm_set_count == 1U);
}
#endif

#if defined(TM_BENCH_RAW_SIXSTEP_TEST) && (TM_BENCH_RAW_SIXSTEP_TEST != 0U)
static void test_bench_raw_sixstep_advances_one_vector_per_sixth_cycle(void)
{
    struct motor_dev      motor;
    struct encoder_config enc_cfg;
    const uint32_t        ticks_per_state = PWM_FREQ_HZ / 6U;

    motor_model_init(&motor, &enc_cfg);
    g_pwm_set_count = 0U;

    for (uint32_t tick = 0U; tick < ticks_per_state; ++tick) {
        foc_current_loop_step(&motor);
        TEST_ASSERT(motor.foc_calc.dtc_a == 0.60f);
        TEST_ASSERT(motor.foc_calc.dtc_b == 0.40f);
        TEST_ASSERT(motor.foc_calc.dtc_c == 0.50f);
    }

    foc_current_loop_step(&motor);
    TEST_ASSERT(motor.foc_calc.dtc_a == 0.60f);
    TEST_ASSERT(motor.foc_calc.dtc_b == 0.50f);
    TEST_ASSERT(motor.foc_calc.dtc_c == 0.40f);
    TEST_ASSERT(g_pwm_set_count == (ticks_per_state + 1U));
}
#endif

#if (defined(TM_BENCH_RAW_SINE_TEST) && (TM_BENCH_RAW_SINE_TEST != 0U)) || \
    (defined(TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST) && \
     (TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST != 0U)) || \
    (defined(TM_BENCH_RAW_SINE_20_TEST) && (TM_BENCH_RAW_SINE_20_TEST != 0U))
static void test_bench_raw_sine_tracks_one_electrical_hz(void)
{
    struct motor_dev      motor;
    struct encoder_config enc_cfg;

    motor_model_init(&motor, &enc_cfg);
    g_pwm_set_count = 0U;

    foc_current_loop_step(&motor);
#if defined(TM_BENCH_RAW_SINE_20_TEST) && (TM_BENCH_RAW_SINE_20_TEST != 0U)
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_a - 0.50f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_b - 0.326795f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_c - 0.673205f) < 1.0e-5f);
#elif defined(TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST) && \
      (TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST != 0U)
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_a - 0.50f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_b - 0.40f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_c - 0.60f) < 1.0e-5f);
#else
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_a - 0.50f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_b - 0.413397f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_c - 0.586603f) < 1.0e-5f);
#endif

    for (uint32_t tick = 1U; tick < (PWM_FREQ_HZ / 4U); ++tick) {
        foc_current_loop_step(&motor);
    }

    foc_current_loop_step(&motor);
#if defined(TM_BENCH_RAW_SINE_20_TEST) && (TM_BENCH_RAW_SINE_20_TEST != 0U)
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_a - 0.70f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_b - 0.40f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_c - 0.40f) < 1.0e-5f);
#elif defined(TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST) && \
      (TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST != 0U)
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_a - 0.615470f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_b - 0.442265f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_c - 0.442265f) < 1.0e-5f);
#else
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_a - 0.60f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_b - 0.45f) < 1.0e-5f);
    TEST_ASSERT(fabsf(motor.foc_calc.dtc_c - 0.45f) < 1.0e-5f);
#endif
    TEST_ASSERT(g_pwm_set_count == ((PWM_FREQ_HZ / 4U) + 1U));
}
#endif

int main(void)
{
#if defined(TM_BENCH_FIXED_DUTY_TEST) && (TM_BENCH_FIXED_DUTY_TEST != 0U)
    test_bench_fixed_duty_bypasses_all_control_paths();
#elif defined(TM_BENCH_RAW_SIXSTEP_TEST) && (TM_BENCH_RAW_SIXSTEP_TEST != 0U)
    test_bench_raw_sixstep_advances_one_vector_per_sixth_cycle();
#elif defined(TM_BENCH_RAW_SINE_TEST) && (TM_BENCH_RAW_SINE_TEST != 0U)
    test_bench_raw_sine_tracks_one_electrical_hz();
#elif defined(TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST) && \
    (TM_BENCH_RAW_SINE_MATCH_SIXSTEP_TEST != 0U)
    test_bench_raw_sine_tracks_one_electrical_hz();
#elif defined(TM_BENCH_RAW_SINE_20_TEST) && (TM_BENCH_RAW_SINE_20_TEST != 0U)
    test_bench_raw_sine_tracks_one_electrical_hz();
#else
    test_voltage_mode_zero_speed_holds_angle();
    test_voltage_mode_ramps_angle_and_bypasses_current_pid();
    test_i_f_mode_uses_the_same_simulated_angle_and_inner_loop();
    test_invalid_vbus_disables_voltage_output();
    test_svpwm_clamps_a_large_valid_vector();
    test_svpwm_skips_sqrt_for_a_low_unsaturated_vector();
    test_svpwm_preserves_alpha_beta_vector();
#endif
    puts("[test_foc_voltage_openloop_model_contract] passed");
    return 0;
}
