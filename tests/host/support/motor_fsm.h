/**
 * @file    motor_fsm.h
 * @brief   Host shim for motor_fsm unit coverage
 * @version v1.0.0
 * @date    2026.05.12
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"
#include "fsm.h"
#include "shell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AXIS_ST_IDLE    ((uint16_t)AXIS_STATE_IDLE)
#define AXIS_ST_CALIB   ((uint16_t)AXIS_STATE_CALIB)
#define AXIS_ST_RUNNING ((uint16_t)AXIS_STATE_RUNNING)
#define AXIS_ST_FAULT   ((uint16_t)AXIS_STATE_FAULT)

enum motor_fsm_event {
    EV_CMD_START       = 0,
    EV_CMD_STOP        = 1,
    EV_CMD_DISABLE     = 2,
    EV_CMD_CLR_FAULT   = 3,
    EV_CMD_CALIB_MOTOR = 4,
    EV_CMD_CALIB_ENC   = 5,
    EV_CMD_CALIB_ABORT = 6,
    EV_FAULT_TRIP      = 7,
    EV_CALIB_DONE      = 8,
};

void motor_fsm_init(struct motor_dev *motor);
int  motor_fsm_dispatch(struct motor_dev *motor, enum motor_fsm_event event);
void motor_fsm_dispatch_calib(struct motor_dev *motor, enum foc_calib_state calib_type);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
