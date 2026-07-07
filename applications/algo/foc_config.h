/**
 * @file    foc_config.h
 * @brief   FOC System Configuration
 * @version v1.0.0
 * @date    2026.03.19
 * 
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* include -------------------------------------------------------------------------------------- */
#include "foc_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Task performance statistics */
#define TASK_PERF_ENABLE       (0U)

/* Section 1: Hardware Topology ----------------------------------------------------------------- */

/* Current sensing topology — select one. */
#define SHUNT_3PHASE           (0U) /* Three-shunt sensing (highest accuracy) */
#define SHUNT_2PHASE           (1U) /* Two-shunt (reconstruct third by KCL) */
#define SHUNT_1PHASE           (2U) /* Single-shunt (time-multiplex) */
#define CURRENT_SENSE_MODE     SHUNT_3PHASE

/* Phase ordering — determines Clarke transform current mapping. */
#define PHASE_ORDER_ABC        (0U) /* Standard ABC order*/
#define PHASE_ORDER_ACB        (1U) /* Swapped B/C (e.g. motor wired A-C-B) */
#define DEFAULT_PHASE_ORDER    PHASE_ORDER_ABC

/* Encoder mounting direction. */
#define DIR_CW                 (0U) /* Clockwise positive convention */
#define DIR_CCW                (1U) /* Counter-clockwise positive convention */
#define DEFAULT_DIR            DIR_CCW

/* Section 2: ADC & Sensing --------------------------------------------------------------------- */

/* ADC voltage reference (V). */
#define V_REF                  (2.8f)
#define V_REF_CURRENT          (1.4f)

/* ADC full-scale code (12-bit). */
#define ADC_FULL_SCALE         (4095.0f)

/* MP6543H */
// /* Current sensing: MP6543H internal amplifier, 3.6kΩ terminal resistor, K=4000 (typical). */
// #define R_REF                   (2000.0f)
// #define K_CURRENT               (4000.0f)

// /**
//  * I = [(ADC_Code × V_REF / 4095) - V_REF_CURRENT] × K_CURRENT / R_REF
//  * I ≈ 0.00076 × ADC_Code - 1.5556
//  */
// #define FAC_CURR(x)             ((x) * V_REF / ADC_FULL_SCALE * K_CURRENT / R_REF)

/* DRV8311H */
#define CURR_AMP_GAIN          0.5f // DRV8311H GAIN=0.5V/A

#define FAC_CURR(x)            ((x) * (V_REF / (ADC_FULL_SCALE * CURR_AMP_GAIN)))

/** Bus-voltage sensing: resistor divider R1 = 10 kΩ, R2 = 100 kΩ.
 *  Vbus = ADC_code × (Vref / ADC_FS) × (R1 + R2) / R1
 */
#define VIN_R1                 (10.0f) /* Lower divider resistor (kΩ) */
#define VIN_R2                 (36.0f) /* Upper divider resistor (kΩ) */
#define FAC_VBUS(x)            ((x) * ((V_REF / ADC_FULL_SCALE) * ((VIN_R1 + VIN_R2) / VIN_R1)))

/* Section 3: Timer & PWM ----------------------------------------------------------------------- */

/* APB2 / TIM1 clock frequency (Hz). at32f415kbu7_4: PLL → 150 MHz. */
#define TMR_CLK_HZ             (150000000U)
#define TMR_CLK_MHZ            (150U) /* Kept for legacy expressions. */

/** PWM carrier frequency (Hz). Centre-aligned so ADC samples at peak.
 *  Effective switching = 20 kHz; TIM1 overflow = 40 kHz (centre-aligned).
 */
#define PWM_FREQ_HZ            (20000U)
#define PWM_PERIOD_S           (1.0f / (float)PWM_FREQ_HZ) /* 50 μs */
#define PWM_PERIOD_CYCLES      (TMR_CLK_HZ / PWM_FREQ_HZ) /* 12000 cnt */
#define PWM_ARR                (PWM_PERIOD_CYCLES / 2U) /* TIM1 ARR */
#define PWM_DUTY_CENTER        (PWM_ARR / 2U) /* 50 % duty */

/* Backward-compatibility aliases used in older files. */
#define PWM_FREQ               PWM_FREQ_HZ
#define PWM_DT                 PWM_PERIOD_S

/* Section 4: Control-Loop Scheduling ----------------------------------------------------------- */

/* Loop frequencies and sample times derived from the PWM clock. */
#define CURR_LOOP_FREQ_HZ      (PWM_FREQ_HZ) /* 20 kHz — ADC ISR */
#define CURR_LOOP_TS           (PWM_PERIOD_S) /* 50 μs */

#define VEL_LOOP_FREQ_HZ       (5000U) /* 5 kHz */
#define VEL_LOOP_TS            (1.0f / (float)VEL_LOOP_FREQ_HZ) /* 200 μs */

#define POS_LOOP_FREQ_HZ       (1000U) /* 1 kHz */
#define POS_LOOP_TS            (1.0f / (float)POS_LOOP_FREQ_HZ) /* 1 ms */

#define TRAJ_TASK_FREQ_HZ      (1000U) /* 1 kHz  — RTOS task */
#define TRAJ_TASK_TS           (1.0f / (float)TRAJ_TASK_FREQ_HZ) /* 1 ms */

/* Section 5: Encoder / Position Sensor --------------------------------------------------------- */

/* SPI read mode for magnetic encoder.
 *  0 = Blocking (polling) — simple, portable, ~5μs per read in ISR
 *  1 = DMA pipeline — ISR zero-wait, DMA transfers in background
 */
#define ENCODER_SPI_DMA_ENABLE (0U)

/* KTH7823 absolute magnetic encoder — 16-bit single-turn. */
#define ENCODER_BITS           (16U)
#define ENCODER_CPR            ((int32_t)(1U << ENCODER_BITS)) /* 65536 counts/rev */

/** Non-linearity compensation LUT.
 *  Resolution = mechanical revolution / LUT_SIZE = 1/128 turn per bin.
 */
#define ENCODER_LUT_BITS       (7U)
#define ENCODER_LUT_SIZE       (1U << ENCODER_LUT_BITS) /* 128 entries */

/** PLL observer for velocity estimation (pll_enc v4.0+).
 *
 *  ENCODER_PLL_BW_HZ is the PLL natural frequency ωn in rad/s — NOT Hz.
 *  Despite the macro name (kept for backward compatibility), the value is ωn:
 *
 *    kp = 2 × ωn          (critically damped, ζ = 1)
 *    ki = ωn²
 *    f_-3dB = ωn / (2π)   → 500 rad/s → ~80 Hz actual bandwidth
 *
 *  To tune: increase ωn for faster velocity tracking (more noise);
 *           decrease for smoother but slower response.
 *  Typical range: 200 – 1000 (rad/s).
 */
#define ENCODER_PLL_BW_HZ      (200.0f) /* PLL natural freq ωn (rad/s). f_-3dB = ωn/(2π) ≈ 160 Hz */

/* Post-PLL velocity LPF for outer-loop velocity PID output. */
#define ENCODER_VEL_LPF_BW_HZ  (100.0f) /* Velocity output LPF (Hz). For outer-loop vel PID. */

/** Zero-velocity snap threshold factor.
 *  snap_thr = snap_factor × dt × ki  (counts/s)
 *  At snap_factor=0.5, ωn=500: snap_thr ≈ 6.25 counts/s ≈ 0.006 rpm output shaft.
 */
#define ENCODER_SNAP_FACTOR    (0.5f)

/* Backward-compatibility aliases (names kept, values updated above). */
#define ENCODER_PLL_BANDWIDTH  ENCODER_PLL_BW_HZ
#define ENCODER_LPF_BANDWIDTH  ENCODER_VEL_LPF_BW_HZ

/* Section 6: Motor Hardware Defaults ----------------------------------------------------------- */

/* Electrical parameters — updated by 'calib motor all'. */
#define DEFAULT_POLE_PAIRS     (1U) /* Pole pairs (pp) */
#define DEFAULT_RS             (1.5f) /* Phase resistance (Ω) */
#define DEFAULT_LS             (0.000115f) /* Phase inductance (H) */
#define DEFAULT_FLUX           (0.00358f) /* PM flux linkage (Wb) */
#define DEFAULT_JS             (0.000000034f) /* Motor-shaft inertia (kg·m²) */

#define DEFAULT_OFFSET         (2.477f) /* Mechanical offset (rad) */

/* D/Q-axis inductance split (set by motor datasheet if available). */
#define DEFAULT_LD             (0.0f) /* D-axis inductance (H) */
#define DEFAULT_LQ             (0.0f) /* Q-axis inductance (H) */
#define DEFAULT_LD_LQ_DIFF     (0.0f) /* Ld − Lq differential (H) */

/* Mechanical parameters. */
#define DEFAULT_INERTIA        (0.0f) /* Output-shaft inertia (kg·m²) — set if known */
#define DEFAULT_GEAR_RATIO     (6.0f) /* Reduction ratio  (motor:output) */
#define DEFAULT_GEAR_EFF       (0.708f) /* Gearbox efficiency */

/* Backward-compatibility aliases */
#define DEFAULT_ID             DEFAULT_LD
#define DEFAULT_IQ             DEFAULT_LQ
#define DEFAULT_IDIQ_DIFF      DEFAULT_LD_LQ_DIFF

/* Section 7: Protection Limits ----------------------------------------------------------------- */

/* Bus voltage protection (V). */
#define DEFAULT_VOLT_RATED     (8.0f) /* Nominal bus voltage */
#define DEFAULT_V_BUS_OV       (12.0f) /* Over-voltage trip threshold */
#define DEFAULT_V_BUS_UV       (4.0f) /* Under-voltage trip threshold */

/* Phase current protection (A). */
#define DEFAULT_CURR_RATED     (1.0f) /* Continuous rated current */
#define DEFAULT_CURR_PEAK      (1.5f) /* Peak current limit (hardware fuse) */

/* Output-shaft velocity protection. */
#define DEFAULT_VEL_RATED      (188.5f) /* Rated output speed  (rad/s) */
#define DEFAULT_VEL_PEAK       (209.44f) /* Peak output speed   (rad/s) */
#define DEFAULT_VEL_OVER       (226.2f) /* Overspeed threshold (rad/s) */

/* Output-shaft torque protection (N·m). */
#define DEFAULT_TOR_RATED      (0.030f) /* Rated output torque */
#define DEFAULT_TOR_PEAK       (0.055f) /* Peak output torque */

/* Temperature protection (°C). */
#define DEFAULT_MOTOR_TEMP     (80.0f) /* Motor winding over-temperature trip */
#define DEFAULT_DRIVER_TEMP    (80.0f) /* Driver MOSFET over-temperature trip */

/* Section 8: Motion Defaults — Ramp & Trajectory Planner --------------------------------------- */

/* Velocity ramp (VEL_RAMP input mode) — internal units: rad/s, rad/s².
 * Derived from 2.0 turns/s × 2π = 12.566 rad/s.
 */
#define DEFAULT_RAMP_VEL       (209.44f) /* Max velocity   (rad/s) */
#define DEFAULT_RAMP_ACCEL     (200.0f) /* Acceleration   (rad/s²) */
#define DEFAULT_RAMP_DECEL     (200.0f) /* Deceleration   (rad/s²) */

/* Trapezoidal trajectory planner (TRAP_TRAJ input mode) — internal units: rad/s, rad/s².
 * traj_trap now operates in the same rad domain as the rest of the control stack.
 * Derived from 2.0 turns/s × 2π = 12.566 rad/s.
 */
#define DEFAULT_TRAJ_VEL       (209.44f) /* Peak velocity  (rad/s) */
#define DEFAULT_TRAJ_ACCEL     (200.0f) /* Acceleration   (rad/s²) */
#define DEFAULT_TRAJ_DECEL     (200.0f) /* Deceleration   (rad/s²) */
#define DEFAULT_TRAJ_JERK      (0.0f) /* Jerk limit (0 = trapezoidal, not S) */

/* Section 9: Controller Defaults — PID Gains & Filter Bandwidths ------------------------------- */

/** Current controller bandwidth — drives automatic Kp/Ki computation.
 *  Range: 500 – 3000 Hz.  Higher = faster but more noise sensitive.
 */
#define DEFAULT_CURR_BW_HZ     (20.0f) /* Current-loop closed-loop bandwidth (Hz). */
#define DEFAULT_CURR_BW        DEFAULT_CURR_BW_HZ /* Backward-compat alias */

#define DEFAULT_D_AXIS_BW_RATIO  (0.2f) /* D-axis bandwidth ratio relative to Q-axis 0.0f ~ 1.0f*/

/* Current PID (id / iq loops).  Kp/Ki are normally auto-computed; leave at 0. */
#define DEFAULT_CURR_PID_KP      (0.0f)
#define DEFAULT_CURR_PID_KI      (0.0f)
#define DEFAULT_CURR_PID_KD      (0.0f)
#define DEFAULT_CURR_MAX_OUT     (DEFAULT_VOLT_RATED * FOC_INV_SQRT3) /* Vbus/√3 */

/** Current measurement LPF (applied to Id and Iq after Park transform).
 *  Must be well above current-loop bandwidth to avoid adding phase lag.
 *  Typical: 4x – 10x DEFAULT_CURR_BW_HZ.
 */
#define DEFAULT_CUTOFF_HZ        (DEFAULT_CURR_BW_HZ * 5.0f) /* 1000 Hz */
#define DEFAULT_SAMPLING_HZ      ((float)PWM_FREQ_HZ) /* 20000 Hz */

/* Velocity controller bandwidth (Mechanical - Slow, depends on inertia and gearbox) */
#define DEFAULT_VEL_BW_HZ        (5.0f)
#define DEFAULT_VEL_BW           DEFAULT_VEL_BW_HZ /* Backward-compat alias */

/* Velocity PID defaults (output-shaft rad/s → motor Iq A). */
#define DEFAULT_VEL_PID_KP       (2.0f)
#define DEFAULT_VEL_PID_KI       (0.5f)
#define DEFAULT_VEL_PID_KD       (0.0f)
#define DEFAULT_VEL_MAX_OUT      (DEFAULT_CURR_RATED) /* Iq clamp (A) */

/** Position controller bandwidth (Mechanical - Fast, depends on inertia and gearbox) */
#define DEFAULT_POS_BW_HZ        (5.0f)
#define DEFAULT_POS_BW           DEFAULT_POS_BW_HZ /* Backward-compat alias */
#define DEFAULT_POS_VEL_BW_RATIO (0.5f)

/* Position PID defaults (output-shaft rad → velocity rad/s).  */
#define DEFAULT_POS_PID_KP       (50.0f)
#define DEFAULT_POS_PID_KI       (0.0f)
#define DEFAULT_POS_PID_KD       (0.0f)
#define DEFAULT_POS_MAX_OUT      (DEFAULT_VEL_RATED) /* vel clamp (rad/s) */

/* Section 10: Calibration Parameters ----------------------------------------------------------- */

/** Startup current-offset calibration.
 *  Blocks for CALIB_CURR_DUR × 1 ms = 2000 ms while averaging ADC readings
 *  with no PWM output to determine the op-amp zero-offset for each phase.
 */
#define CALIB_CURR_DUR           (2000U) /* Averaging duration (PWM cycles) */

/* Valid-flag sentinel: 0 means encoder calib has NOT been loaded yet. */
#define CALIB_VALID_FLAG         (0U)

/* Bandwidth scaling factor κ for cascade gain design (vel from curr bandwidth). */
#define CALIB_K_DELTA            (4.0f)

/* Resistance (Rs) calibration ------------------------------------------------------------------ */
#define CALIB_RS_STEPS           (4U) /* Total DC steps  */
#define CALIB_RS_HALF_STEPS      (CALIB_RS_STEPS / 2U) /* Steps per polarity */
#define CALIB_RS_SAMPLES         (100U) /* Samples per step */
#define CALIB_RS_CURR_MAX        (1.0f) /* Max test current (A) */
#define CALIB_RS_CURR_THR        (0.001f) /* Convergence threshold (A) */
#define CALIB_RS_VOLT_STEP       (0.005f) /* Voltage increment (V) */
#define CALIB_RS_WAIT_CNT        (100U) /* Stabilisation cycles */
#define CALIB_RS_ALIGN_VOLT      (1.0f) /* Alignment voltage (V) */
#define CALIB_RS_ALIGN_CNT       ((float)PWM_FREQ_HZ * 0.2f) /* Alignment duration (PWM cycles) */

/* Backward-compat aliases */
#define DEFAULT_RS_STEPS         CALIB_RS_STEPS
#define DEFAULT_RS_HSTEP         CALIB_RS_HALF_STEPS
#define DEFAULT_RS_SAMPS         CALIB_RS_SAMPLES
#define DEFAULT_RS_CURR_MAX      CALIB_RS_CURR_MAX
#define DEFAULT_RS_CURR_THRE     CALIB_RS_CURR_THR
#define DEFAULT_RS_VOLT_STEP     CALIB_RS_VOLT_STEP
#define DEFAULT_RS_WAIT_CNT      CALIB_RS_WAIT_CNT
#define DEFAULT_RS_ALIGN_VOLT    CALIB_RS_ALIGN_VOLT
#define DEFAULT_RS_ALIGN_CNT     CALIB_RS_ALIGN_CNT

/* Inductance (Ls) calibration ------------------------------------------------------------------ */
#define CALIB_LS_CYCLES          (500U) /* Test-pulse count per polarity */
#define CALIB_LS_TS              (0.0001f) /* dI/dt integration Ts (s) */
#define CALIB_LS_VD_POS          (+0.6f) /* Positive d-axis test voltage (V) */
#define CALIB_LS_VD_NEG          (-0.6f) /* Negative d-axis test voltage (V) */
#define CALIB_LS_VQ_POS          (+0.6f)
#define CALIB_LS_VQ_NEG          (-0.6f)
#define CALIB_LS_CNT_THR         (10U) /* Voltage-on duration (cycles) */

/* Backward-compat aliases */
#define DEFAULT_LS_CYCLE         CALIB_LS_CYCLES
#define DEFAULT_LS_TS            CALIB_LS_TS
#define DEFAULT_VD_PLUS          CALIB_LS_VD_POS
#define DEFAULT_VD_MINU          CALIB_LS_VD_NEG
#define DEFAULT_VQ_PLUS          CALIB_LS_VQ_POS
#define DEFAULT_VQ_MINU          CALIB_LS_VQ_NEG
#define DEFAULT_LS_CNT_THRE      CALIB_LS_CNT_THR

/* Flux linkage (Ψm) calibration ---------------------------------------------------------------- */
#define CALIB_FLUX_IQ_A          (1.0f) /* Test Iq during BEMF measurement (A) */
#define CALIB_FLUX_WE_LOW        (500.0f) /* Low  test speed  ωe (rad/s) */
#define CALIB_FLUX_WE_HIGH       (1000.0f) /* High test speed  ωe (rad/s) */
#define CALIB_FLUX_WE_ACCEL      (500.0f) /* Speed ramp rate  (rad/s²) */
#define CALIB_FLUX_TS            CURR_LOOP_TS /* Integration Ts (s) */
#define CALIB_FLUX_WAIT_CNT      ((uint32_t)(PWM_FREQ_HZ * 0.8f)) /* Speed-settle wait */
#define CALIB_FLUX_SAMP_CNT      ((uint32_t)(PWM_FREQ_HZ * 1.0f)) /* Averaging window */

/* Backward-compat aliases */
#define DEFAULT_FLUX_IQ          CALIB_FLUX_IQ_A
#define DEFAULT_FLUX_WE_L        CALIB_FLUX_WE_LOW
#define DEFAULT_FLUX_WE_H        CALIB_FLUX_WE_HIGH
#define DEFAULT_FLUX_WE_ACC      CALIB_FLUX_WE_ACCEL
#define DEFAULT_FLUX_TS          CALIB_FLUX_TS
#define DEFAULT_FLUX_WAIT_CNT    CALIB_FLUX_WAIT_CNT
#define DEFAULT_FLUX_SAMP_CNT    CALIB_FLUX_SAMP_CNT

/* Inertia (Js) calibration --------------------------------------------------------------------- */
#define CALIB_JS_IQ_AMP          (1.0f) /* Sinusoidal excitation current amplitude (A) */
#define CALIB_JS_FREQ_HZ         (2.0f) /* Excitation frequency (Hz) */
#define CALIB_JS_WAIT_CYCLES     (1U) /* Number of cycles to wait before integration */
#define CALIB_JS_CALC_CYCLES     (1U) /* Number of Fourier integration cycles */
#define CALIB_JS_TS              CURR_LOOP_TS /* Integration time step (s) */

#define DEFAULT_JS_JIQ_MAX       CALIB_JS_IQ_AMP
#define DEFAULT_JS_JFS           CALIB_JS_FREQ_HZ
#define DEFAULT_JS_WAIT_CYCLES   CALIB_JS_WAIT_CYCLES
#define DEFAULT_JS_CALC_CYCLES   CALIB_JS_CALC_CYCLES
#define DEFAULT_JS_TS            CALIB_JS_TS

/* Encoder LUT calibration ---------------------------------------------------------------------- */
#define CALIB_LUT_SIZE           ENCODER_LUT_SIZE
#define CALIB_WINDOW_SIZE        (ENCODER_LUT_SIZE / 2U)

/* Rotor alignment duration before LUT sweep (PWM cycles). */
#define CALIB_ALIGN_DURATION     ((uint32_t)PWM_FREQ_HZ)

/* Number of electrical-angle steps per LUT bin during sweep. */
#define CALIB_SCAN_STEPS         (40U)

/* Open-loop voltage used during alignment and LUT scan (V). */
#define CALIB_VOLTAGE            (2.0f)

/* Open-loop acceleration per step (normalised electrical angle increment). */
#define CALIB_ACCEL              (0.002f)

/* Position hold tolerance and valid angle window for polarity detection. */
#define CALIB_CHECK_POS_THR      (0.001f)
#define CALIB_CHECK_ANGLE_MIN    (FOC_PI * 0.50f)
#define CALIB_CHECK_ANGLE_MAX    (FOC_PI * 0.55f)
#define CALIB_CHECK_POS_TH       CALIB_CHECK_POS_THR /* Backward-compat aliases */

#define MOTOR_POLE_PAIRS_MAX     (2U) /* Hardware safety limits */

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
