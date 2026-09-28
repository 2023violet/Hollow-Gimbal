/**
 * @file    motor_drv.h
 * @brief   Host shim for motor_fsm unit coverage
 * @version v1.0.0
 * @date    2026.05.12
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void pwm_out_enable(void);
void pwm_out_disable(void);
void foc_lpf_refresh(struct motor_dev *motor);

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
bool motor_drv_diag_set_pwm_map(uint8_t map_id);
bool motor_drv_diag_set_adc_map(uint8_t map_id);
void motor_drv_diag_reset_maps(void);
uint8_t motor_drv_diag_get_pwm_map(void);
uint8_t motor_drv_diag_get_adc_map(void);
bool motor_drv_diag_set_adc_trigger(uint16_t compare);
uint16_t motor_drv_diag_get_adc_trigger(void);
#endif

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
