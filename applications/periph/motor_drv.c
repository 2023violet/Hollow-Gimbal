/**
 * @file motor_drv.h.c
 * @brief  FOC Control Utilities & Constants - Driver
 * @version v1.0.0
 * @date 2025.12.23
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_drv.h"
#include "ntc_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* private functions ---------------------------------------------------------------------------- */

/**
 * @brief  Initialize all FOC PID controllers (id/iq/vel/pos)
 * 
 * @param  motor  Pointer to motor instance structure
 */
void foc_pid_init(struct motor_dev *motor)
{
    /* --- 1. Current Loop (Inner Loop - Fast) --- */
    struct pid_config curr_cfg = {
        .mode           = PID_POSITION,
        .kp             = DEFAULT_CURR_PID_KP,
        .ki             = DEFAULT_CURR_PID_KI,
        .kd             = DEFAULT_CURR_PID_KD,
        .max_output     = DEFAULT_CURR_MAX_OUT,
        .integral_limit = DEFAULT_CURR_MAX_OUT,
        .dead_band      = 0.0f,
        .output_lpf_hz  = 0.0f,
        .deriv_lpf_hz   = 0.0f,
        .improve        = PID_INTEGRAL_LIMIT,
    };
    pid_init(&motor->ctrl_config.id_pid, &curr_cfg);
    pid_init(&motor->ctrl_config.iq_pid, &curr_cfg);

    /* --- 2. Velocity Loop (Middle Loop) --- */
    struct pid_config vel_cfg = {
        .mode           = PID_POSITION,
        .kp             = DEFAULT_VEL_PID_KP,
        .ki             = DEFAULT_VEL_PID_KI,
        .kd             = DEFAULT_VEL_PID_KD,
        .max_output     = DEFAULT_VEL_MAX_OUT,
        .integral_limit = DEFAULT_VEL_MAX_OUT,
        .dead_band      = 0.0f,
        .output_lpf_hz  = 0.0f,
        .deriv_lpf_hz   = 0.0f,
        .improve        = PID_INTEGRAL_LIMIT,
    };
    pid_init(&motor->ctrl_config.vel_pid, &vel_cfg);

    /* --- 3. Position Loop (Outer Loop - Slow) --- */
    struct pid_config pos_cfg = {
        .mode           = PID_POSITION,
        .kp             = DEFAULT_POS_PID_KP,
        .ki             = DEFAULT_POS_PID_KI,
        .kd             = DEFAULT_POS_PID_KD,
        .max_output     = DEFAULT_POS_MAX_OUT,
        .integral_limit = DEFAULT_POS_MAX_OUT,
        .dead_band      = 0.0f,
        .output_lpf_hz  = 0.0f,
        .deriv_lpf_hz   = 0.0f,
        .improve        = PID_INTEGRAL_LIMIT,
    };
    pid_init(&motor->ctrl_config.pos_pid, &pos_cfg);

    motor->ctrl_config.id_pid.dt  = CURR_LOOP_TS;
    motor->ctrl_config.iq_pid.dt  = CURR_LOOP_TS;
    motor->ctrl_config.vel_pid.dt = VEL_LOOP_TS;
    motor->ctrl_config.pos_pid.dt = POS_LOOP_TS;
}

/**
 * @brief  Initialize a low-pass filter
 * 
 * @param  motor  Pointer to motor instance structure
 */
void foc_lpf_init(struct motor_dev *motor)
{
    lpf_init(&motor->ctrl_state.iq_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    lpf_init(&motor->ctrl_state.id_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.vq_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.vd_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.v_bus_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.vel_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.pos_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
}

/**
 * @brief  Enable PWM Outputs
 */
void pwm_out_enable(void)
{
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_1, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_1C, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_2, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_2C, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_3, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_3C, TRUE);

    tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_1, 0);
    tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_2, 0);
    tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_3, 0);
}

/**
 * @brief  Disable PWM Outputs
 */
void pwm_out_disable(void)
{
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_1, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_1C, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_2, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_2C, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_3, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_3C, FALSE);
}

/**
 * @brief  Set PWM duty cycles
 * 
 * @param  motor  Pointer to motor instance structure
 */
void pwm_out_set_duty(struct motor_dev *motor)
{
    switch (motor->hw_config.phase_order) {
    case PHASE_ORDER_ABC: {
        tmr_channel_value_set(TMR1,
                              TMR_SELECT_CHANNEL_3,
                              (uint32_t)(motor->foc_calc.dtc_a * TMR1->pr));
        tmr_channel_value_set(TMR1,
                              TMR_SELECT_CHANNEL_2,
                              (uint32_t)(motor->foc_calc.dtc_b * TMR1->pr));
        tmr_channel_value_set(TMR1,
                              TMR_SELECT_CHANNEL_1,
                              (uint32_t)(motor->foc_calc.dtc_c * TMR1->pr));
    } break;

    case PHASE_ORDER_ACB: {
        tmr_channel_value_set(TMR1,
                              TMR_SELECT_CHANNEL_3,
                              (uint32_t)(motor->foc_calc.dtc_a * TMR1->pr));
        tmr_channel_value_set(TMR1,
                              TMR_SELECT_CHANNEL_2,
                              (uint32_t)(motor->foc_calc.dtc_c * TMR1->pr));
        tmr_channel_value_set(TMR1,
                              TMR_SELECT_CHANNEL_1,
                              (uint32_t)(motor->foc_calc.dtc_b * TMR1->pr));
    } break;
    }
}

/* public functions adc ------------------------------------------------------------------------- */

/**
 * @brief  Read ADC channel blocking
 * 
 * @param  channel  ADC channel select type
 * 
 * @return uint16_t ADC conversion data
 */
uint16_t adc_read_channel_blocking(adc_channel_select_type channel)
{
    (void)channel;
    return 0;
}

/**
 * @brief  Read VBUS and NTC voltages
 * 
 * @param  motor  Pointer to motor instance structure
 */
void adc_read_ordinary(struct motor_dev *motor)
{
    (void)motor;
}

/**
 * @brief  Read and convert phase currents from ADC injected group
 * 
 * @param  motor  Pointer to motor instance structure
 */
void adc_read_phases(struct motor_dev *motor)
{
    motor->meas_data.v_bus_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_1);
    motor->meas_data.v_bat_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_2);
    motor->meas_data.ib_raw    = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_3);
    motor->meas_data.ia_raw    = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_4);

    motor->meas_data.b     = FAC_CURR((motor->meas_data.ib_raw - motor->meas_data.ib_offset));
    motor->meas_data.a     = FAC_CURR((motor->meas_data.ia_raw - motor->meas_data.ia_offset));
    /* ia + ib + ic = 0 → ic = -ia -ib */
    motor->meas_data.c     = -motor->meas_data.a - motor->meas_data.b;
    motor->meas_data.v_bus = FAC_VBUS(motor->meas_data.v_bus_raw);
    motor->meas_data.v_bat = FAC_VBUS(motor->meas_data.v_bat_raw);
}

// void adc_read_phases(struct motor_dev *motor)
// {
//     motor->meas_data.v_bus_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_1);

//     switch (motor->hw_config.phase_order) {
//     case PHASE_ORDER_ABC: {
//         motor->meas_data.ic_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_2);
//         motor->meas_data.ib_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_3);
//         motor->meas_data.ia_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_4);
//     } break;

//     case PHASE_ORDER_ACB: {
//         motor->meas_data.ib_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_2);
//         motor->meas_data.ic_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_3);
//         motor->meas_data.ia_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_4);
//     } break;
//     }

//     motor->meas_data.c     = FAC_CURR((motor->meas_data.ic_raw - motor->meas_data.ic_offset));
//     motor->meas_data.b     = FAC_CURR((motor->meas_data.ib_raw - motor->meas_data.ib_offset));
//     motor->meas_data.a     = FAC_CURR((motor->meas_data.ia_raw - motor->meas_data.ia_offset));
//     motor->meas_data.v_bus = FAC_VBUS(motor->meas_data.v_bus_raw);
// }

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
