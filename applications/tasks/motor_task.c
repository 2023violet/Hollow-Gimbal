/**
 * @file    motor_task.c
 * @brief   Falcon Motor Control Task — State Machine Brain
 * @version v2.0.0
 * @date    2026.02.19
 *
 * Revision History:
 *   v2.0.0 - State-transition-based entry actions (no redundant per-cycle ops);
 *            Flash param load/save integrated; foc_cmd_xxx removed from task loop
 *            (Shell/CAN own target writes); deferred save moved to safe_task;
 *            calib entry helper added; all hardcoded test vectors removed.
 *   v1.0.0 - Initial implementation.
 *
 * Architecture:
 *   ADC ISR  @20kHz — current loop + encoder
 *   TIM6 ISR @5khz — velocity/position outer loops
 *   motor_task @1kHz — state machine + input processing + trajectory
 *   safe_task @100Hz — temperature + fault + deferred Flash save + telemetry
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_task.h"
#include "motor_fsm.h"
#if (TM_FEATURE_VOFA != 0U)
#include "vofa.h"
#endif
#include "foc_core.h"
#include "foc_math.h"
#include "foc_ctrl.h"
#include "motor_drv.h"
#include "foc_calib.h"
#include "foc_calib_enc.h"
#include "foc_param.h"
#include "foc_restore.h"
#include "shell_task.h"
#include "core_time.h"
#include "tm_export.h"
#include "tm_shell.h"

#if (SHELL_ENABLE == 1)
#define MOTOR_DEBUG_PRINT(fmt, ...)         \
    do {                                    \
        shell_printf((fmt), ##__VA_ARGS__); \
    } while (0)
#else
#define MOTOR_DEBUG_PRINT(fmt, ...) \
    do { /* Nothing */              \
    } while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Private variables ---------------------------------------------------------------------------- */

static osThreadId_t motor_task_handle      = NULL;
static osThreadId_t motor_safe_task_handle = NULL;

static const osThreadAttr_t motor_task_attr = {
    .name       = "motor_task",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityHigh,
};

static const osThreadAttr_t safe_task_attr = {
    .name       = "motor_safe",
    .stack_size = 128 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/* Task performance statistics */
#if (TASK_PERF_ENABLE == 1)
static struct core_time_perf adc_iqr_perf;
static struct core_time_perf tim5_iqr_perf;
static struct core_time_perf motor_task_perf;
static struct core_time_perf safe_task_perf;

static uint32_t adc_time_us   = 0;
static uint32_t tim5_time_us  = 0;
static uint32_t motor_time_us = 0;
static uint32_t safe_time_us  = 0;
#endif

/* Public motor instance (extern-referenced by shell commands and other modules) */
struct motor_dev _motor_ins;

/*
 * Default configuration structures.
 * These serve as compile-time fallbacks if Flash load fails or the device
 * has never been configured. Values match the GIM6010 motor datasheet.
 */
static struct motor_hw_config _default_hw_config = {
    .poles       = DEFAULT_POLE_PAIRS,
    .rs          = DEFAULT_RS,
    .ls          = DEFAULT_LS,
    .flux        = DEFAULT_FLUX,
    .js          = DEFAULT_JS,
    .l_d         = DEFAULT_ID,
    .l_q         = DEFAULT_IQ,
    .l_d_diff    = DEFAULT_IDIQ_DIFF,
    .inertia     = DEFAULT_INERTIA,
    .gear_ratio  = DEFAULT_GEAR_RATIO,
    .gear_eff    = DEFAULT_GEAR_EFF,
    .phase_order = DEFAULT_PHASE_ORDER,
};

static struct motor_prot_config _default_prot_config = {
    .v_rated         = DEFAULT_VOLT_RATED,
    .v_bus_ov_thr    = DEFAULT_V_BUS_OV,
    .v_bus_uv_thr    = DEFAULT_V_BUS_UV,
    .i_rated         = DEFAULT_CURR_RATED,
    .i_peak          = DEFAULT_CURR_PEAK,
    .vel_rated       = DEFAULT_VEL_RATED,
    .vel_peak        = DEFAULT_VEL_PEAK,
    .vel_over_thr    = DEFAULT_VEL_OVER,
    .tor_rated       = DEFAULT_TOR_RATED,
    .tor_peak        = DEFAULT_TOR_PEAK,
    .motor_temp_thr  = DEFAULT_MOTOR_TEMP,
    .driver_temp_thr = DEFAULT_DRIVER_TEMP,
};

static struct motor_ctrl_config _default_ctrl_config = {
    .ramp_vel       = DEFAULT_RAMP_VEL,
    .ramp_accel     = DEFAULT_RAMP_ACCEL,
    .ramp_decel     = DEFAULT_RAMP_DECEL,
    .traj_vel       = DEFAULT_TRAJ_VEL,
    .traj_accel     = DEFAULT_TRAJ_ACCEL,
    .traj_decel     = DEFAULT_TRAJ_DECEL,
    .traj_jerk      = DEFAULT_TRAJ_JERK,
    .curr_bandwidth = DEFAULT_CURR_BW,
    .vel_bandwidth  = DEFAULT_VEL_BW,
    .pos_bandwidth  = DEFAULT_POS_BW,
};

static struct encoder_config _default_encoder_cfg = {
    .spi_instance      = SPI1,
    .cs_port           = GPIOA,
    .cs_pin            = GPIO_PINS_4,
    .dir               = DEFAULT_DIR,
    .mech_offset       = DEFAULT_OFFSET,
    .pole_pairs        = DEFAULT_POLE_PAIRS,
    .pll_bandwidth     = ENCODER_PLL_BANDWIDTH,
    .vel_lpf_bandwidth = ENCODER_LPF_BANDWIDTH,
};

/* Private function prototypes ------------------------------------------------------------------ */

static void _motor_task_entry(void *argument);
static void _motor_safe_task_entry(void *argument);
static void _motor_init(struct motor_dev *motor);
static void _motor_task_load_params(struct motor_dev *motor);
static void _motor_handle_deferred_save(struct motor_dev *motor);
static void _motor_clear_setpoints(struct motor_dev *motor);
static void _motor_restore_finalize(struct motor_dev *motor);

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize and launch all motor control tasks
 *
 * @note   Called automatically via INIT_EXPORT at RTOS startup.
 *         Performs blocking current calibration (~2s) before task creation.
 */
void motor_task_init(void)
{
    /*
     * Shell transport/session startup stays an explicit APP-owner step here so
     * the live startup chain remains visible in code and docs.
     */
#if (SHELL_ENABLE == 1)
    shell_task_init();
#endif

    _motor_init(&_motor_ins);

    motor_task_handle      = osThreadNew(_motor_task_entry, NULL, &motor_task_attr);
    motor_safe_task_handle = osThreadNew(_motor_safe_task_entry, NULL, &safe_task_attr);
}
INIT_EXPORT(motor_task_init, EXPORT_LEVEL_APP);

/* Private functions ---------------------------------------------------------------------------- */

/**
 * @brief  Full motor system initialization sequence
 *
 * @note   Order is critical:
 *         1. Zero entire motor_dev to guarantee no garbage state
 *         2. Copy compile-time defaults
 *         3. Initialize all software components
 *         4. Attempt Flash parameter restore (overwrites defaults on success)
 *         5. Re-initialize PID with loaded (or default) configs
 *         6. Run blocking current offset calibration (ADC offsets)
 *
 * @param  motor  Pointer to motor instance
 */
static void _motor_init(struct motor_dev *motor)
{
    /* 1. Zero all fields: eliminates any random RAM content */
    memset(motor, 0, sizeof(struct motor_dev));

    /* 2. Write compile-time defaults */
    motor->hw_config   = _default_hw_config;
    motor->prot_config = _default_prot_config;
    motor->ctrl_config = _default_ctrl_config;

    /* 3. Initial state machine configuration */
    motor->axis_state  = AXIS_STATE_IDLE;
    motor->ctrl_mode   = CTRL_MODE_VOLT;
    motor->input_mode  = INPUT_MODE_INACTIVE;
    motor->sensor_type = SENSOR_TYPE_MAGNETIC;

    /* 4. Initialize encoder hardware driver with a modifiable copy */
    encoder_init(&motor->enc_pri, &_default_encoder_cfg);

    /* 5. Initialize FOC subsystems */
    foc_calib_init(motor);
    foc_pid_init(motor);
    foc_lpf_init(motor);

    /* 6. Initialize trajectory planner */
    traj_trap_init(&motor->traj);
    struct traj_trap_config traj_cfg = {
        .max_vel = motor->ctrl_config.traj_vel,
        .max_acc = motor->ctrl_config.traj_accel,
        .max_dec = motor->ctrl_config.traj_decel,
    };
    traj_trap_config_set(&motor->traj, &traj_cfg);

    /* 8. Restore parameters from Flash (overwrites defaults on success) */
    _motor_task_load_params(motor);
}

/**
 * @brief  Load all parameters from Flash, fall back to defaults on any error
 *
 * @note   Called once from _motor_init() before current calibration.
 *         Each slot is independent — a failure in one does not block others.
 *
 * @param  motor  Pointer to motor instance (defaults already written)
 */
static void _motor_task_load_params(struct motor_dev *motor)
{
    int  st;
    bool motor_restored = false;
    bool enc_restored   = false;
    bool sys_restored   = false;

    /* --- Motor: hw_config + prot_config + PID configs + motion params --- */
    st = foc_param_load_motor(motor);
    switch (st) {
    case PARAM_OK:
        motor_restored = true;
        MOTOR_DEBUG_PRINT("[PARAM] Motor params restored from Flash.\r\n");
        break;
    case PARAM_EMPTY:
        MOTOR_DEBUG_PRINT("[PARAM] Motor slot empty - using factory defaults.\r\n");
        break;
    case PARAM_ERR_VERSION:
        MOTOR_DEBUG_PRINT("[WARN]  Motor param version mismatch after firmware update.\r\n");
        MOTOR_DEBUG_PRINT("        Using defaults. Run 'store save motor' to repersist.\r\n");
        break;
    case PARAM_ERR_CRC:
        MOTOR_DEBUG_PRINT("[ERR]   Motor param CRC error - possible Flash corruption.\r\n");
        MOTOR_DEBUG_PRINT("        Using defaults. Run 'store erase' then reconfigure.\r\n");
        break;
    default:
        MOTOR_DEBUG_PRINT("[ERR]   Motor param load failed (%s).\r\n", foc_param_status_str(st));
        break;
    }

    /* --- Encoder: LUT + mech_offset + calib_valid --- */
    st = foc_param_load_enc(motor);
    switch (st) {
    case PARAM_OK:
        enc_restored = true;
        if (motor->enc_pri.state.calib_valid) {
            MOTOR_DEBUG_PRINT("[PARAM] Encoder calibration restored. Motor ready.\r\n");
        } else {
            MOTOR_DEBUG_PRINT("[WARN]  Encoder slot OK but calib_valid=false.\r\n");
            MOTOR_DEBUG_PRINT("        Run 'calib enc' before using closed-loop modes.\r\n");
        }
        break;
    case PARAM_EMPTY:
        MOTOR_DEBUG_PRINT("[WARN]  Encoder not calibrated. Run 'calib enc'.\r\n");
        break;
    default:
        MOTOR_DEBUG_PRINT("[WARN]  Encoder param load: %s. Run 'calib enc'.\r\n",
                          foc_param_status_str(st));
        break;
    }

    /* --- System config (non-critical, silent on EMPTY) --- */
    st = foc_param_load_sys(motor);
    if (st == PARAM_OK) {
        sys_restored = true;
        MOTOR_DEBUG_PRINT("[PARAM] System config restored.\r\n");
    }
    /* PARAM_EMPTY is normal for devices that never had sys config saved */

    if (motor_restored || enc_restored || sys_restored) {
        _motor_restore_finalize(motor);
    }
}

/**
 * @brief  Execute deferred Flash save requests
 *
 * @note   Called from safe_task (100Hz, RTOS context).
 *         Flash erase + write (~20ms/sector) disables all IRQs.
 *         This function MUST NOT be called from any ISR context.
 *
 * @param  motor  Pointer to motor instance
 */
static void _motor_handle_deferred_save(struct motor_dev *motor)
{
    /* Atomically snapshot and clear the request flags.
     * Even though motor_task and safe_task share the struct, the flag byte
     * is only set from ISR (atomic) and cleared here (non-ISR). The 100Hz
     * polling rate means at most ~10ms delay between request and save. */
    uint8_t req = motor->calib_state.param_save_req;
    if (req == PARAM_SAVE_NONE) {
        return;
    }

    motor->calib_state.param_save_req = PARAM_SAVE_NONE; /* Clear before long operation */

    if (req & PARAM_SAVE_ENC) {
#if (SHELL_ENABLE == 1)
        int st = foc_param_save_enc(motor);
        if (st == PARAM_OK) {
            MOTOR_DEBUG_PRINT("[PARAM] Encoder calibration saved to Flash.\r\n");
        } else {
            MOTOR_DEBUG_PRINT("[ERR]   Encoder save failed: %s\r\n", foc_param_status_str(st));
        }
#else
        foc_param_save_enc(motor);
#endif
    }

    if (req & PARAM_SAVE_MOTOR) {
#if (SHELL_ENABLE == 1)
        int st = foc_param_save_motor(motor);
        if (st == PARAM_OK) {
            MOTOR_DEBUG_PRINT("[PARAM] Motor params saved to Flash.\r\n");
        } else {
            MOTOR_DEBUG_PRINT("[ERR]   Motor param save failed: %s\r\n", foc_param_status_str(st));
        }
#else
        foc_param_save_motor(motor);
#endif
    }

    if (req & PARAM_SAVE_SYS) {
#if (SHELL_ENABLE == 1)
        int st = foc_param_save_sys(motor);
        if (st == PARAM_OK) {
            MOTOR_DEBUG_PRINT("[PARAM] System config saved to Flash.\r\n");
        } else {
            MOTOR_DEBUG_PRINT("[ERR]   Sys config save failed: %s\r\n", foc_param_status_str(st));
        }
#else
        foc_param_save_sys(motor);
#endif
    }
}

/**
 * @brief  Clear all active and pending setpoints
 *
 * @note   Called before entering any calibration or on motor stop.
 *         Prevents stale targets from being immediately re-applied when
 *         the axis returns to RUNNING state.
 *
 * @param  motor  Pointer to motor instance
 */
/* motor_task.c */
static void _motor_clear_setpoints(struct motor_dev *motor)
{
    motor->ctrl_state.target_vel = 0.0f;
    motor->ctrl_state.target_iq  = 0.0f;
    motor->ctrl_state.target_id  = 0.0f;
    motor->ctrl_state.target_pos = motor->meas_data.out_pos_rad;
    motor->ctrl_state.target_tor = 0.0f;

    motor->ctrl_state.vel_set = 0.0f;
    motor->ctrl_state.iq_set  = 0.0f;
    motor->ctrl_state.id_set  = 0.0f;
    motor->ctrl_state.pos_set = motor->meas_data.out_pos_rad;
    motor->ctrl_state.vel_ff  = 0.0f;
    motor->ctrl_state.iq_ff   = 0.0f;

    motor->ctrl_state.dyn_iq_limit  = motor->ctrl_state.iq_max;
    motor->ctrl_state.dyn_vel_limit = motor->ctrl_state.vel_max;

    /* pos_pid output → vel_set (rad/s), limit by vel_max ✓ */
    motor->ctrl_config.pos_pid.config.max_output     = motor->ctrl_state.vel_max;
    motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.vel_max;

    /* vel_pid output → iq_set (A), limit by iq_max */
    motor->ctrl_config.vel_pid.config.max_output     = motor->ctrl_state.iq_max;
    motor->ctrl_config.vel_pid.config.integral_limit = motor->ctrl_state.iq_max;

    motor->input_mode = INPUT_MODE_INACTIVE;
}

static void _motor_restore_finalize(struct motor_dev *motor)
{
    foc_restore_finalize(motor);
    foc_restore_clear_transient_setpoints(motor);
}

void motor_restore_after_param_load(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    _motor_restore_finalize(motor);
}

/* ISR Functions: ADC and Timer -----------------------------------------------------------------
 *
 * These run at the highest priority (IRQ 1 and 3 respectively).
 * Keep execution time minimal — no RTOS calls, no blocking, no Flash I/O.
 */

/**
 * @brief  ADC Injected Group ISR — 20kHz, Priority 1
 *
 * @note   Execution budget: < 25μs (half PWM period).
 *         Measured typical: ~12-15μs on AT32F415 @ 150MHz.
 */
void ADC1_2_IRQHandler(void)
{
    if (adc_interrupt_flag_get(ADC1, ADC_PCCE_FLAG) == RESET) {
        return;
    }
    adc_flag_clear(ADC1, ADC_PCCE_FLAG);

    /* [1] Sample phase currents and bus voltage from injected ADC group */
    adc_read_phases(&_motor_ins);
#if (TASK_PERF_ENABLE == 1)
    core_time_perf_start(&adc_iqr_perf);
#endif
    /* [2] Update encoder: SPI read → LUT correction → PLL velocity estimation */
    encoder_update(&_motor_ins.enc_pri);
#if (TASK_PERF_ENABLE == 1)
    core_time_perf_stop(&adc_iqr_perf);
    adc_time_us = (uint32_t)core_time_perf_get_us(&adc_iqr_perf);
    (void)(adc_time_us);
#endif
    /* [3] Dispatch to the appropriate real-time computation */
    switch (_motor_ins.axis_state) {
    case AXIS_STATE_CALIB:
        /* Calibration routines: Rs, Ls, Flux, Js, or Encoder LUT generation */
        foc_calib_loop(&_motor_ins);
        break;

    case AXIS_STATE_RUNNING:
        /* Full FOC pipeline: parameter calc → Park/Clarke → PID → SVPWM */
        foc_current_loop_step(&_motor_ins);
        break;

    default:
        /* IDLE or FAULT: no current control needed */
        break;
    }
}

/**
 * @brief  TIM6 Overflow ISR — 5khz, Priority 3
 *
 * @note   Outer loops only run in RUNNING state.
 *         Velocity loop: 5khz.
 *         Position loop: 5kHz (every 2nd TIM6 tick, internally divided).
 */
void TMR5_GLOBAL_IRQHandler(void)
{
    if (tmr_interrupt_flag_get(TMR5, TMR_OVF_FLAG) == RESET) {
        return;
    }
    tmr_flag_clear(TMR5, TMR_OVF_FLAG);

#if (TASK_PERF_ENABLE == 1)
    core_time_perf_start(&tim5_iqr_perf);
#endif

    if (_motor_ins.axis_state == AXIS_STATE_RUNNING) {
        foc_outer_loop_step(&_motor_ins);
    }

#if (TASK_PERF_ENABLE == 1)
    core_time_perf_stop(&tim5_iqr_perf);
    tim5_time_us = (uint32_t)core_time_perf_get_us(&tim5_iqr_perf);
    (void)(tim5_time_us);
#endif
}

/* RTOS Tasks ---------------------------------------------------------------------------------  */

/**
 * @brief  Motor state machine task — 1kHz
 *
 * @note   Handles state transitions, setpoint processing, and trajectory updates.
 *         Shell commands write to target_xxx fields; this task consumes them.
 *
 *         foc_cmd_xxx design note:
 *         These are one-shot setpoint writers, NOT loop functions.
 *         Calling them from here in a loop is WRONG for shell-driven use —
 *         they must be called by the command source (Shell / CAN handler) once,
 *         and foc_process_input_task() within will
 *         consume the target each cycle at 1kHz.
 */
static void _motor_task_entry(void *argument)
{
    motor_fsm_init(&_motor_ins);

    foc_calib_curr(&_motor_ins);

    for (;;) {
#if (TASK_PERF_ENABLE == 1)
        core_time_perf_start(&motor_task_perf);
#endif

        if (_motor_ins.axis_state == AXIS_STATE_CALIB) {
            if (_motor_ins.calib_state.js_state == JS_END ||
                _motor_ins.calib_state.enc_state == ENC_LUT_END) {
                _motor_ins.calib_state.js_state  = JS_IDLE;
                _motor_ins.calib_state.enc_state = ENC_IDLE;

                motor_fsm_dispatch(&_motor_ins, EV_CALIB_DONE);
            }
        }

        foc_fault_check(&_motor_ins);
        if (_motor_ins.fault_code != FAULT_NONE) {
            motor_fsm_dispatch(&_motor_ins, EV_FAULT_TRIP);
        }

        if (_motor_ins.axis_state == AXIS_STATE_RUNNING) {
            foc_process_input_task(&_motor_ins);
        }

#if (TASK_PERF_ENABLE == 1)
        core_time_perf_stop(&motor_task_perf);
        motor_time_us = (uint32_t)core_time_perf_get_us(&motor_task_perf);
        (void)(motor_time_us);
#endif

        osDelay(1);
    }
}

/**
 * @brief  safe monitoring and telemetry task — 100Hz
 *
 * @note   Responsibilities:
 *         1. NTC temperature sampling (slow ordinary ADC group)
 *         2. Deferred Flash parameter save (IRQ-safe ~20ms writes)
 *         3. Vofa+ telemetry output via DMA UART
 *
 *         This task runs at Normal priority, well below motor_task (High).
 *         Flash erase/write here will not interrupt the FOC ISR because
 *         __disable_irq() in bsp_flash is brief (~20ms) but this task
 *         context is preemptible — the FOC ISR still fires between individual
 *         word writes. Only the RTOS scheduler is briefly suspended.
 */
static void _motor_safe_task_entry(void *argument)
{
    /* Bind real-time data channels for Vofa+ visualization */
#if (TM_FEATURE_VOFA != 0U) && (VOFA_PROTOCOL == VOFA_PROTO_JUSTFLOAT)
    vofa_bind(0, &_motor_ins.meas_data.out_vel_rpm, VOFA_FLOAT);
    vofa_bind(1, &_motor_ins.meas_data.out_pos_turns, VOFA_FLOAT);
#endif
    // foc_sincos_selftest();
    for (;;) {
#if (TASK_PERF_ENABLE == 1)
        core_time_perf_start(&safe_task_perf);
#endif

        /* [1] Read NTC thermistors and bus voltage (ordinary ADC group, software triggered) */
        adc_read_ordinary(&_motor_ins);

        /* [2] Execute any pending Flash writes from calibration completion
         *     or shell 'store save' commands. Safe here in RTOS context. */
        _motor_handle_deferred_save(&_motor_ins);

#if (TASK_PERF_ENABLE == 1)
        core_time_perf_stop(&safe_task_perf);
        safe_time_us = (uint32_t)core_time_perf_get_us(&safe_task_perf);
        (void)(safe_time_us);
#endif

        osDelay(10);
    }
}

/* Public helper exported to product shell commands --------------------------------------------- */

/**
 * @brief  Prepare motor for calibration entry
 *
 * @note   Public so product shell command handlers can prepare CALIB entry.
 *         Atomically zeros all setpoints and resets all PIDs.
 *         If motor is running, targets are zeroed; the ramp/passthrough modes
 *         will command toward 0 for one more 1ms cycle, then axis_state will
 *         switch to CALIB and foc_process_input_task stops being called.
 *
 * @param  motor  Pointer to motor instance
 */
void motor_calib_prepare_entry(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    /* Zero all targets immediately — prevents old setpoints from being re-applied
     * when the axis returns to RUNNING after calibration */
    _motor_clear_setpoints(motor);

    /* Reset all PID integrators to prevent integrator windup during calib */
    pid_reset(&motor->ctrl_config.iq_pid);
    pid_reset(&motor->ctrl_config.id_pid);
    pid_reset(&motor->ctrl_config.vel_pid);
    pid_reset(&motor->ctrl_config.pos_pid);
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
