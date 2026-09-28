/**
 * @file    motor_task_config.h
 * @brief   Application-owned motor task budget configuration
 * @version v1.0.0
 * @date    2026.05.22
 *
 * Design overview
 * ---------------
 * This header keeps the FalconFoc motor-task stack budgets explicit at the
 * application config layer. It is intentionally macro-only: no runtime state
 * and no task behavior live here.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "config/tm_core_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/* Increased stack budgets for FOC ISR-coupled tasks.  The safe task runs
 * adc_read_ordinary_dma() plus deferred flash save bookkeeping; the motor
 * task dispatches the FSM and calibration completion callbacks. */
#define TM_MOTOR_TASK_STACK_BYTES      (512U * 4U)
#define TM_MOTOR_SAFE_TASK_STACK_BYTES (256U * 4U)

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
#define TM_MOTOR_FOC_ADC_DIVIDER       (4U)
#else
#define TM_MOTOR_FOC_ADC_DIVIDER       (2U)
#endif

/* Bench-only last-resort energy bound.  The generated TMR1/ADC trigger is
 * approximately 20 kHz.  CMake supplies 50 ms for the normal bench image and
 * may supply 200 ms or 400 ms only for explicitly named bench-long images.
 * This guard is deliberately independent of Shell servicing and does not
 * change the product image. */
#if !defined(TM_MOTOR_BENCH_RUN_BUDGET_MS)
#define TM_MOTOR_BENCH_RUN_BUDGET_MS   (50U)
#endif
#if (TM_MOTOR_BENCH_RUN_BUDGET_MS != 50U) && \
    (TM_MOTOR_BENCH_RUN_BUDGET_MS != 200U) && \
    (TM_MOTOR_BENCH_RUN_BUDGET_MS != 400U) && \
    (TM_MOTOR_BENCH_RUN_BUDGET_MS != 1000U) && \
    (TM_MOTOR_BENCH_RUN_BUDGET_MS != 30000U)
#error "TM_MOTOR_BENCH_RUN_BUDGET_MS must be 50U/200U/400U/1000U/30000U in bench firmware."
#endif
#define TM_MOTOR_BENCH_MAX_RUN_MS      (TM_MOTOR_BENCH_RUN_BUDGET_MS)
#define TM_MOTOR_BENCH_ADC_TICK_HZ     (20000U)
#define TM_MOTOR_BENCH_MAX_RUN_TICKS   ((TM_MOTOR_BENCH_MAX_RUN_MS * \
                                         TM_MOTOR_BENCH_ADC_TICK_HZ) / 1000U)
#define TM_MOTOR_BENCH_HEARTBEAT_TIMEOUT_MS    (1500U)
#define TM_MOTOR_BENCH_HEARTBEAT_TIMEOUT_TICKS \
    ((TM_MOTOR_BENCH_HEARTBEAT_TIMEOUT_MS * TM_MOTOR_BENCH_ADC_TICK_HZ) / 1000U)

/* The prior 3.14 rad/s command is only about 4.3 mechanical RPM for this
 * seven-pole-pair motor and produces negligible angle travel in a short
 * pulse.  Use one electrical-cycle-scale window for the first meaningful
 * open-loop rotation trial. */
#define TM_MOTOR_BENCH_OPEN_LOOP_MECH_RPM             (30.0f)
#define TM_MOTOR_BENCH_OPEN_LOOP_WE_RAD_S             \
    (FOC_2PI * (float)DEFAULT_POLE_PAIRS * TM_MOTOR_BENCH_OPEN_LOOP_MECH_RPM / 60.0f)
#define TM_MOTOR_BENCH_OPEN_LOOP_ELECTRICAL_PERIOD_MS \
    (60000.0f / ((float)DEFAULT_POLE_PAIRS * TM_MOTOR_BENCH_OPEN_LOOP_MECH_RPM))
#define TM_MOTOR_BENCH_OPEN_LOOP_RUN_MS              (400U)

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
