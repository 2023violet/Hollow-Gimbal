/**
 * @file    motor_task.c
 * @brief   FalconFoc motor RTOS task owner
 * @version v2.2.0
 * @date    2026.05.22
 *
 * Implementation role
 * -------------------
 * - owns the motor task/thread bring-up sequence
 * - owns ISR-to-task cadence boundaries for the live motor instance
 * - delegates product meaning to runtime/fsm/periph owners instead of
 *   re-implementing product policy locally
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_task.h"

#include "../runtime/foc_motion_runtime.h"
#include "../runtime/motor_calibration_runtime.h"
#include "../runtime/motor_fault_runtime.h"
#include "../runtime/motor_runtime.h"
#include "motor_fsm.h"
#if (TM_FEATURE_VOFA != 0U)
#include "vofa.h"
#endif
#include "core_time.h"
#include "foc_calib.h"
#include "foc_calib_enc.h"
#include "foc_core.h"
#include "foc_ctrl.h"
#include "motor_drv.h"
#include "motor_task_config.h"
#include "shell_task.h"
#include "tm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER
#error \
    "motor_task.c requires THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER from platform compile definitions."
#endif

#ifndef THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE
#error \
    "motor_task.c requires THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE from platform compile definitions."
#endif

/* Private Types -------------------------------------------------------------------------------- */

/* Private State -------------------------------------------------------------------------------- */

static osThreadId_t motor_task_handle = NULL;
static osThreadId_t motor_safe_task_handle = NULL;

static const osThreadAttr_t motor_task_attr = {
    .name = "motor_task",
    .stack_size = TM_MOTOR_TASK_STACK_BYTES,
    .priority = (osPriority_t)osPriorityHigh,
};

static const osThreadAttr_t safe_task_attr = {
    .name = "motor_safe",
    .stack_size = TM_MOTOR_SAFE_TASK_STACK_BYTES,
    .priority = (osPriority_t)osPriorityNormal,
};

#if (TASK_PERF_ENABLE == 1)
static struct core_time_perf adc_iqr_perf;
static struct core_time_perf tim6_iqr_perf;
static struct core_time_perf motor_task_perf;
static struct core_time_perf safe_task_perf;

static uint32_t adc_time_us = 0;
static uint32_t tim6_time_us = 0;
static uint32_t motor_time_us = 0;
static uint32_t safe_time_us = 0;
#endif

struct motor_dev _motor_ins;

/* Private helpers ------------------------------------------------------------------------------ */

static void _motor_task_entry(void *argument);
static void _motor_safe_task_entry(void *argument);

/* Public functions ----------------------------------------------------------------------------- */

void motor_task_init(void)
{
#if (SHELL_ENABLE == 1)
    shell_task_init();
#endif

    motor_runtime_init(&_motor_ins);
    motor_runtime_attach(&_motor_ins);

    motor_task_handle = osThreadNew(_motor_task_entry, NULL, &motor_task_attr);
    motor_safe_task_handle = osThreadNew(_motor_safe_task_entry, NULL, &safe_task_attr);
}
INIT_EXPORT(motor_task_init, EXPORT_LEVEL_APP);

/* Public Functions ----------------------------------------------------------------------------- */

void ADC1_IRQHandler(void)
{
    if (adc_interrupt_flag_get(ADC1, ADC_PCCE_FLAG) == RESET) {
        return;
    }
    adc_flag_clear(ADC1, ADC_PCCE_FLAG);

    adc_read_phases(&_motor_ins);
#if (TASK_PERF_ENABLE == 1)
    core_time_perf_start(&adc_iqr_perf);
#endif
    encoder_update(&_motor_ins.enc_pri);
#if (TASK_PERF_ENABLE == 1)
    core_time_perf_stop(&adc_iqr_perf);
    adc_time_us = (uint32_t)core_time_perf_get_us(&adc_iqr_perf);
    (void)(adc_time_us);
#endif

    switch (_motor_ins.axis_state) {
    case AXIS_STATE_CALIB:
        foc_calib_loop(&_motor_ins);
        break;
    case AXIS_STATE_RUNNING:
        foc_current_loop_step(&_motor_ins);
        break;
    default:
        break;
    }
}

void THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER(void)
{
    if (tmr_interrupt_flag_get(THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE, TMR_OVF_FLAG) == RESET) {
        return;
    }
    tmr_flag_clear(THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE, TMR_OVF_FLAG);

#if (TASK_PERF_ENABLE == 1)
    core_time_perf_start(&tim6_iqr_perf);
#endif

    if (_motor_ins.axis_state == AXIS_STATE_RUNNING) {
        foc_outer_loop_step(&_motor_ins);
    }

#if (TASK_PERF_ENABLE == 1)
    core_time_perf_stop(&tim6_iqr_perf);
    tim6_time_us = (uint32_t)core_time_perf_get_us(&tim6_iqr_perf);
    (void)(tim6_time_us);
#endif
}

/* Private Helpers ------------------------------------------------------------------------------ */

static void _motor_task_entry(void *argument)
{
    TM_UNUSED(argument);

    motor_fsm_init(&_motor_ins);
    foc_calib_curr(&_motor_ins);

    for (;;) {
#if (TASK_PERF_ENABLE == 1)
        core_time_perf_start(&motor_task_perf);
#endif

        if (_motor_ins.axis_state == AXIS_STATE_CALIB) {
            if ((_motor_ins.calib_state.js_state == JS_END) ||
                (_motor_ins.calib_state.enc_state == ENC_LUT_END)) {
                _motor_ins.calib_state.js_state = JS_IDLE;
                _motor_ins.calib_state.enc_state = ENC_IDLE;
                motor_fsm_dispatch(&_motor_ins, EV_CALIB_DONE);
            }
        }

        foc_fault_check(&_motor_ins);
        if ((_motor_ins.fault_code != FAULT_NONE) && (_motor_ins.axis_state != AXIS_STATE_FAULT)) {
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

static void _motor_safe_task_entry(void *argument)
{
    TM_UNUSED(argument);

    /* Bind VOFA channels for telemetry.
     * Only bind when JustFloat is selected; FireWater output is handled by the
     * shell/console path in the full-diag profile.
     *
     * Debug channel layout for voltage-open-loop bring-up:
     *   ch0: dtc_a       - SVPWM duty cycle phase A
     *   ch1: dtc_b       - SVPWM duty cycle phase B
     *   ch2: dtc_c       - SVPWM duty cycle phase C
     *   ch3: i_q         - measured torque current (A)
     *   ch4: iq_set      - iq-axis current command (A)
     *   ch5: out_vel_rpm - output velocity (rpm)
     *   ch6: out_pos_turns - position (turns)
     */
#if (TM_FEATURE_VOFA != 0U) && (VOFA_PROTOCOL == VOFA_PROTO_JUSTFLOAT)
    vofa_bind(0, &_motor_ins.foc_calc.dtc_a, VOFA_FLOAT);
    vofa_bind(1, &_motor_ins.foc_calc.dtc_b, VOFA_FLOAT);
    vofa_bind(2, &_motor_ins.foc_calc.dtc_c, VOFA_FLOAT);
    vofa_bind(3, &_motor_ins.foc_calc.i_q, VOFA_FLOAT);
    vofa_bind(4, &_motor_ins.ctrl_state.iq_set, VOFA_FLOAT);
    vofa_bind(5, &_motor_ins.meas_data.out_vel_rpm, VOFA_FLOAT);
    vofa_bind(6, &_motor_ins.meas_data.out_pos_turns, VOFA_FLOAT);
#endif

    for (;;) {
#if (TASK_PERF_ENABLE == 1)
        core_time_perf_start(&safe_task_perf);
#endif

        adc_read_ordinary(&_motor_ins);
        motor_runtime_handle_deferred_save(&_motor_ins);

#if (TASK_PERF_ENABLE == 1)
        core_time_perf_stop(&safe_task_perf);
        safe_time_us = (uint32_t)core_time_perf_get_us(&safe_task_perf);
        (void)(safe_time_us);
#endif

        osDelay(10);
    }
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
