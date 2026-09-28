/**
 * @file    motor_fsm.h
 * @brief   Motor Axis Finite State Machine — Public Interface
 * @version v1.2.0
 * @date    2026.03.03
 *
 * Architecture contract
 * ---------------------
 * 1. axis_state SYNC
 *    Every enter_cb writes motor->axis_state so the 20 kHz ADC1_2_IRQHandler
 *    switch(motor->axis_state) continues to work without modification.
 *    The FSM is the *only* legal place axis_state may be written.
 *
 * 2. DISABLE -> FAULT, never IDLE
 *    EV_CMD_DISABLE must transition to FAULT (_on_fault_enter kills PWM).
 *    Routing to IDLE would invoke _on_idle_enter -> pwm_out_enable(), which
 *    is the root cause of the v1.0 "silent re-enable" bug.
 *
 * 3. ISR safety
 *    motor_fsm_dispatch() / motor_fsm_dispatch_calib() must only be called
 *    from RTOS task context.  ISRs set a volatile flag; motor_task polls at
 *    1 kHz and dispatches EV_CALIB_DONE.
 *
 * 4. Calibration type passing
 *    motor_fsm_dispatch_calib() stores calib_type in the module-private
 *    _pending_calib_type slot before dispatching.  The action_cb reads it
 *    synchronously in the same task tick — no mutex needed.
 *
 * State diagram
 * -------------
 *
 *              EV_CMD_START
 *   +-------+ ------------> +---------+
 *   | IDLE  |               | RUNNING |
 *   |       | <------------ |         |
 *   +-------+  EV_CMD_STOP  +---------+
 *       ^                        |
 *       | EV_CMD_CLR_FAULT       | EV_FAULT_TRIP
 *       |                        v
 *   +-------+ <-- EV_CMD_DISABLE (ANY state) ---
 *   | FAULT | <-- EV_FAULT_TRIP  (ANY state) ---
 *   +-------+
 *
 *   IDLE  --EV_CMD_CALIB_{MOTOR|ENC}--> CALIB
 *   CALIB --EV_CALIB_DONE-------------> IDLE
 *   CALIB --EV_CMD_CALIB_ABORT--------> IDLE
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"
#include "shell_config.h"
#include "fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros — State IDs (uint16_t aliases of foc_axis_state enum) -------------------------- */

/**
 * @brief  FSM state IDs cast from foc_axis_state.
 *
 * Using the enum numeric values directly guarantees that the FSM's internal
 * current_state is always identical to motor->axis_state, so the ISR's
 * switch(motor->axis_state) requires zero translation.
 */
#define AXIS_ST_IDLE    ((uint16_t)AXIS_STATE_IDLE)
#define AXIS_ST_CALIB   ((uint16_t)AXIS_STATE_CALIB)
#define AXIS_ST_RUNNING ((uint16_t)AXIS_STATE_RUNNING)
#define AXIS_ST_FAULT   ((uint16_t)AXIS_STATE_FAULT)

/* Public Types — Events ------------------------------------------------------------------------ */

/**
 * @brief  Motor FSM event identifiers.
 *
 * MUST be dispatched from RTOS task context only — never from ISR.
 */
enum motor_fsm_event {
    EV_CMD_START       = 0, /*!< Request axis start       IDLE    -> RUNNING */
    EV_CMD_STOP        = 1, /*!< Request axis stop        RUNNING -> IDLE    */
    EV_CMD_DISABLE     = 2, /*!< Emergency stop           ANY     -> FAULT   */
    EV_CMD_CLR_FAULT   = 3, /*!< Clear fault latch        FAULT   -> IDLE    */
    EV_CMD_CALIB_MOTOR = 4, /*!< Start motor-param calib  IDLE    -> CALIB   */
    EV_CMD_CALIB_ENC   = 5, /*!< Start encoder calib      IDLE    -> CALIB   */
    EV_CMD_CALIB_ABORT = 6, /*!< Abort calibration        CALIB   -> IDLE    */
    EV_FAULT_TRIP      = 7, /*!< Fault detected           ANY     -> FAULT   */
    EV_CALIB_DONE      = 8, /*!< Calibration complete     CALIB   -> IDLE    */
};

/* Public Function Declarations ----------------------------------------------------------------- */

/**
 * @brief  Initialize the motor axis FSM.
 *
 * Wires motor->axis_fsm to the static state/transition tables and fires the
 * IDLE enter-callback.  Product firmware enables the 0 % duty active clamp;
 * bench firmware keeps PWM disabled until EV_CMD_START is accepted.
 *
 * Must be called once after all motor structures are fully initialised.
 *
 * @param  motor  Pointer to the motor device instance.  Must not be NULL.
 *
 * @note   On fsm_init() failure the function writes AXIS_STATE_FAULT directly
 *         and calls pwm_out_disable() as a fail-safe.
 */
void motor_fsm_init(struct motor_dev *motor);

/**
 * @brief  Dispatch an event to the motor axis FSM.
 *
 * Searches the transition table for (current_state, event).
 * If found, executes any action_cb (may veto the transition), then fires
 * exit_cb / enter_cb as required by the generic FSM engine.
 *
 * Unmatched events are silently dropped — this is the intentional safety-gate
 * behaviour (e.g. EV_CMD_START while in FAULT has no table entry).
 *
 * @param  motor  Pointer to the motor device instance.
 * @param  event  Event to dispatch (enum motor_fsm_event).
 *
 * @note   Must be called from RTOS task context only.
 */
int motor_fsm_dispatch(struct motor_dev *motor, enum motor_fsm_event event);

/**
 * @brief  Dispatch a calibration request (motor-param or encoder).
 *
 * Stores calib_type in the module-private _pending_calib_type slot, then
 * fires the correct event:
 *   - CALIB_ENC  -> EV_CMD_CALIB_ENC
 *   - all others -> EV_CMD_CALIB_MOTOR
 *
 * If the motor is not in IDLE state the FSM has no matching row for the event
 * and the request is silently dropped (safety gate).
 *
 * @param  motor       Pointer to the motor device instance.
 * @param  calib_type  One of: CALIB_RS, CALIB_LS, CALIB_FLUX, CALIB_JS,
 *                     CALIB_ENC.
 *
 * @note   Must be called from RTOS task context only.
 */
void motor_fsm_dispatch_calib(struct motor_dev *motor, enum foc_calib_state calib_type);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
