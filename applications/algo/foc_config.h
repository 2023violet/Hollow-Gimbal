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
#define V_REF                  (3.3f)
#define V_REF_CURRENT          (1.65f)
#define FOC_VBUS_NUMERIC_MIN_V (1.0f) /* Prevent invalid SVPWM normalisation */

/* SVPWM per-phase duty envelope.  2026-09-26 bench evidence: sustained duty
 * above ~0.79 latches the DRV8311H half-bridges off (phase-specific and
 * stochastic — worst on the CH2/DRV-B pair, faster at higher duty; reproduced
 * on firmware phases A/B/C and under both bench PWM maps, so it follows the
 * TIM-channel <-> DRV half-bridge pairing, not the firmware phase index).
 * Duty <= 0.72 never latched (I-f drag ran 20 s+).  The [0.28, 0.72] window
 * keeps every control mode inside the proven-safe envelope; at an 11.9 V bus
 * it caps the phase-voltage amplitude near 2.6 V, which covers the 30 rpm
 * closed-loop operating point (vq <= 2.5 V).  Raise only with new bench
 * evidence, and expect the DRV latch to return above ~0.79. */
#define FOC_SVPWM_DUTY_MIN     (0.28f)
#define FOC_SVPWM_DUTY_MAX     (0.72f)

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
#define VIN_R2                 (100.0f) /* Upper divider resistor (kΩ) */
#define FAC_VBUS(x)            ((x) * ((V_REF / ADC_FULL_SCALE) * ((VIN_R1 + VIN_R2) / VIN_R1)))

/* Section 3: Timer & PWM ----------------------------------------------------------------------- */

/* APB2 / TIM1 clock frequency (Hz). at32f415kbu7_4: PLL → 150 MHz. */
#define TMR_CLK_HZ             (150000000U)
#define TMR_CLK_MHZ            (150U) /* Kept for legacy expressions. */

/** FOC current-loop cadence (Hz).
 *
 *  The legacy PWM_FREQ_HZ name denotes the software FOC step cadence here;
 *  it is not the generated TMR1 carrier frequency. TMR1 uses ARR=3749 with
 *  a 150 MHz timer clock and two-way counting, giving an approximately 20 kHz
 *  carrier and ADC trigger. motor_task.c executes one FOC step every second
 *  ADC tick in the product configuration; the selected PWM_FREQ_HZ value is
 *  the control-loop sample rate used by PWM_PERIOD_S, PID dt and the
 *  current-feedback LPF calculations. The bench configuration deliberately
 *  selects a lower FOC step cadence to bound short-pulse runtime.
 *
 *  All time-constant derivations below (PWM_PERIOD_S / CURR_LOOP_TS / PID dt /
 *  calibration cycle counts) follow the selected current-loop cadence, so the
 *  numbers stay self-consistent without touching the generated wk_* config.
 */
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
#define PWM_FREQ_HZ            (5000U) /* Bench FOC step; TMR1 carrier remains ~20 kHz. */
#else
#define PWM_FREQ_HZ            (10000U) /* Legacy name: FOC step, not TMR1 carrier. */
#endif
#define PWM_PERIOD_S           (1.0f / (float)PWM_FREQ_HZ) /* Derived from the FOC step cadence. */
#define PWM_PERIOD_CYCLES      (TMR_CLK_HZ / PWM_FREQ_HZ) /* 15000 cnt */
#define PWM_ARR                (PWM_PERIOD_CYCLES / 2U) /* TIM1 ARR */
#define PWM_DUTY_CENTER        (PWM_ARR / 2U) /* 50 % duty */

/* Backward-compatibility aliases used in older files. */
#define PWM_FREQ               PWM_FREQ_HZ
#define PWM_DT                 PWM_PERIOD_S

/* Section 4: Control-Loop Scheduling ----------------------------------------------------------- */

/* Loop frequencies and sample times derived from the PWM clock. */
#define CURR_LOOP_FREQ_HZ      (PWM_FREQ_HZ) /* Same as the selected FOC step cadence. */
#define CURR_LOOP_TS           (PWM_PERIOD_S) /* Derived FOC step period. */

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
#define ENCODER_PLL_BW_HZ      (500.0f) /* PLL natural freq ωn (rad/s). f_-3dB = ωn/(2π) ≈ 160 Hz */

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

/* Electrical parameters — updated by 'calib motor all'.
 * 2804 hollow-shaft gimbal motor (12N14P, star), datasheet values:
 *   phase resistance Rs = 2.55 Ω  (line-to-line 5.1 Ω / 2)
 *   phase inductance Ls = 0.86 mH (line-to-line 2.8 mH / ~3)
 *   flux linkage      = 0.0035 Wb
 *   rotor inertia     = 3700 g·mm² = 3.7e-6 kg·m²
 */
#define DEFAULT_POLE_PAIRS     (7U) /* Pole pairs (pp) */
#define DEFAULT_RS             (2.55f) /* Phase resistance (Ω) */
#define DEFAULT_LS             (0.00086f) /* Phase inductance (H) */
#define DEFAULT_FLUX           (0.0035f) /* PM flux linkage (Wb) */
#define DEFAULT_JS             (0.0000037f) /* Motor-shaft inertia (kg·m²) */

#define DEFAULT_OFFSET         (0.0f) /* Mechanical offset (rad) */

/* D/Q-axis inductance split (set by motor datasheet if available).
 * Surface-PM magnet ring: Ld ≈ Lq ≈ Ls.
 */
#define DEFAULT_LD             (DEFAULT_LS) /* D-axis inductance (H) */
#define DEFAULT_LQ             (DEFAULT_LS) /* Q-axis inductance (H) */
#define DEFAULT_LD_LQ_DIFF     (0.0f) /* Ld − Lq differential (H) */

/* Mechanical parameters. */
#define DEFAULT_INERTIA        (0.0f) /* Output-shaft inertia (kg·m²) — set if known */
#define DEFAULT_GEAR_RATIO     (1.0f) /* Reduction ratio  (motor:output) */
#define DEFAULT_GEAR_EFF       (1.0f) /* Gearbox efficiency */

/* Backward-compatibility aliases */
#define DEFAULT_ID             DEFAULT_LD
#define DEFAULT_IQ             DEFAULT_LQ
#define DEFAULT_IDIQ_DIFF      DEFAULT_LD_LQ_DIFF

/* Section 7: Protection Limits ----------------------------------------------------------------- */

/* Bus voltage protection (V).  Input supply = 12 V; datasheet working range
 * 7.4–16 V, absolute peak 20 V.  UV trip below working floor, OV trip below
 * the 20 V absolute peak with margin.
 */
#define DEFAULT_VOLT_RATED     (12.0f) /* Nominal bus voltage */
#define DEFAULT_V_BUS_OV       (17.0f) /* Over-voltage trip threshold */
#define DEFAULT_V_BUS_UV       (7.4f) /* Under-voltage trip threshold */

/* Phase current protection (A).  Datasheet: 0.5 A rated / 2 A peak.
 * 0.71 A(measured) corresponds to the datasheet 0.5 A TRUE winding current:
 * the bench-verified sampling chain reports ~0.7x of the true phase current
 * (k factor, see docs/planning open-loop plan 7.2), so the command cap must
 * be scaled up to deliver the rated TRUE current. Fast OCP stays at 1.0 A. */
#define DEFAULT_CURR_RATED     (1.20f) /* Command cap (measured A). 2026-08-31 bench: measured ≈ true (R≈3.5 Ω); 1.2 A ≈ 52 mN·m — 2x the detent stall load; ordering 1.2 < 1.5 < 1.6. */
#define DEFAULT_CURR_PEAK      (2.0f) /* Peak current limit (hardware fuse) */

/* Bring-up protection envelope.  The 2804 reference specifies 0.5 A rated
 * and 2.0 A maximum current.  These provisional diagnostic limits leave
 * margin below the motor maximum while allowing the rated-current startup
 * transient required by a meaningful open-loop test.  They are not a
 * product thermal certification and must be revisited with measured current
 * and temperature evidence. */
#define FOC_FAST_CURRENT_TRIP_A (1.60f) /* Immediate per-phase trip (A). 2026-08-31 bench: raised from 1.0 A — the 2804 detent/stiction needs the datasheet 2 A peak headroom; 0.5 A continuous rating unchanged in thermal terms for short bench runs. */
#define FOC_WARN_CURRENT_A      (1.50f) /* Task-level sustained-current trip (A). Ordering: command 1.2 < sustained 1.5 < fast 1.6. */
#define FOC_STALL_CURRENT_A     (1.50f) /* Closed-loop stall current (A). Ordering: command 1.2 < stall 1.5 < fast 1.6. */
#define FOC_STALL_SPEED_RPM     (5.0f)  /* Closed-loop stall window only */
#define FOC_STALL_TRIP_MS       (200U)  /* Low-speed/high-current trip time */
#define FOC_WARNING_TRIP_MS     (3000U) /* Sustained warning trip time */

/* Keep all calibration routines that energize the motor disabled during the
 * first bring-up profile.  Current-offset calibration remains non-energized. */
#ifndef FOC_ENERGIZED_CALIB_ENABLE
#define FOC_ENERGIZED_CALIB_ENABLE (0U)
#endif

/* Output-shaft velocity protection.
 * No-load speed = 2600 RPM @ 12 V = 272.27 rad/s; Ke = 0.0434 V/(rad/s) →
 * ideal no-load ceiling at 12 V = 276.5 rad/s.
 */
#define DEFAULT_VEL_RATED      (272.27f) /* Rated output speed  (rad/s) */
#define DEFAULT_VEL_PEAK       (276.5f) /* Peak output speed   (rad/s) */
#define DEFAULT_VEL_OVER       (300.0f) /* Overspeed threshold (rad/s) */

/* Output-shaft torque protection (N·m).
 * Rated 300 g·cm = 0.03 Nm; Kt = Ke = 0.0434 Nm/A → peak = 2 A × 0.0434 = 0.0868 Nm.
 */
#define DEFAULT_TOR_RATED      (0.030f) /* Rated output torque */
#define DEFAULT_TOR_PEAK       (0.0868f) /* Peak output torque */

/* Temperature protection (°C). */
#define DEFAULT_MOTOR_TEMP     (80.0f) /* Motor winding over-temperature trip */
#define DEFAULT_DRIVER_TEMP    (80.0f) /* Driver MOSFET over-temperature trip */

/* Section 8: Motion Defaults — Ramp & Trajectory Planner --------------------------------------- */

/* Velocity ramp (VEL_RAMP input mode) — internal units: rad/s, rad/s².
 * Max velocity set to motor rated no-load speed (2600 RPM @ 12 V).
 */
#define DEFAULT_RAMP_VEL       (272.27f) /* Max velocity   (rad/s) */
#define DEFAULT_RAMP_ACCEL     (200.0f) /* Acceleration   (rad/s²) */
#define DEFAULT_RAMP_DECEL     (200.0f) /* Deceleration   (rad/s²) */

/* Trapezoidal trajectory planner (TRAP_TRAJ input mode) — internal units: rad/s, rad/s².
 * traj_trap now operates in the same rad domain as the rest of the control stack.
 */
#define DEFAULT_TRAJ_VEL       (272.27f) /* Peak velocity  (rad/s) */
#define DEFAULT_TRAJ_ACCEL     (200.0f) /* Acceleration   (rad/s²) */
#define DEFAULT_TRAJ_DECEL     (200.0f) /* Deceleration   (rad/s²) */
#define DEFAULT_TRAJ_JERK      (0.0f) /* Jerk limit (0 = trapezoidal, not S) */

/* Voltage open-loop (CTRL_MODE_VOLT): continuous electrical-angular-velocity
 * ramp applied in the ADC ISR so the rotor can pull into synchronism.
 * 0 -> 62.83 rad/s elec in ~2.5 s at the default rate. 25 rad/s² gives a
 * ~0.9 s pull-in ramp to 22 rad/s elec (30 rpm at 7 pole pairs) so the rotor
 * locks at low speed before the field runs away; bench-verified 2026-08-30
 * (60 rad/s² out-ran the rotor before it could lock).
 */
#define DEFAULT_VOLT_WE_ACCEL  (25.0f) /* Electrical accel limit (rad/s²). 2026-09-26
                                        * bench A/B: an instantaneous we launch
                                        * (5000 tested) drags the rotor a few turns
                                        * asynchronously then desyncs — same freeze
                                        * physics as the 25 rad/s² ramp. Kept at 25. */

/* Section 9: Controller Defaults — PID Gains & Filter Bandwidths ------------------------------- */

/** Current controller bandwidth — drives automatic Kp/Ki computation.
 *  The 250 Hz bring-up value is intentionally conservative for the 10 kHz
 *  soft-float current step; higher values require a measured timing margin.
 */
/* Current-loop bandwidth: the FOC step runs at 10 kHz on this soft-float
 * port (dt = 100 us). The continuous-form PI integration needs wc*dt << 1;
 * 250 Hz gives wc*dt ~= 0.16 rad/step. The previous 1000 Hz default
 * (wc*dt = 0.63) destabilised the loop on this rig. */
#define DEFAULT_CURR_BW_HZ     (250.0f) /* Current-loop closed-loop bandwidth (Hz). */
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
#define DEFAULT_CUTOFF_HZ        (DEFAULT_CURR_BW_HZ * 5.0f) /* 1250 Hz */
#define DEFAULT_SAMPLING_HZ      ((float)CURR_LOOP_FREQ_HZ) /* Same as current-loop cadence. */

/* Velocity controller bandwidth (Mechanical - Slow, depends on inertia and gearbox) */
#define DEFAULT_VEL_BW_HZ        (10.0f)
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

/* Voltage-mode velocity loop (CTRL_MODE_VVEL diagnostic fallback): the
 * velocity PID output IS the q-axis voltage, the current loop is bypassed.
 * Gains are therefore V per output-shaft rad/s, not A per rad/s.  Starting
 * values follow the bldc_ctrl_v1.0 reference (voltage-mode FOC runs its
 * velocity loop straight into Uq); tune on the bench via `diag pid set
 * vvel ...`.  The per-tick clamp is min(this limit, Vbus*0.95/√3). */
#define DEFAULT_VVEL_PID_KP      (0.5f)  /* V/(rad/s) */
#define DEFAULT_VVEL_PID_KI      (5.0f)  /* V/(rad/s·s); carries IR + BEMF in steady state */
#define DEFAULT_VVEL_PID_KD      (0.0f)
#define DEFAULT_VVEL_MAX_OUT     (6.0f)  /* Uq clamp (V), under 12 V-bus ceiling 6.6 V */

/* Section 10: Calibration Parameters ----------------------------------------------------------- */

/** Startup current-offset calibration.
 *  Blocks for CALIB_CURR_DUR × 1 ms = 2000 ms while averaging ADC readings
 *  with no active phase voltage to determine the op-amp zero-offset for each
 *  phase.  The bridge is disabled during the settle/sample window; the normal
 *  zero-duty idle clamp is restored only after the samples pass validation.
 */
#define CALIB_CURR_DISCHARGE_MS  (5U)    /* All bridge switches off before clamp. */
#define CALIB_CURR_SETTLE_MS     (100U)  /* Low-side clamp / CSA settle time. */
#define CALIB_CURR_DUR           (2000U) /* Averaging duration (PWM cycles) */
#define CALIB_CURR_RAW_MIN       (1900.0f) /* DRV8311H zero-current CSA window. */
#define CALIB_CURR_RAW_MAX       (2200.0f)
#define CALIB_CURR_MAX_SPAN      (64.0f) /* Largest A/B/C zero-code separation. */
#define CALIB_CURR_MAX_STDDEV    (16.0f) /* Maximum raw-code noise (counts) */
#define CALIB_CURR_MIN_SAMPLES   (128U)

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
#define CALIB_JS_FREQ_HZ         (1.0f) /* Excitation frequency (Hz) */
#define CALIB_JS_WAIT_CYCLES     (1U) /* Number of cycles to wait before integration */
#define CALIB_JS_CALC_CYCLES     (4U) /* Number of Fourier integration cycles */
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

/* Open-loop voltage used during alignment and LUT scan (V).
 * Retained for reference; the sweep now drags with regulated I-f current. */
#define CALIB_VOLTAGE            (0.8f)
#define CALIB_ENC_IF_IQ          (0.3f) /* I-f drag current for encoder calib (A) */

/* Open-loop acceleration per step (normalised electrical angle increment). */
#define CALIB_ACCEL              (0.002f)

/* Position hold tolerance and valid angle window for polarity detection. */
#define CALIB_CHECK_POS_THR      (0.001f)
#define CALIB_CHECK_ANGLE_MIN    (FOC_PI * 0.50f)
#define CALIB_CHECK_ANGLE_MAX    (FOC_PI * 0.55f)
#define CALIB_CHECK_POS_TH       CALIB_CHECK_POS_THR /* Backward-compat aliases */

#define MOTOR_POLE_PAIRS_MAX     (15U) /* Hardware safety limits */

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
