/**
 * @file    motor_fault_runtime.c
 * @brief   FalconFoc fault-policy runtime owner
 * @version v1.0.0
 * @date    2026.05.21
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_fault_runtime.h"
#include "motor_drv.h"

#ifdef __cplusplus
extern "C" {
#endif

void foc_fault_check(struct motor_dev *motor)
{
    (void)motor;

//     bool drive_active;

//     if (motor == NULL) {
//         return;
//     }

//     drive_active =
//         (AXIS_STATE_RUNNING == motor->axis_state) || (AXIS_STATE_CALIB == motor->axis_state);

//     if (drive_active) {
//         float i_total_sq =
//             motor->foc_calc.i_d * motor->foc_calc.i_d + motor->foc_calc.i_q * motor->foc_calc.i_q;
//         float i_trip = motor->prot_config.i_rated * 1.5f;

//         if (i_total_sq > (i_trip * i_trip)) {
//             motor->fault_code |= FAULT_OVER_CURR;
//             goto fault_trip;
//         }
//     }

//     if (motor->meas_data.v_bus > 1.0f) {
//         if (motor->meas_data.v_bus > motor->prot_config.v_bus_ov_thr) {
//             motor->fault_code |= FAULT_OVER_VOLT;
//             goto fault_trip;
//         }

//         if (motor->meas_data.v_bus < motor->prot_config.v_bus_uv_thr) {
//             motor->fault_code |= FAULT_UNDER_VOLT;
//             goto fault_trip;
//         }
//     }

//     if (drive_active) {
//         float abs_vel = motor->meas_data.out_vel_rads;
//         if (abs_vel < 0.0f) {
//             abs_vel = -abs_vel;
//         }

//         if (abs_vel > motor->prot_config.vel_over_thr) {
//             motor->fault_code |= FAULT_OVER_VEL;
//             goto fault_trip;
//         }
//     }

//     if ((motor->meas_data.motor_temp > 5.0f) &&
//         (motor->meas_data.motor_temp > motor->prot_config.motor_temp_thr)) {
//         motor->fault_code |= FAULT_MOTOR_OVER_TEMP;
//         goto fault_trip;
//     }

//     if ((motor->meas_data.driver_temp > 5.0f) &&
//         (motor->meas_data.driver_temp > motor->prot_config.driver_temp_thr)) {
//         motor->fault_code |= FAULT_DRIVER_OVER_TEMP;
//         goto fault_trip;
//     }

//     return;

// fault_trip:
//     pwm_out_disable();
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
