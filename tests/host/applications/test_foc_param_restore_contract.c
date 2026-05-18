/**
 * @file    test_foc_param_restore_contract.c
 * @brief   Host regression tests for FalconFoc parameter restore contract
 * @version v1.0.0
 * @date    2026.05.14
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_flash.h"
#include "foc_param.h"
#include "foc_restore.h"
#include "foc_math.h"

#define TEST_ASSERT(expr)                                                                  \
    do {                                                                                   \
        if (!(expr)) {                                                                     \
            fprintf(stderr,                                                                \
                    "[test_foc_param_restore_contract] assertion failed: %s (%s:%d)\n",   \
                    #expr,                                                                 \
                    __FILE__,                                                              \
                    __LINE__);                                                             \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

#define TEST_ASSERT_NEAR(actual, expected, eps)                                            \
    do {                                                                                   \
        if (fabs((double)((actual) - (expected))) > (double)(eps)) {                      \
            fprintf(stderr,                                                                \
                    "[test_foc_param_restore_contract] near assertion failed: %s ~= %s "   \
                    "(actual=%f expected=%f eps=%f) (%s:%d)\n",                            \
                    #actual,                                                               \
                    #expected,                                                             \
                    (double)(actual),                                                      \
                    (double)(expected),                                                    \
                    (double)(eps),                                                         \
                    __FILE__,                                                              \
                    __LINE__);                                                             \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

static uint8_t g_flash[BSP_FLASH_SIZE];

static void test_flash_reset(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
}

int bsp_flash_read(uint32_t addr, uint8_t *buf, uint32_t size)
{
    uint32_t offset;

    if ((buf == NULL) || (addr < BSP_FLASH_BASE_ADDR)) {
        return -1;
    }

    offset = addr - BSP_FLASH_BASE_ADDR;
    if ((offset + size) > sizeof(g_flash)) {
        return -1;
    }

    memcpy(buf, &g_flash[offset], size);
    return (int)size;
}

int bsp_flash_write(uint32_t addr, const uint8_t *buf, uint32_t size)
{
    uint32_t offset;

    if ((buf == NULL) || (addr < BSP_FLASH_BASE_ADDR)) {
        return -1;
    }

    offset = addr - BSP_FLASH_BASE_ADDR;
    if ((offset + size) > sizeof(g_flash)) {
        return -1;
    }

    memcpy(&g_flash[offset], buf, size);
    return (int)size;
}

int bsp_flash_erase(uint32_t addr, uint32_t size)
{
    uint32_t offset;

    if (addr < BSP_FLASH_BASE_ADDR) {
        return -1;
    }

    offset = addr - BSP_FLASH_BASE_ADDR;
    if ((offset + size) > sizeof(g_flash)) {
        return -1;
    }

    memset(&g_flash[offset], 0xFF, size);
    return 0;
}

bool bsp_flash_is_empty(uint32_t addr, uint32_t size)
{
    uint32_t offset;
    uint32_t i;

    if (addr < BSP_FLASH_BASE_ADDR) {
        return false;
    }

    offset = addr - BSP_FLASH_BASE_ADDR;
    if ((offset + size) > sizeof(g_flash)) {
        return false;
    }

    for (i = 0U; i < size; ++i) {
        if (g_flash[offset + i] != 0xFFU) {
            return false;
        }
    }

    return true;
}

void encoder_init(struct encoder_dev *enc, struct encoder_config *cfg)
{
    if (enc == NULL) {
        return;
    }

    memset(enc, 0, sizeof(*enc));
    enc->config = cfg;
}

void encoder_update(struct encoder_dev *enc)
{
    (void)enc;
}

void foc_calib_runtime_refresh(struct motor_dev *motor)
{
    float torque_const;
    float tor_motor_needed;
    float iq_limit_by_mech;
    float iq_limit_by_heat;

    TEST_ASSERT(motor != NULL);

    motor->calc_params.inv_poles = 1.0f / (float)motor->hw_config.poles;
    motor->calc_params.inv_gear  = 1.0f / motor->hw_config.gear_ratio;
    motor->calc_params.poles_over_2pi = (float)motor->hw_config.poles / FOC_2PI;

    torque_const            = 1.5f * (float)motor->hw_config.poles * motor->hw_config.flux;
    motor->calc_params.kt   = torque_const;
    motor->calc_params.inv_kt = (torque_const > 1e-6f) ? (1.0f / torque_const) : 1.0f;

    tor_motor_needed =
        motor->prot_config.tor_peak / (motor->hw_config.gear_ratio * motor->hw_config.gear_eff);
    iq_limit_by_mech = tor_motor_needed * motor->calc_params.inv_kt;
    iq_limit_by_heat = motor->prot_config.i_peak;

    motor->ctrl_state.iq_max  = FOC_MIN(iq_limit_by_mech, iq_limit_by_heat);
    motor->ctrl_state.id_max  = motor->ctrl_state.iq_max;
    motor->ctrl_state.tor_max = motor->ctrl_state.iq_max * motor->calc_params.kt *
                                motor->hw_config.gear_ratio * motor->hw_config.gear_eff;
    motor->ctrl_state.vel_max = motor->prot_config.vel_peak;

    motor->ctrl_config.vel_pid.config.max_output     = motor->prot_config.i_rated;
    motor->ctrl_config.vel_pid.config.integral_limit = motor->prot_config.i_rated;
    motor->ctrl_config.pos_pid.config.max_output     = motor->ctrl_state.vel_max;
    motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.vel_max;
}

static void test_env_reset(void)
{
    test_flash_reset();
}

static void test_sys_invalid_enum_falls_back_to_safe_defaults(void)
{
    struct motor_dev saved;
    struct motor_dev loaded;

    test_env_reset();
    memset(&saved, 0, sizeof(saved));
    memset(&loaded, 0, sizeof(loaded));

    saved.ctrl_mode  = (enum foc_ctrl_mode)99;
    saved.input_mode = (enum foc_input_mode)88;

    TEST_ASSERT(foc_param_save_sys(&saved) == PARAM_OK);
    TEST_ASSERT(foc_param_load_sys(&loaded) == PARAM_OK);
    TEST_ASSERT(loaded.ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(loaded.input_mode == INPUT_MODE_INACTIVE);
}

static void test_restore_transient_clear_preserves_persisted_input_mode(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));

    motor.ctrl_mode  = CTRL_MODE_VEL;
    motor.input_mode = INPUT_MODE_TRAP_TRAJ;

    motor.meas_data.out_pos_rad = 12.5f;
    motor.ctrl_state.iq_max     = 6.0f;
    motor.ctrl_state.vel_max    = 18.0f;
    motor.ctrl_state.target_vel = 100.0f;
    motor.ctrl_state.target_iq  = 5.0f;
    motor.ctrl_state.target_id  = 1.0f;
    motor.ctrl_state.target_pos = 99.0f;
    motor.ctrl_state.target_tor = 3.0f;
    motor.ctrl_state.vel_set    = 88.0f;
    motor.ctrl_state.iq_set     = 4.0f;
    motor.ctrl_state.id_set     = 2.0f;
    motor.ctrl_state.pos_set    = 77.0f;
    motor.ctrl_state.vel_ff     = 6.0f;
    motor.ctrl_state.iq_ff      = 7.0f;

    motor.ctrl_config.pos_pid.config.max_output     = 1.0f;
    motor.ctrl_config.pos_pid.config.integral_limit = 1.0f;
    motor.ctrl_config.vel_pid.config.max_output     = 2.0f;
    motor.ctrl_config.vel_pid.config.integral_limit = 2.0f;

    foc_restore_clear_transient_setpoints(&motor);

    TEST_ASSERT(motor.ctrl_mode == CTRL_MODE_VEL);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_TRAP_TRAJ);
    TEST_ASSERT_NEAR(motor.ctrl_state.target_vel, 0.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.target_iq, 0.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.target_id, 0.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.target_pos, 12.5f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.target_tor, 0.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.vel_set, 0.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.iq_set, 0.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.id_set, 0.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.pos_set, 12.5f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.vel_ff, 0.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.iq_ff, 0.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.dyn_iq_limit, 6.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_state.dyn_vel_limit, 18.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_config.pos_pid.config.max_output, 18.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_config.pos_pid.config.integral_limit, 18.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_config.vel_pid.config.max_output, 6.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_config.vel_pid.config.integral_limit, 6.0f, 1e-6f);
}

static void test_restore_finalize_rebinds_traj_and_runtime_limits(void)
{
    struct motor_dev motor;
    struct encoder_config enc_cfg;

    test_env_reset();
    memset(&motor, 0, sizeof(motor));
    memset(&enc_cfg, 0, sizeof(enc_cfg));

    motor.enc_pri.config = &enc_cfg;

    motor.hw_config.poles      = 7.0f;
    motor.hw_config.rs         = 0.25f;
    motor.hw_config.ls         = 0.00031f;
    motor.hw_config.flux       = 0.0081f;
    motor.hw_config.gear_ratio = 6.0f;
    motor.hw_config.gear_eff   = 0.85f;
    motor.prot_config.i_peak   = 13.0f;
    motor.prot_config.i_rated  = 4.2f;
    motor.prot_config.tor_peak = 8.5f;
    motor.prot_config.vel_peak = 32.0f;

    motor.ctrl_config.traj_vel       = 17.0f;
    motor.ctrl_config.traj_accel     = 23.0f;
    motor.ctrl_config.traj_decel     = 19.0f;
    motor.ctrl_config.curr_bandwidth = 1200.0f;

    motor.ctrl_config.vel_pid.config.kp = 5.5f;
    motor.ctrl_config.vel_pid.config.ki = 6.5f;
    motor.ctrl_config.pos_pid.config.kp = 7.5f;
    motor.ctrl_config.pos_pid.config.ki = 8.5f;

    motor.traj.config.max_vel = 1.0f;
    motor.traj.config.max_acc = 2.0f;
    motor.traj.config.max_dec = 3.0f;

    foc_restore_finalize(&motor);

    TEST_ASSERT_NEAR(motor.traj.config.max_vel, 17.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.traj.config.max_acc, 23.0f, 1e-6f);
    TEST_ASSERT_NEAR(motor.traj.config.max_dec, 19.0f, 1e-6f);

    TEST_ASSERT(motor.ctrl_state.iq_max > 0.0f);
    TEST_ASSERT(motor.ctrl_state.vel_max == motor.prot_config.vel_peak);
    TEST_ASSERT(motor.ctrl_state.dyn_iq_limit == motor.ctrl_state.iq_max);
    TEST_ASSERT(motor.ctrl_state.dyn_vel_limit == motor.ctrl_state.vel_max);
    TEST_ASSERT(motor.ctrl_config.vel_pid.config.max_output == motor.prot_config.i_rated);
    TEST_ASSERT(motor.ctrl_config.vel_pid.config.integral_limit == motor.prot_config.i_rated);
    TEST_ASSERT(motor.ctrl_config.pos_pid.config.max_output == motor.ctrl_state.vel_max);
    TEST_ASSERT(motor.ctrl_config.pos_pid.config.integral_limit == motor.ctrl_state.vel_max);

    /*
     * Restored persisted PID tuning must remain authoritative.
     * finalize() may refresh runtime ceilings, but not Kp/Ki values.
     */
    TEST_ASSERT_NEAR(motor.ctrl_config.vel_pid.config.kp, 5.5f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_config.vel_pid.config.ki, 6.5f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_config.pos_pid.config.kp, 7.5f, 1e-6f);
    TEST_ASSERT_NEAR(motor.ctrl_config.pos_pid.config.ki, 8.5f, 1e-6f);
}

static void test_load_motor_then_finalize_preserves_tuning_and_refreshes_runtime(void)
{
    struct motor_dev saved;
    struct motor_dev loaded;
    struct encoder_config saved_enc_cfg;
    struct encoder_config loaded_enc_cfg;

    test_env_reset();
    memset(&saved, 0, sizeof(saved));
    memset(&loaded, 0, sizeof(loaded));
    memset(&saved_enc_cfg, 0, sizeof(saved_enc_cfg));
    memset(&loaded_enc_cfg, 0, sizeof(loaded_enc_cfg));

    saved.enc_pri.config = &saved_enc_cfg;
    loaded.enc_pri.config = &loaded_enc_cfg;

    saved.hw_config.poles      = 11.0f;
    saved.hw_config.rs         = 0.18f;
    saved.hw_config.ls         = 0.00022f;
    saved.hw_config.flux       = 0.0065f;
    saved.hw_config.l_d        = 0.00024f;
    saved.hw_config.l_q        = 0.00027f;
    saved.hw_config.gear_ratio = 8.0f;
    saved.hw_config.gear_eff   = 0.9f;
    saved.prot_config.i_peak   = 16.0f;
    saved.prot_config.i_rated  = 3.8f;
    saved.prot_config.tor_peak = 12.0f;
    saved.prot_config.vel_peak = 41.0f;

    saved.ctrl_config.id_pid.config.kp  = 1.1f;
    saved.ctrl_config.id_pid.config.ki  = 1.2f;
    saved.ctrl_config.iq_pid.config.kp  = 2.1f;
    saved.ctrl_config.iq_pid.config.ki  = 2.2f;
    saved.ctrl_config.vel_pid.config.kp = 3.1f;
    saved.ctrl_config.vel_pid.config.ki = 3.2f;
    saved.ctrl_config.pos_pid.config.kp = 4.1f;
    saved.ctrl_config.pos_pid.config.ki = 4.2f;

    saved.ctrl_config.ramp_vel       = 9.0f;
    saved.ctrl_config.ramp_accel     = 10.0f;
    saved.ctrl_config.ramp_decel     = 11.0f;
    saved.ctrl_config.traj_vel       = 12.0f;
    saved.ctrl_config.traj_accel     = 13.0f;
    saved.ctrl_config.traj_decel     = 14.0f;
    saved.ctrl_config.traj_jerk      = 15.0f;
    saved.ctrl_config.curr_bandwidth = 900.0f;
    saved.ctrl_config.vel_bandwidth  = 35.0f;
    saved.ctrl_config.pos_bandwidth  = 12.0f;

    TEST_ASSERT(foc_param_save_motor(&saved) == PARAM_OK);
    TEST_ASSERT(foc_param_load_motor(&loaded) == PARAM_OK);

    loaded.traj.config.max_vel = 0.5f;
    loaded.traj.config.max_acc = 0.6f;
    loaded.traj.config.max_dec = 0.7f;

    foc_restore_finalize(&loaded);

    TEST_ASSERT_NEAR(loaded.ctrl_config.id_pid.config.kp, 1.1f, 1e-6f);
    TEST_ASSERT_NEAR(loaded.ctrl_config.id_pid.config.ki, 1.2f, 1e-6f);
    TEST_ASSERT_NEAR(loaded.ctrl_config.iq_pid.config.kp, 2.1f, 1e-6f);
    TEST_ASSERT_NEAR(loaded.ctrl_config.iq_pid.config.ki, 2.2f, 1e-6f);
    TEST_ASSERT_NEAR(loaded.ctrl_config.vel_pid.config.kp, 3.1f, 1e-6f);
    TEST_ASSERT_NEAR(loaded.ctrl_config.vel_pid.config.ki, 3.2f, 1e-6f);
    TEST_ASSERT_NEAR(loaded.ctrl_config.pos_pid.config.kp, 4.1f, 1e-6f);
    TEST_ASSERT_NEAR(loaded.ctrl_config.pos_pid.config.ki, 4.2f, 1e-6f);

    TEST_ASSERT_NEAR(loaded.traj.config.max_vel, 12.0f, 1e-6f);
    TEST_ASSERT_NEAR(loaded.traj.config.max_acc, 13.0f, 1e-6f);
    TEST_ASSERT_NEAR(loaded.traj.config.max_dec, 14.0f, 1e-6f);
    TEST_ASSERT(loaded.ctrl_state.iq_max > 0.0f);
    TEST_ASSERT(loaded.ctrl_state.vel_max == 41.0f);
    TEST_ASSERT(loaded.ctrl_state.dyn_iq_limit == loaded.ctrl_state.iq_max);
    TEST_ASSERT(loaded.ctrl_state.dyn_vel_limit == loaded.ctrl_state.vel_max);
}

int main(void)
{
    test_sys_invalid_enum_falls_back_to_safe_defaults();
    test_restore_transient_clear_preserves_persisted_input_mode();
    test_restore_finalize_rebinds_traj_and_runtime_limits();
    test_load_motor_then_finalize_preserves_tuning_and_refreshes_runtime();

    puts("[test_foc_param_restore_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
