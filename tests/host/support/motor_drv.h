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

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
