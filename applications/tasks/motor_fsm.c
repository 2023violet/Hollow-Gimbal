/**
 * @file    motor_fsm.c
 * @brief   Motor Axis Finite State Machine ¡ª Implementation
 * @version v1.2.0
 * @date    2026.03.03
 *
 * Key design decisions
 * --------------------
 *
 * A. axis_state sync contract
 *    Every enter_cb immediately writes motor->axis_state = AXIS_STATE_XXX.
 *    Because current_state is committed inside _fsm_do_transition() *before*
 *    enter_cb fires, any re-entrant read of axis_state (e.g. inside the
 *    callback itself) already sees the new value.  The 20 kHz ADC ISR reads
 *    motor->axis_state without any lock ¡ª it must always find a consistent,
 *    up-to-date value.
 *
 * B. DISABLE routes to FAULT, not IDLE  (the v1.0 critical bug fix)
 *    _on_idle_enter calls pwm_out_enable() unconditionally (industry-standard
 *    brake/clamp idle).  If EV_CMD_DISABLE targeted IDLE the sequence would be:
 *       action ¡ú pwm_out_disable()   ¡û correct
 *       enter  ¡ú pwm_out_enable()    ¡û silently cancels the disable!
 *    Routing to FAULT inverts this: _on_fault_enter calls pwm_out_disable()
 *    and there is nothing to re-enable it afterwards.
 *
 * C. CALIB state has no enter_cb
 *    All CALIB setup (axis_state write, calib_state.state assignment, optional
 *    foc_calib_enc_init) is done in the transition action_cb, not in an
 *    enter_cb.  This keeps the two calib sub-paths (motor params vs encoder)
 *    cleanly separated without branching inside a shared callback.
 *    _action_calib_motor and _action_calib_enc each write axis_state directly,
 *    satisfying the ISR-sync contract.
 *
 * D. _pending_calib_type
 *    A file-static variable.  motor_fsm_dispatch_calib() writes it immediately
 *    before calling motor_fsm_dispatch().  The action_cb reads it in the same
 *    RTOS task tick ¡ª no concurrent access is possible in a single-motor
 *    system, so no mutex is required.
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#if defined(THEMIS_HOST_TEST)
#include "../../tests/host/support/motor_fsm.h"
#include "../../tests/host/support/motor_task.h"
#include "../../tests/host/support/foc_calib.h"
#include "../../tests/host/support/foc_calib_enc.h"
#include "../../tests/host/support/motor_calibration_runtime.h"
#include "../../tests/host/support/foc_param.h"
#include "../../tests/host/support/motor_drv.h"
#include "../../tests/host/support/tm_shell.h"
#include "../../tests/host/support/tmdef.h"
#else
#include "motor_fsm.h"
#include "motor_task.h"
#include "foc_calib.h"
#include "foc_calib_enc.h"
#include "motor_calibration_runtime.h"
#include "foc_param.h"
#include "motor_drv.h"
#include "tm_shell.h"
#include "tmdef.h"
#endif

#include <string.h>

#if (SHELL_ENABLE == 1)
#define FSM_DEBUG_PRINT(fmt, ...)           \
    do {                                    \
        shell_printf((fmt), ##__VA_ARGS__); \
    } while (0)
#else
#define FSM_DEBUG_PRINT(fmt, ...) \
    do { /* Nothing */            \
    } while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Private constants ¡ª state ID aliases --------------------------------------------------------- */
/*
 * Defined in the header but also repeated here for local readability.
 * #define AXIS_ST_IDLE    ((uint16_t)AXIS_STATE_IDLE)
 * #define AXIS_ST_CALIB   ((uint16_t)AXIS_STATE_CALIB)
 * #define AXIS_ST_RUNNING ((uint16_t)AXIS_STATE_RUNNING)
 * #define AXIS_ST_FAULT   ((uint16_t)AXIS_STATE_FAULT)
 */

/* Private module state ------------------------------------------------------------------------- */

/**
 * @brief  Pending calibration type.
 *
 * Written by motor_fsm_dispatch_calib() immediately before the FSM dispatch.
 * Read by _action_calib_motor() in the same synchronous task context.
 * Single-motor system: no mutex required.
 */
static enum foc_calib_state _pending_calib_type = CALIB_RS;

/* Private callback prototypes ------------------------------------------------------------------ */

/* Enter callbacks */
static void _on_idle_enter(void *user_data);
static void _on_calib_enter(void *user_data);
static void _on_running_enter(void *user_data);
static void _on_fault_enter(void *user_data);

/* Transition action callbacks */
static bool _action_start(void *user_data);
static bool _action_calib_motor(void *user_data);
static bool _action_calib_enc(void *user_data);
static bool _action_calib_abort(void *user_data);
static bool _action_calib_done(void *user_data);
static bool _action_clear_fault(void *user_data);

/* Private constant tables ---------------------------------------------------------------------- */

/**
 * @brief  State node table.
 *
 * One entry per logical state.  Exit callbacks are NULL ¡ª cleanup is the
 * responsibility of the *entering* state's enter_cb (or action_cb for CALIB).
 *
 * IMPORTANT: AXIS_ST_CALIB uses a dedicated enter_cb (_on_calib_enter) that
 * writes motor->axis_state = AXIS_STATE_CALIB.  The two calib action_cbs
 * (_action_calib_motor / _action_calib_enc) still run first and may perform
 * additional sub-state setup ¡ª they also write axis_state themselves so the
 * ISR is never caught with a stale value between action and enter_cb.
 */
static const struct fsm_state_node _motor_fsm_states[] = {
    /* State ID          enter_cb            exit_cb */
    {AXIS_ST_IDLE, _on_idle_enter, NULL},
    {AXIS_ST_CALIB, _on_calib_enter, NULL},
    {AXIS_ST_RUNNING, _on_running_enter, NULL},
    {AXIS_ST_FAULT, _on_fault_enter, NULL},
};

/**
 * @brief  Transition table.
 *
 * Row format: { current_state, event, next_state, action_cb }
 * First matching row wins.  action_cb returning false vetoes the transition.
 *
 * Layout rationale:
 *  - Normal control rows first (readability).
 *  - Calibration rows grouped together.
 *  - DISABLE rows after calibration ¡ª four rows cover all four states so
 *    an emergency stop is never silently dropped.
 *  - FAULT_TRIP rows last ¡ª three rows (no FAULT->FAULT: a double-trip
 *    while already faulted is harmlessly dropped by -ENOENT).
 *
 * EV_CMD_DISABLE -> AXIS_ST_FAULT (NOT AXIS_ST_IDLE).
 * This is the critical fix: routing to IDLE would fire _on_idle_enter ->
 * pwm_out_enable(), silently cancelling the disable command.
 */
static const struct fsm_transition _motor_fsm_trans[] = {
    /* ©¤©¤ Normal axis control ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤ */
    {AXIS_ST_IDLE, EV_CMD_START, AXIS_ST_RUNNING, _action_start},
    {AXIS_ST_RUNNING, EV_CMD_STOP, AXIS_ST_IDLE, NULL},
    {AXIS_ST_FAULT, EV_CMD_CLR_FAULT, AXIS_ST_IDLE, _action_clear_fault},

    /* ©¤©¤ Calibration ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤ */
    {AXIS_ST_IDLE, EV_CMD_CALIB_MOTOR, AXIS_ST_CALIB, _action_calib_motor},
    {AXIS_ST_IDLE, EV_CMD_CALIB_ENC, AXIS_ST_CALIB, _action_calib_enc},
    {AXIS_ST_CALIB, EV_CMD_CALIB_ABORT, AXIS_ST_IDLE, _action_calib_abort},
    {AXIS_ST_CALIB, EV_CALIB_DONE, AXIS_ST_IDLE, _action_calib_done},

    /* ©¤©¤ Emergency disable (ALL states -> FAULT) ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤
     * Four rows: IDLE, CALIB, RUNNING, FAULT.
     * Including FAULT->FAULT (self-transition) is intentional: a second
     * EV_CMD_DISABLE while already faulted re-fires _on_fault_enter which
     * safely re-disables PWM and re-zeroes FOC variables ¡ª idempotent and
     * harmless.  Omitting the row would cause -ENOENT to be returned from
     * the FSM; keeping it makes the API contract cleaner for callers.      */
    {AXIS_ST_IDLE, EV_CMD_DISABLE, AXIS_ST_FAULT, NULL},
    {AXIS_ST_CALIB, EV_CMD_DISABLE, AXIS_ST_FAULT, NULL},
    {AXIS_ST_RUNNING, EV_CMD_DISABLE, AXIS_ST_FAULT, NULL},
    {AXIS_ST_FAULT, EV_CMD_DISABLE, AXIS_ST_FAULT, NULL},

    /* ©¤©¤ Fault trip (ALL non-FAULT states -> FAULT) ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤
     * Three rows: no FAULT->FAULT for EV_FAULT_TRIP ¡ª a second trip while
     * already in FAULT returns -ENOENT and is silently discarded, which is
     * the desired behaviour (no spurious re-entry of _on_fault_enter).     */
    {AXIS_ST_IDLE, EV_FAULT_TRIP, AXIS_ST_FAULT, NULL},
    {AXIS_ST_CALIB, EV_FAULT_TRIP, AXIS_ST_FAULT, NULL},
    {AXIS_ST_RUNNING, EV_FAULT_TRIP, AXIS_ST_FAULT, NULL},
};

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the motor axis FSM.
 */
void motor_fsm_init(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    int ret = fsm_init(&motor->axis_fsm,
                       _motor_fsm_states,
                       FSM_ARRAY_SIZE(_motor_fsm_states),
                       _motor_fsm_trans,
                       FSM_ARRAY_SIZE(_motor_fsm_trans),
                       AXIS_ST_IDLE,
                       (void *)motor);

    if (ret != 0) {
        /*
         * Should never happen with a correct table.
         * Write axis_state directly so the ADC ISR switch() sees a known
         * value even though the FSM control block is in an invalid state.
         */
        motor->axis_state = AXIS_STATE_FAULT;
        pwm_out_disable();
        FSM_DEBUG_PRINT("[ERR] motor_fsm_init: fsm_init failed - axis forced FAULT\r\n");
    }
}

/**
 * @brief  Dispatch an event to the motor axis FSM.
 */
int motor_fsm_dispatch(struct motor_dev *motor, enum motor_fsm_event event)
{
    if (motor == NULL) {
        return -EINVAL;
    }

    /*
     * fsm_dispatch_event() returns -ENOENT when no transition matches.
     * This is intentional "safety gate" behaviour ¡ª unhandled events in
     * certain states are simply ignored (e.g. EV_CMD_START in FAULT).
     * No error log here to avoid spamming the shell on normal operation.
     */
    // (void)fsm_dispatch_event(&motor->axis_fsm, (uint16_t)event);
    return fsm_dispatch_event(&motor->axis_fsm, (uint16_t)event);
}

/**
 * @brief  Dispatch a motor-param or encoder calibration request.
 */
void motor_fsm_dispatch_calib(struct motor_dev *motor, enum foc_calib_state calib_type)
{
    if (motor == NULL) {
        return;
    }

    /* Stash the type BEFORE dispatch ¡ª action_cb reads it synchronously */
    _pending_calib_type = calib_type;

    enum motor_fsm_event ev = (calib_type == CALIB_ENC) ? EV_CMD_CALIB_ENC : EV_CMD_CALIB_MOTOR;

    motor_fsm_dispatch(motor, ev);
}

/* Private callback implementations ¡ª enter callbacks ------------------------------------------- */

/**
 * @brief  IDLE state enter callback.
 *
 * Resets all PID integrators and setpoints via motor_calib_prepare_entry(),
 * then enables PWM at 0 % duty cycle (low-impedance brake / clamp idle ¡ª
 * industry standard for safe axis hold).
 *
 * @note   pwm_out_enable() with 0 % duty is NOT the same as pwm_out_disable().
 *         The former drives all low-side switches ON (active short / brake).
 *         The latter tri-states the bridge (motor may free-spin).
 *         Use EV_CMD_DISABLE -> FAULT for free-spin / coasting behaviour.
 */
static void _on_idle_enter(void *user_data)
{
    struct motor_dev *motor = (struct motor_dev *)user_data;
    bool              preserve_restored_modes;

    /* Sync axis_state FIRST ¡ª ADC ISR may read on the very next tick */
    preserve_restored_modes = (motor->axis_state == AXIS_STATE_IDLE);
    motor->axis_state       = AXIS_STATE_IDLE;

    if (preserve_restored_modes) {
        motor_restore_after_param_load(motor);
    } else {
        motor_calib_prepare_entry(motor);
    }

    /* Enable PWM at 0% ¡ª active low-side clamp (safe idle) */
    pwm_out_enable();
}

/**
 * @brief  CALIB state enter callback.
 *
 * Only syncs axis_state.  All sub-state-machine setup is performed by the
 * transition action_cb (_action_calib_motor or _action_calib_enc) which runs
 * before this callback.  Writing axis_state here is still required because
 * the generic FSM fires action_cb before updating current_state, so there is
 * a brief window where current_state == IDLE but axis_state must already read
 * CALIB for the ISR switch().
 */
static void _on_calib_enter(void *user_data)
{
    struct motor_dev *motor = (struct motor_dev *)user_data;

    /* axis_state was already written in the action_cb for immediate ISR sync.
     * Write it again here to cover any path where enter_cb fires without a
     * preceding action_cb (e.g. fsm_force_state used in recovery). */
    motor->axis_state = AXIS_STATE_CALIB;
}

/**
 * @brief  RUNNING state enter callback.
 *
 * Syncs pos_set and target_pos to the current measured position when entering
 * position-control mode.  Without this sync a sudden large error spike would
 * occur on the first PID tick if the motor moved while idle or in calib.
 */
static void _on_running_enter(void *user_data)
{
    struct motor_dev *motor = (struct motor_dev *)user_data;

    motor->axis_state = AXIS_STATE_RUNNING;

    if (motor->ctrl_mode == CTRL_MODE_POS) {
        motor->ctrl_state.pos_set    = motor->meas_data.out_pos_rad;
        motor->ctrl_state.target_pos = motor->meas_data.out_pos_rad;
    }
}

/**
 * @brief  FAULT state enter callback.
 *
 * Hard sequence (order matters):
 *  1. Write axis_state ¡ª ADC ISR must see FAULT on the next tick to stop
 *     updating FOC loops.
 *  2. Disable PWM ¡ª kill gate drive immediately.
 *  3. Reset PIDs / zero setpoints ¡ª no integrator windup while halted.
 *  4. Zero foc_calc ¡ª prevent stale SVPWM duty cycles from being output
 *     if the ISR fires between step 1 and step 2.
 *
 * @note   This callback also handles EV_CMD_DISABLE.  Routing DISABLE here
 *         (not to IDLE) is the critical fix introduced in v1.1: it ensures
 *         pwm_out_disable() is the last word on PWM state and is not
 *         subsequently overwritten by pwm_out_enable() in _on_idle_enter.
 */
static void _on_fault_enter(void *user_data)
{
    struct motor_dev *motor = (struct motor_dev *)user_data;

    /* Step 1: sync axis_state ¡ª ISR sees FAULT immediately */
    motor->axis_state = AXIS_STATE_FAULT;

    /* Step 2: hard-kill PWM outputs */
    pwm_out_disable();

    /* Step 3: reset PIDs and zero all setpoints */
    motor_calib_prepare_entry(motor);

    /* Step 4: zero all live FOC variables ¡ª no stale duty cycle output */
    memset(&motor->foc_calc, 0, sizeof(struct foc_vars));
}

/* Private callback implementations ¡ª action callbacks ------------------------------------------ */

/**
 * @brief  Guard for IDLE + EV_CMD_START -> RUNNING.
 *
 * Warns if the encoder has not been calibrated ¡ª closed-loop control will be
 * unreliable ¡ª but does NOT veto the transition.  The operator may intentionally
 * run open-loop voltage mode without a valid LUT.
 */
static bool _action_start(void *user_data)
{
    struct motor_dev *motor = (struct motor_dev *)user_data;

    if (!motor->enc_pri.state.calib_valid) {
        FSM_DEBUG_PRINT("[WARN] motor_fsm: encoder not calibrated. "
                        "Closed-loop modes may be unreliable.\r\n");
    }

    return true; /* Never veto ¡ª caller decides the risk */
}

/**
 * @brief  Action for IDLE + EV_CMD_CALIB_MOTOR -> CALIB.
 *
 * Prepares motor state, then arms the motor-parameter calibration sub-state
 * machine using the type stored in _pending_calib_type.
 *
 * axis_state is written here (before _on_calib_enter fires) to guarantee the
 * ADC ISR switch() sees AXIS_STATE_CALIB on the very next 20 kHz tick.
 */
static bool _action_calib_motor(void *user_data)
{
    struct motor_dev *motor = (struct motor_dev *)user_data;

    motor_calib_prepare_entry(motor);

    motor->calib_state.state = _pending_calib_type;

    /* Write axis_state eagerly ¡ª _on_calib_enter will write it again, which
     * is harmless, but this ensures no ISR tick sees a stale IDLE value. */
    motor->axis_state = AXIS_STATE_CALIB;

    return true;
}

/**
 * @brief  Action for IDLE + EV_CMD_CALIB_ENC -> CALIB.
 *
 * Prepares motor state, resets the encoder calibration context (clears LUT,
 * invalidates calib_valid, binds Vofa+ debug channels), then arms the encoder
 * sub-state machine.
 */
static bool _action_calib_enc(void *user_data)
{
    struct motor_dev *motor = (struct motor_dev *)user_data;

    motor_calib_prepare_entry(motor);

    motor->calib_state.state = CALIB_ENC;

    /* Reset encoder context: clear LUT, calib_valid flag, Vofa bindings */
    foc_calib_enc_init(motor);

    /* Eagerly arm the encoder sub-state machine so the next ADC ISR tick
     * starts from the pole-pair detection state.  Without this, a repeated
     * 'cal enc' could inherit the previous run's ENC_LUT_END state and
     * complete without performing the actual calibration sweep. */
    motor->calib_state.enc_state = ENC_PP_START;

    /* Eager axis_state sync (see note in _action_calib_motor) */
    motor->axis_state = AXIS_STATE_CALIB;

    return true;
}

/**
 * @brief  Action for CALIB + EV_CMD_CALIB_ABORT -> IDLE.
 *
 * Resets every calibration sub-state machine to its idle/initial value so
 * that a subsequent 'calib motor' or 'calib enc' starts from a clean slate.
 *
 * If the aborted flow is encoder calibration, restore the last committed
 * encoder payload from application-owned persistence so an operator does not
 * lose a previously valid calibration merely by entering and aborting
 * `cal enc`.
 *
 * axis_state will be updated by _on_idle_enter() after this returns true.
 */
static bool _action_calib_abort(void *user_data)
{
    struct motor_dev *motor = (struct motor_dev *)user_data;
    bool              restore_last_committed_enc;

    restore_last_committed_enc = (motor->calib_state.state == CALIB_ENC);

    /*
     * Close the ISR race window immediately.
     * Until _on_idle_enter() runs, ADC IRQ may still preempt this task. If
     * axis_state remains CALIB for that window, foc_calib_loop() can execute
     * one more step and re-arm a calib sub-state that we just cleared.
     */
    motor->axis_state = AXIS_STATE_IDLE;

    if (restore_last_committed_enc) {
        int st = motor_calibration_runtime_restore_encoder_after_abort(motor);

        if (st == PARAM_OK) {
            FSM_DEBUG_PRINT("[OK] motor_fsm: encoder calibration abort restored last committed payload.\r\n");
        } else {
            FSM_DEBUG_PRINT("[WARN] motor_fsm: encoder calibration abort found no committed payload (%s).\r\n",
                            foc_param_status_str(st));
        }
    }

    FSM_DEBUG_PRINT("[OK] motor_fsm: calibration aborted -> IDLE\r\n");

    return true;
}

/**
 * @brief  Action for CALIB + EV_CALIB_DONE -> IDLE.
 */
static bool _action_calib_done(void *user_data)
{
    TM_UNUSED(user_data);

    FSM_DEBUG_PRINT("\r\n[SUCCESS] Calibration sequence completed normally!\r\n");

    return true;
}

/**
 * @brief  Action for FAULT + EV_CMD_CLR_FAULT -> IDLE.
 *
 * Clearing the state latch alone is not sufficient. The live shell and
 * product-state contract both treat `fault clear` as the formal owner of
 * leaving FAULT and dropping the persisted fault mask that drove that state.
 */
static bool _action_clear_fault(void *user_data)
{
    struct motor_dev *motor = (struct motor_dev *)user_data;

    if (motor == NULL) {
        return false;
    }

    motor->fault_code = FAULT_NONE;
    return true;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
