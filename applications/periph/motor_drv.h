/**
 * @file motor_drv.h
 * @brief  FOC Control Utilities & Constants - Driver
 * @version v1.0.0
 * @date 2025.12.23
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize PID controllers
 * 
 * @param  motor  Pointer to motor instance structure
 */
void foc_pid_init(struct motor_dev *motor);

/**
 * @brief  Initialize low-pass filters
 * 
 * @param  motor  Pointer to motor instance structure
 */
void foc_lpf_init(struct motor_dev *motor);

/**
 * @brief  Enable PWM Outputs
 */
void pwm_out_enable(void);

/**
 * @brief  Disable PWM Outputs
 */
void pwm_out_disable(void);

/**
 * @brief  Set PWM duty cycles
 * 
 * @param  motor  Pointer to motor instance structure
 */
void pwm_out_set_duty(struct motor_dev *motor);

/**
 * @brief  Read ADC channel blocking
 * 
 * @param  channel  ADC channel select type
 * 
 * @return uint16_t ADC conversion data
 */
uint16_t adc_read_channel_blocking(adc_channel_select_type channel);

/**
 * @brief  Read VBUS and NTC voltages using DMA
 * 
 * @param  motor  Pointer to motor instance structure
 */
void adc_read_ordinary_dma(struct motor_dev *motor);

/**
 * @brief  Read VBUS and NTC voltages
 * 
 * Wrapper that uses the DMA-based implementation by default.
 * 
 * @param  motor  Pointer to motor instance structure
 */
void adc_read_ordinary(struct motor_dev *motor);

/**
 * @brief  Read and convert phase currents from ADC injected group
 * 
 * @param  motor  Pointer to motor instance structure
 */
void adc_read_phases(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
