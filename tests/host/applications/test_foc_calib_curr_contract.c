/**
 * @file    test_foc_calib_curr_contract.c
 * @brief   Host contract tests for startup current-offset calibration
 * @version v1.0.0
 * @date    2026.08.27
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foc_calib.h"
#include "foc_config.h"
#include "motor_drv.h"

#define TEST_ASSERT(expr)                                                        \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(stderr,                                                      \
                    "[test_foc_calib_curr_contract] assertion failed: %s\n",    \
                    #expr);                                                      \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

static int              g_pwm_enable_calls;
static int              g_pwm_disable_calls;
static int              g_after_offsets_calls;
static uint32_t         g_delay_ms_values[4];
static uint32_t         g_delay_ms_count;
static uint32_t         g_delay_us_calls;
static bool             g_mutate_samples;
static bool             g_advance_sample_sequence;
static struct motor_dev *g_calib_motor;

void pwm_out_enable(void)
{
    g_pwm_enable_calls++;
}

void pwm_out_disable(void)
{
    g_pwm_disable_calls++;
}

void core_time_delay_ms(uint32_t ms)
{
    TEST_ASSERT(g_delay_ms_count < (sizeof(g_delay_ms_values) / sizeof(g_delay_ms_values[0])));
    g_delay_ms_values[g_delay_ms_count++] = ms;
}

void core_time_delay_us(uint32_t us)
{
    (void)us;
    g_delay_us_calls++;

    if (g_advance_sample_sequence && (g_calib_motor != NULL)) {
        g_calib_motor->meas_data.injected_sample_seq++;
    }

    if (g_mutate_samples && (g_calib_motor != NULL)) {
        float sample = ((g_delay_us_calls & 0x01U) != 0U) ? 2000.0f : 2100.0f;
        g_calib_motor->meas_data.ia_raw = sample;
        g_calib_motor->meas_data.ib_raw = sample;
        g_calib_motor->meas_data.ic_raw = sample;
    }
}

void motor_calibration_runtime_after_current_offsets(struct motor_dev *motor)
{
    (void)motor;
    g_after_offsets_calls++;
}

void foc_para_calc(struct motor_dev *motor)
{
    (void)motor;
}

void foc_ctrl_volt(struct motor_dev *motor, float vd_set, float vq_set, float pos)
{
    (void)motor;
    (void)vd_set;
    (void)vq_set;
    (void)pos;
}

void foc_ctrl_curr(struct motor_dev *motor, float id_set, float iq_set, float pos)
{
    (void)motor;
    (void)id_set;
    (void)iq_set;
    (void)pos;
}

void foc_calib_enc(struct motor_dev *motor)
{
    (void)motor;
}

void foc_curr_gain_calc(struct motor_dev *motor)
{
    (void)motor;
}

void motor_calibration_runtime_after_flux_complete(struct motor_dev *motor)
{
    (void)motor;
}

void motor_calibration_runtime_after_js_complete(struct motor_dev *motor)
{
    (void)motor;
}

static void test_env_reset(struct motor_dev *motor)
{
    memset(motor, 0, sizeof(*motor));
    motor->hw_config.phase_order = PHASE_ORDER_ABC;
    g_pwm_enable_calls          = 0;
    g_pwm_disable_calls         = 0;
    g_after_offsets_calls       = 0;
    memset(g_delay_ms_values, 0, sizeof(g_delay_ms_values));
    g_delay_ms_count            = 0U;
    g_delay_us_calls            = 0U;
    g_mutate_samples            = false;
    g_advance_sample_sequence   = true;
    g_calib_motor               = motor;
}

static void test_stable_samples_are_applied_after_settle(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.meas_data.ia_raw = 2048.0f;
    motor.meas_data.ib_raw = 2050.0f;
    motor.meas_data.ic_raw = 2049.0f;

    foc_calib_curr(&motor);

    TEST_ASSERT(fabsf(motor.meas_data.ia_offset - 2048.0f) < 0.01f);
    TEST_ASSERT(fabsf(motor.meas_data.ib_offset - 2050.0f) < 0.01f);
    TEST_ASSERT(fabsf(motor.meas_data.ic_offset - 2049.0f) < 0.01f);
    TEST_ASSERT(motor.fault_code == FAULT_NONE);
    TEST_ASSERT(g_pwm_disable_calls == 1);
    TEST_ASSERT(g_pwm_enable_calls == 1);
    TEST_ASSERT(g_delay_ms_count == 2U);
    TEST_ASSERT(g_delay_ms_values[0] == 5U);
    TEST_ASSERT(g_delay_ms_values[1] == CALIB_CURR_SETTLE_MS);
    TEST_ASSERT(g_after_offsets_calls == 1);
    TEST_ASSERT(g_delay_us_calls == CALIB_CURR_DUR);
}

static void test_rail_offset_is_rejected_and_not_committed(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.meas_data.ia_raw = 0.0f;
    motor.meas_data.ib_raw = 0.0f;
    motor.meas_data.ic_raw = 0.0f;

    foc_calib_curr(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_CALIBRATION_FAILED) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 2);
    TEST_ASSERT(g_pwm_enable_calls == 1);
    TEST_ASSERT(g_after_offsets_calls == 0);
}

static void test_noisy_samples_are_rejected(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.meas_data.ia_raw = 2000.0f;
    motor.meas_data.ib_raw = 2000.0f;
    motor.meas_data.ic_raw = 2000.0f;
    g_mutate_samples      = true;

    foc_calib_curr(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_CALIBRATION_FAILED) != 0U);
    TEST_ASSERT(g_pwm_enable_calls == 1);
    TEST_ASSERT(g_after_offsets_calls == 0);
}

static void test_incoherent_phase_zero_offsets_are_rejected(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.meas_data.ia_raw = 2050.0f;
    motor.meas_data.ib_raw = 2100.0f;
    motor.meas_data.ic_raw = 2200.0f;

    foc_calib_curr(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_CALIBRATION_FAILED) != 0U);
    TEST_ASSERT(g_after_offsets_calls == 0);
}

static void test_stalled_injected_adc_sequence_is_rejected(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.meas_data.ia_raw = 2050.0f;
    motor.meas_data.ib_raw = 2051.0f;
    motor.meas_data.ic_raw = 2052.0f;
    g_advance_sample_sequence = false;

    foc_calib_curr(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_CALIBRATION_FAILED) != 0U);
    TEST_ASSERT(g_after_offsets_calls == 0);
}

static void test_off_center_zero_offsets_are_rejected(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.meas_data.ia_raw = 2300.0f;
    motor.meas_data.ib_raw = 2301.0f;
    motor.meas_data.ic_raw = 2302.0f;

    foc_calib_curr(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_CALIBRATION_FAILED) != 0U);
    TEST_ASSERT(g_after_offsets_calls == 0);
}

int main(void)
{
    test_stable_samples_are_applied_after_settle();
    test_rail_offset_is_rejected_and_not_committed();
    test_noisy_samples_are_rejected();
    test_incoherent_phase_zero_offsets_are_rejected();
    test_stalled_injected_adc_sequence_is_rejected();
    test_off_center_zero_offsets_are_rejected();

    puts("[test_foc_calib_curr_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
