/**
 * @file    foc_ctrl.c
 * @brief   FOC Control Layer — Closed-Loop Pipeline & Command Interface
 * @version v2.0.0
 * @date    2026.02.19
 *
 * Responsibilities:
 *   · Real-time current/velocity/position pipeline (called from ISRs)
 *   · Parameter pre-calculation for every ADC tick (foc_para_calc)
 *   · Outer-loop dispatch (foc_outer_loop_step, 10 kHz)
 *   · Input-mode dispatch (foc_process_input_task, 1 kHz RTOS)
 *   · Command setpoint writers (foc_cmd_xxx — zero latency, non-blocking)
 *   · Fault detection (foc_fault_check)
 *
 * Naming conventions:
 *   foc_ctrl_xxx   — Low-level direct-drive functions used by calibration code.
 *   foc_cmd_xxx    — High-level command setpoint writers used by shell / CAN.
 *   foc_xxx_step   — Functions called from ISR context; must be non-blocking.
 *   foc_process_xx — Functions called from RTOS task context.
 *
 * ISR budget (20 kHz, 240 MHz):
 *   Total budget: ~25 μs.  Measured current-loop runtime: ~12–15 μs.
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_ctrl.h"
#include "motor_drv.h"
#include "traj_trap.h"
#include "tm_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private constants ---------------------------------------------------------------------------- */

#define SVPWM_VBUS_FACTOR (FOC_INV_SQRT3 * 0.95f)

/** Position-loop sub-divisor relative to the velocity-loop ISR.
 *  pos loop runs at  VEL_LOOP_FREQ / POS_LOOP_DIVIDER.
 */
#define POS_LOOP_DIVIDER  (5U)

/* Trajectory-task sample time (matches RTOS motor_task period). */
#define DT_TRAJ_TASK      (TRAJ_TASK_TS) /* 1 ms */

/* Private state variables ---------------------------------------------------------------------- */

/* Position-loop frequency divider counter (incremented each TIM6 tick). */
static uint8_t _s_pos_divider = 0U;

/* Private function prototypes ------------------------------------------------------------------ */

/* ═══════════════════════════════════════════════════════════════════════════
 * Real-time Pipeline — called from ADC ISR and TIM6 ISR
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _current_loop_core(struct motor_dev *motor)
{
    foc_clarke(&motor->foc_calc);
    sin_cos_val(&motor->foc_calc);
    foc_park(&motor->foc_calc);

    motor->foc_calc.i_d = lpf_update(&motor->ctrl_state.id_filter, motor->foc_calc.i_d);
    motor->foc_calc.i_q = lpf_update(&motor->ctrl_state.iq_filter, motor->foc_calc.i_q);

    pid_calc_position_fast(&motor->ctrl_config.id_pid,
                           motor->ctrl_state.id_set,
                           motor->foc_calc.i_d,
                           0);
    pid_calc_position_fast(&motor->ctrl_config.iq_pid,
                           motor->ctrl_state.iq_set,
                           motor->foc_calc.i_q,
                           0);

    motor->foc_calc.v_d = motor->ctrl_config.id_pid.output;
    motor->foc_calc.v_q = motor->ctrl_config.iq_pid.output;

    /**
      *
      * Cancel the cross-axis disturbance and BEMF before inverse Park:
      *   Vd_ff = -ωe · Lq · Iq   (d-axis sees Lq·Iq coupling from q-axis)
      *   Vq_ff = +ωe · Ld · Id   (q-axis sees Ld·Id coupling from d-axis)
      *           +ωe · Ψm        (q-axis BEMF — dominant at high speed)
      *
      * With id ≈ 0 (MTPA, non-salient): Vd_ff ≈ -ωe·Ls·Iq,  Vq_ff ≈ ωe·Ψm
      *
      * This eliminates the need for the PID integrator to fight these terms,
      * improving bandwidth and eliminating steady-state lag at high speed.
      */
    float omega_e = motor->meas_data.omega_elec;
    float L_d     = (motor->hw_config.l_d > 1e-6f) ? motor->hw_config.l_d : motor->hw_config.ls;
    float L_q     = (motor->hw_config.l_q > 1e-6f) ? motor->hw_config.l_q : motor->hw_config.ls;

    motor->foc_calc.v_d -= omega_e * L_q * motor->foc_calc.i_q;
    motor->foc_calc.v_q += omega_e * (L_d * motor->foc_calc.i_d + motor->hw_config.flux);

    foc_inv_park(&motor->foc_calc);
    foc_svpwm(&motor->foc_calc);
    pwm_out_set_duty(motor);
}

/* ─── foc_para_calc ─────────────────────────────────────────────────────── */

/**
 * @brief  Pre-calculate all derived measurements and dynamic limits.
 *
 * Called as the first action in every ADC ISR tick (20 kHz).  Populates:
 *   · foc_calc  — bus voltage, phase currents, inv_vbus
 *   · meas_data — electrical/mechanical angle, velocity, position
 *   · id/iq PID output limits (track Vbus in real time)
 *
 * @note   The id/iq limits here are hard per-tick limits driven by Vbus.
 *         Soft command-level limits (iq_max, vel_max) are set separately
 *         in foc_process_input_task() via the motor protection configuration.
 *
 * @param  motor  Motor instance.
 */
void foc_para_calc(struct motor_dev *motor)
{
    /* Bus voltage & phase current snapshot */
    motor->foc_calc.v_bus    = motor->meas_data.v_bus;
    motor->foc_calc.inv_vbus = 1.0f / motor->meas_data.v_bus; /* SVPWM normalisation */
    motor->foc_calc.i_a      = motor->meas_data.a;
    motor->foc_calc.i_b      = motor->meas_data.b;
    motor->foc_calc.i_c      = motor->meas_data.c;

    /* Encoder angle & velocity */
    motor->meas_data.theta_elec = motor->enc_pri.state.angle_elec_rad;
    motor->meas_data.theta_mech = motor->enc_pri.state.angle_mech_rad;
    motor->meas_data.omega_mech = motor->enc_pri.state.velocity_rads;
    motor->meas_data.omega_elec = motor->meas_data.omega_mech * motor->enc_pri.config->pole_pairs;
    motor->meas_data.total_enc  = motor->enc_pri.state.total_enc;

    /* Output-shaft observables (after gearbox).
     *
     * Unit-domain boundary (motor shaft → output shaft):
     *   omega_mech  [rad/s motor] / gear_ratio  → out_vel_rads [rad/s output]
     *   total_enc   [turns motor] × 2π / gear_ratio → out_pos_rad [rad output]
     *
     * This is the ONLY point where the inner rad/s domain maps to the outer rad/s
     * output domain.  All velocity/position setpoints and feedback downstream of
     * this point are in rad/s and rad respectively.
     */
    motor->meas_data.out_vel_rads = motor->meas_data.omega_mech / motor->hw_config.gear_ratio;
    motor->meas_data.out_pos_rad =
        motor->meas_data.total_enc * FOC_2PI / motor->hw_config.gear_ratio;

    motor->meas_data.out_vel_rpm   = FOC_RADS_TO_RPM(motor->meas_data.out_vel_rads);
    motor->meas_data.out_pos_turns = motor->meas_data.out_pos_rad * FOC_INV_2PI;

    /* Dynamic PID output limits — track Vbus */
    float vbus_limit = motor->meas_data.v_bus * SVPWM_VBUS_FACTOR;

    motor->ctrl_config.id_pid.config.max_output     = vbus_limit;
    motor->ctrl_config.id_pid.config.integral_limit = vbus_limit;
    motor->ctrl_config.iq_pid.config.max_output     = vbus_limit;
    motor->ctrl_config.iq_pid.config.integral_limit = vbus_limit;

    // motor->ctrl_config.id_pid.config.kp = 0.5f;
    // motor->ctrl_config.id_pid.config.ki = 5.0f;
    // motor->ctrl_config.iq_pid.config.kp = 0.5f;
    // motor->ctrl_config.iq_pid.config.ki = 5.0f;

    // motor->foc_calc.dtc_a = 0.0f;
    // motor->foc_calc.dtc_b = 0.0f;
    // motor->foc_calc.dtc_c = 0.0f;
    // pwm_out_set_duty(motor);

    // static float cnt = 0;
    // if (cnt++ <= PWM_FREQ * 2.0f) {
    //     foc_ctrl_volt(motor, 1.0f, 0.0f, 0.0f);
    // }

    // static float omega_cmd = 0.0f, theta_sim = 0.0f;
    // omega_cmd = foc_ramp_linear(omega_cmd, 600.0f, 50.0f, PWM_DT);
    // theta_sim += omega_cmd * PWM_DT;

    /* 2.193931026 = 50% Volt Max speed（rpm）：628.662rpm */
    // foc_ctrl_volt(motor, 0.0f, 2.193931026f, theta_sim);

    /* 3.290896539 = 75% Volt Max speed（rpm）：946.972rpm */
    // foc_ctrl_volt(motor, 0.0f, 3.290896539f, theta_sim);

    /* 4.387862052 = 100% Volt Max speed（rpm）：839.623rpm */
    // foc_ctrl_volt(motor, 0.0f, 4.387862052f, theta_sim);

    // foc_ctrl_volt(motor, 2.0f, 0.0f, 0.0f);
    // foc_ctrl_volt(motor, 2.0f, 0.0f, FOC_PI);

    /* Check the step response of the D-axis. */
    // foc_ctrl_curr(motor, 0.05f, 0.0f, 0.0f);

    /* When the current is set to 0.5A, the motor starts spinning wildly. */
    // foc_ctrl_curr(motor, 0.0f, 0.05f, motor->meas_data.theta_elec);
}

/* ─── foc_current_loop_step (ADC ISR, 20 kHz) ───────────────────────────── */

/**
 * @brief  Full current-loop step: measurement → Park → PID → SVPWM.
 *
 * Called exclusively from ADC1_2_IRQHandler when axis_state == RUNNING.
 *
 * @param  motor  Motor instance.
 */
void foc_current_loop_step(struct motor_dev *motor)
{
    /* [1] Update all measurements and dynamic limits */
    foc_para_calc(motor);

    /* [2] Latch the latest electrical angle from the PLL observer */
    motor->foc_calc.theta = foc_wrap_0_2pi(motor->meas_data.theta_elec);

    /* [3] Execute the inner current-loop pipeline */
    _current_loop_core(motor);
}

/* ─── foc_outer_loop_step (TIM6 ISR, 10 kHz) ───────────────────────────── */

/**
 * @brief  Outer velocity and position PID loops.
 *
 * Called from TMR6_GLOBAL_IRQHandler when axis_state == RUNNING.
 * Produces iq_set (consumed by the inner current loop at 20 kHz).
 *
 * Cascade structure:
 *   [1 kHz] pos_pid  : pos_set → vel_set
 *   [5 kHz] vel_pid : vel_set → iq_set
 *
 * @param  motor  Motor instance.
 */
void foc_outer_loop_step(struct motor_dev *motor)
{
    /* Position cascade */
    if (motor->ctrl_mode == CTRL_MODE_POS) {
        _s_pos_divider++;
        if (_s_pos_divider >= POS_LOOP_DIVIDER) {
            _s_pos_divider = 0U;

            /* pos_pid: error = pos_set [rad] − out_pos_rad [rad] → output = vel_set [rad/s] */
            pid_calc_position_fast(&motor->ctrl_config.pos_pid,
                                   motor->ctrl_state.pos_set,
                                   motor->meas_data.out_pos_rad,
                                   motor->ctrl_state.vel_ff);

            motor->ctrl_state.vel_set =
                FOC_SATURATE(motor->ctrl_config.pos_pid.output, motor->ctrl_state.vel_max);
        }
    }

    /* Velocity loop */
    if (motor->ctrl_mode >= CTRL_MODE_VEL) {
        /* vel_pid: error = vel_set [rad/s] − out_vel_rads [rad/s] → output = iq_set [A] */
        pid_calc_position_fast(&motor->ctrl_config.vel_pid,
                               motor->ctrl_state.vel_set,
                               motor->meas_data.out_vel_rads,
                               motor->ctrl_state.iq_ff);

        /* Negate: positive vel_error needs positive torque in CCW (DIR_CCW) convention.
             * In DIR_CCW, increasing encoder count = positive angle.
             * Positive iq = CW torque → needs -output to produce CCW acceleration. */
        motor->ctrl_state.iq_set =
            FOC_SATURATE(-motor->ctrl_config.vel_pid.output, motor->ctrl_state.iq_max);
    }

    /* Force id_set = 0 (MTPA, non-salient rotor assumption) */
    motor->ctrl_state.id_set = 0.0f;
}

/* ─── foc_process_input_task (RTOS, 1 kHz) ─────────────────────────────── */

/**
 * @brief  Input-mode dispatcher and trajectory processor.
 *
 * Called from motor_task every 1 ms (RTOS task, not ISR).
 * Updates the setpoints (vel_set, iq_set, pos_set) that the outer ISR loops consume.
 *
 * Input modes:
 *   INACTIVE      — Zero all setpoints; hold current position.
 *   PASSTHROUGH   — Apply target_xxx directly with no filtering.
 *   VEL_RAMP      — Rate-limit velocity setpoint (foc_ramp_linear).
 *   TORQUE_RAMP   — Rate-limit torque current setpoint.
 *   TRAP_TRAJ     — Advance the trapezoidal trajectory planner.
 *   POS_FILTER    — (Reserved)
 *   VEL_FILTER    — (Reserved)
 *
 * @param  motor  Motor instance.
 */
void foc_process_input_task(struct motor_dev *motor)
{
    /* Clear feed-forward terms; input modes fill them as needed */
    motor->ctrl_state.iq_ff  = 0.0f;
    motor->ctrl_state.vel_ff = 0.0f;

    /* Propagate soft limits into PID configuration each cycle */
    motor->ctrl_config.pos_pid.config.max_output     = motor->ctrl_state.vel_max;
    motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.vel_max;
    motor->ctrl_config.vel_pid.config.max_output     = motor->ctrl_state.iq_max;
    motor->ctrl_config.vel_pid.config.integral_limit = motor->ctrl_state.iq_max;

    switch (motor->input_mode) {
    /* ── INACTIVE: motor enabled but no command active ──────────────────── */
    case INPUT_MODE_INACTIVE:
        motor->ctrl_state.vel_set = 0.0f;
        motor->ctrl_state.iq_set  = 0.0f;
        /* Hold current output-shaft position (rad, multi-turn) so motor doesn't drift.
         * MUST use out_pos_rad, NOT theta_mech — theta_mech is single-turn [0, 2π)
         * and would create a unit/domain mismatch with pos_pid feedback. */
        motor->ctrl_state.pos_set = motor->meas_data.out_pos_rad;
        break;

    /* ── PASSTHROUGH: zero-latency direct command (high-frequency host) ─── */
    case INPUT_MODE_PASSTHROUGH:
        if (motor->ctrl_mode == CTRL_MODE_CURR) {
            motor->ctrl_state.iq_set = motor->ctrl_state.target_iq;

        } else if (motor->ctrl_mode == CTRL_MODE_VEL) {
            motor->ctrl_state.vel_set                        = motor->ctrl_state.target_vel;
            /* Apply per-command current limit when set */
            motor->ctrl_config.vel_pid.config.max_output     = motor->ctrl_state.dyn_iq_limit;
            motor->ctrl_config.vel_pid.config.integral_limit = motor->ctrl_state.dyn_iq_limit;

        } else if (motor->ctrl_mode == CTRL_MODE_POS) {
            motor->ctrl_state.pos_set                        = motor->ctrl_state.target_pos;
            /* Apply per-command velocity limit when set */
            motor->ctrl_config.pos_pid.config.max_output     = motor->ctrl_state.dyn_vel_limit;
            motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.dyn_vel_limit;
        }
        break;

    /* ── VEL_RAMP: rate-limited velocity (smooth acceleration) ─────────── */
    case INPUT_MODE_VEL_RAMP:
        float accel = (motor->ctrl_state.target_vel > motor->ctrl_state.vel_set)
                          ? motor->ctrl_config.ramp_accel
                          : motor->ctrl_config.ramp_decel;

        motor->ctrl_state.vel_set = foc_ramp_linear(motor->ctrl_state.vel_set,
                                                    motor->ctrl_state.target_vel,
                                                    accel,
                                                    DT_TRAJ_TASK);
        break;

    /* ── TORQUE_RAMP: rate-limited Iq current ───────────────────────────── */
    case INPUT_MODE_TORQUE_RAMP:
        motor->ctrl_state.iq_set = foc_ramp_linear(motor->ctrl_state.iq_set,
                                                   motor->ctrl_state.target_iq,
                                                   motor->ctrl_config.ramp_accel,
                                                   DT_TRAJ_TASK);
        break;

    /* ── TRAP_TRAJ: trapezoidal position trajectory ─────────────────────── */
    case INPUT_MODE_TRAP_TRAJ:
        if (traj_trap_step_update(&motor->traj, DT_TRAJ_TASK) == 0) {
            /* traj_trap now operates in rad / rad·s⁻¹ — write directly to
             * control setpoints with no conversion. */
            motor->ctrl_state.pos_set = motor->traj.step.pos; /* rad */
            motor->ctrl_state.vel_ff  = motor->traj.step.vel; /* rad/s */
            /* Optional: inertia-based current feed-forward (uncomment when Js is calibrated)
            float acc_rad  = motor->traj.step.acc * motor->hw_config.gear_ratio;
            motor->ctrl_state.iq_ff = acc_rad * motor->hw_config.inertia
                                      * motor->calc_params.inv_kt;
            */
        }
        break;

    /* ── Reserved input modes ────────────────────────────────────────────── */
    case INPUT_MODE_POS_FILTER:
    case INPUT_MODE_VEL_FILTER:
    case INPUT_MODE_TUNING:
        /* Not yet implemented */
        break;

    default:
        /* Unknown input mode — safe fallback: zero everything */
        motor->ctrl_state.vel_set = 0.0f;
        motor->ctrl_state.iq_set  = 0.0f;
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Low-Level Direct-Drive Functions (used by calibration routines)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Voltage-mode direct drive: inject (Vd, Vq) at a specified electrical angle.
 *
 * Bypasses both current and velocity PID loops.
 * Used by Rs, Ls, and Flux calibration routines, and for open-loop alignment.
 *
 * @param  motor   Motor instance.
 * @param  vd_set  D-axis voltage setpoint (V).  Clamped to ±v_rated.
 * @param  vq_set  Q-axis voltage setpoint (V).  Clamped to ±v_rated.
 * @param  pos     Electrical angle (rad).  Will be wrapped to [0, 2π).
 */
void foc_ctrl_volt(struct motor_dev *motor, float vd_set, float vq_set, float pos)
{
    motor->ctrl_mode = CTRL_MODE_VOLT;

    /* Measure currents (Clarke + Park) for logging / fault check */
    foc_clarke(&motor->foc_calc);
    motor->foc_calc.theta = foc_wrap_0_2pi(pos);
    sin_cos_val(&motor->foc_calc);
    foc_park(&motor->foc_calc);

    /* Update current filters (used by fault check) */
    motor->foc_calc.i_d = lpf_update(&motor->ctrl_state.id_filter, motor->foc_calc.i_d);
    motor->foc_calc.i_q = lpf_update(&motor->ctrl_state.iq_filter, motor->foc_calc.i_q);

    /* Apply voltage clamp and write to FOC calc struct */
    float v_limit            = motor->meas_data.v_bus * SVPWM_VBUS_FACTOR;
    motor->ctrl_state.vd_set = FOC_SATURATE(vd_set, v_limit);
    motor->ctrl_state.vq_set = FOC_SATURATE(vq_set, v_limit);
    motor->foc_calc.v_d      = motor->ctrl_state.vd_set;
    motor->foc_calc.v_q      = motor->ctrl_state.vq_set;

    /* Inverse Park → SVPWM → PWM output */
    foc_inv_park(&motor->foc_calc);
    foc_svpwm(&motor->foc_calc);
    pwm_out_set_duty(motor);
}

/**
 * @brief  Current-mode direct drive: close only the inner current loop.
 *
 * Sets id_set and iq_set directly, then runs _current_loop_core().
 * The outer velocity/position loops are NOT involved.
 * Used by Js calibration and single-axis diagnostic tests.
 *
 * @param  motor   Motor instance.
 * @param  id_set  D-axis current setpoint (A).  Clamped to ±i_rated.
 * @param  iq_set  Q-axis current setpoint (A).  Clamped to ±i_rated.
 * @param  pos     Electrical angle (rad).
 */
void foc_ctrl_curr(struct motor_dev *motor, float id_set, float iq_set, float pos)
{
    motor->ctrl_mode         = CTRL_MODE_CURR;
    motor->ctrl_state.id_set = FOC_SATURATE(id_set, motor->prot_config.i_rated);
    motor->ctrl_state.iq_set = FOC_SATURATE(iq_set, motor->prot_config.i_rated);
    motor->foc_calc.theta    = foc_wrap_0_2pi(pos);
    _current_loop_core(motor);
}

/**
 * @brief  Velocity-mode direct drive: inject a velocity feed-forward.
 *
 * Writes vel_ff and iq_ff, then triggers one outer-loop step.
 * Used for open-loop velocity checks and encoder pole-pair scan.
 *
 * @param  motor    Motor instance.
 * @param  vel_set  Velocity feed-forward (rad/s, output shaft).
 * @param  iq_ff    Torque current feed-forward (A).
 * @param  pos      Electrical angle snapshot (rad) — for outer-loop state.
 */
void foc_ctrl_vel(struct motor_dev *motor, float vel_set, float iq_ff, float pos)
{
    motor->ctrl_mode         = CTRL_MODE_VEL;
    motor->ctrl_state.vel_ff = vel_set;
    motor->ctrl_state.iq_ff  = iq_ff;
    motor->foc_calc.theta    = foc_wrap_0_2pi(pos);
    foc_outer_loop_step(motor);
}

/**
 * @brief  Position-mode direct drive: set position, velocity, and current feed-forwards.
 *
 * @param  motor    Motor instance.
 * @param  pos_set  Position setpoint (rad, output shaft, multi-turn).
 * @param  vel_ff   Velocity feed-forward (rad/s, output shaft).
 * @param  iq_ff    Current feed-forward (A).
 * @param  pos      Current electrical angle (rad).
 */
void foc_ctrl_pos(struct motor_dev *motor, float pos_set, float vel_ff, float iq_ff, float pos)
{
    motor->ctrl_mode          = CTRL_MODE_POS;
    motor->ctrl_state.pos_set = pos_set;
    motor->ctrl_state.vel_ff  = vel_ff;
    motor->ctrl_state.iq_ff   = iq_ff;
    motor->foc_calc.theta     = foc_wrap_0_2pi(pos);
    foc_outer_loop_step(motor);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * High-Level Command Setpoint Writers (Shell / CAN / Host Interface)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Write a setpoint in PASSTHROUGH mode.
 *
 * This function is a pure setpoint writer — it performs no PID computation.
 * The actual control action happens in the ISR loops within 50–100 μs.
 *
 * The `limit` parameter provides a per-command soft constraint:
 *   · VEL mode: limit = max Iq current (A)
 *   · POS mode: limit = max velocity (rad/s, output shaft)
 *   · CURR mode: limit is ignored (no outer loop to limit)
 *
 * @param  motor   Motor instance.
 * @param  mode    Target control mode (CURR / VEL / POS).
 * @param  target  Setpoint value — units depend on mode:
 *                   CURR: A
 *                   VEL:  rad/s (output shaft, internal unit)
 *                   POS:  rad   (output shaft, multi-turn, internal unit)
 * @param  limit   Optional soft limit (≤ 0 = use global protection value).
 */
void foc_cmd_passthrough(struct motor_dev  *motor,
                         enum foc_ctrl_mode mode,
                         float              target,
                         float              limit)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode  = mode;
    motor->input_mode = INPUT_MODE_PASSTHROUGH;

    switch (mode) {
    case CTRL_MODE_CURR:
        motor->ctrl_state.target_iq = FOC_SATURATE(target, motor->ctrl_state.iq_max);
        motor->ctrl_state.target_id = 0.0f;
        break;

    case CTRL_MODE_VEL:
        motor->ctrl_state.target_vel = FOC_SATURATE(target, motor->ctrl_state.vel_max);
        motor->ctrl_state.dyn_iq_limit =
            (limit > 0.0f) ? FOC_MIN(limit, motor->ctrl_state.iq_max) : motor->ctrl_state.iq_max;
        break;

    case CTRL_MODE_POS:
        /* Position target is not saturated — multi-turn range can exceed ±vel_max */
        motor->ctrl_state.target_pos = target;
        motor->ctrl_state.dyn_vel_limit =
            (limit > 0.0f) ? FOC_MIN(limit, motor->ctrl_state.vel_max) : motor->ctrl_state.vel_max;
        break;

    default:
        break;
    }
}

/**
 * @brief  Command a velocity setpoint with linear acceleration ramp.
 *
 * Sets input_mode = VEL_RAMP.  foc_process_input_task() advances the
 * ramp each 1 ms until the target is reached.
 *
 * @param  motor    Motor instance.
 * @param  new_vel  Target velocity (rad/s, output shaft).  Clamped to ±vel_max.
 * @param  max_acc  Maximum acceleration (rad/s²).  0 = use stored ramp_accel.
 */
void foc_cmd_vel_ramp(struct motor_dev *motor, float new_vel, float max_acc)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode             = CTRL_MODE_VEL;
    motor->input_mode            = INPUT_MODE_VEL_RAMP;
    motor->ctrl_state.target_vel = FOC_SATURATE(new_vel, motor->ctrl_state.vel_max);

    if (max_acc > 0.0f) {
        motor->ctrl_config.ramp_accel = max_acc;
    }
}

/**
 * @brief  Command a torque setpoint with linear ramp.
 *
 * The target torque (N·m) is converted to Iq (A) via the motor torque
 * constant Kt = 1.5 × pp × Ψm.  The ramp rate (N·m/s) is likewise
 * converted to A/s for the ramp generator.
 *
 * @param  motor       Motor instance.
 * @param  new_torque  Target output torque (N·m).
 * @param  max_acc     Maximum torque rate of change (N·m/s).  0 = use stored value.
 */
void foc_cmd_torque_ramp(struct motor_dev *motor, float new_torque, float max_acc)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode  = CTRL_MODE_CURR;
    motor->input_mode = INPUT_MODE_TORQUE_RAMP;

    /* Convert torque target to Iq (A) via inverse Kt */
    float torque_motor = new_torque / (motor->hw_config.gear_ratio * motor->hw_config.gear_eff);
    float target_iq    = torque_motor * motor->calc_params.inv_kt;
    motor->ctrl_state.target_iq = FOC_SATURATE(target_iq, motor->ctrl_state.iq_max);

    /* Convert torque rate to current rate (A/s) */
    if (max_acc > 0.0f) {
        motor->ctrl_config.ramp_accel = max_acc * motor->calc_params.inv_kt;
    }
}

/**
 * @brief  Command a position move with trapezoidal velocity profile.
 *
 * Plans a time-optimal trajectory from the current position/velocity to
 * new_pos_rad, respecting max_vel, max_acc, and max_decel constraints.
 * The trajectory generator (traj_trap) advances in foc_process_input_task().
 *
 * If new_pos_rad equals the currently active target, the call is a no-op to
 * avoid re-planning and interrupting an in-progress move.
 *
 * All parameters and internal state are in the rad domain — no unit
 * conversions occur inside this function.  The caller (shell layer) is
 * responsible for converting user-facing units (turns, rpm, rpm/s) to the
 * rad / rad·s⁻¹ / rad·s⁻² internal representation before calling here.
 *
 * @param  motor       Motor instance.
 * @param  new_pos_rad Target position (rad, output shaft, multi-turn).
 * @param  max_vel     Cruise velocity  (rad/s).   0 = use stored traj_vel.
 * @param  max_acc     Acceleration     (rad/s²).  0 = use stored traj_accel.
 * @param  max_decel   Deceleration     (rad/s²).  0 = use stored traj_decel.
 */
void foc_cmd_pos_traj(struct motor_dev *motor,
                      float             new_pos_rad,
                      float             max_vel,
                      float             max_acc,
                      float             max_decel)
{
    if (motor == NULL) {
        return;
    }

    /* Avoid redundant re-planning for the same target (both in rad). */
    if ((motor->ctrl_mode == CTRL_MODE_POS) && (motor->input_mode == INPUT_MODE_TRAP_TRAJ) &&
        (motor->ctrl_state.target_pos == new_pos_rad)) {
        return;
    }

    motor->ctrl_mode             = CTRL_MODE_POS;
    motor->input_mode            = INPUT_MODE_TRAP_TRAJ;
    motor->ctrl_state.target_pos = new_pos_rad; /* rad */

    /* Update trajectory configuration with per-command overrides */
    struct traj_trap_config cfg     = motor->traj.config;
    bool                    changed = false;

    if (max_vel > 0.0f) {
        cfg.max_vel = max_vel; /* rad/s */
        changed     = true;
    }
    if (max_acc > 0.0f) {
        cfg.max_acc = max_acc; /* rad/s² */
        changed     = true;
    }
    if (max_decel > 0.0f) {
        cfg.max_dec = max_decel; /* rad/s² */
        changed     = true;
    }

    if (changed) {
        traj_trap_config_set(&motor->traj, &cfg);
    }

    /* Seed the planner from the current state.
     * traj_trap now uses rad / rad·s⁻¹ — pass values directly. */
    float start_pos = motor->traj.is_active ? motor->traj.step.pos /* rad (from active traj) */
                                            : motor->meas_data.out_pos_rad; /* rad (from encoder) */
    float start_vel = motor->traj.is_active ? motor->traj.step.vel /* rad/s (from active traj) */
                                            : motor->meas_data.out_vel_rads; /* rad/s (from PLL) */

    traj_trap_plan(&motor->traj, new_pos_rad, start_pos, start_vel);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fault Detection
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
  * @brief  Software fault check — runs in motor_task context (1 kHz).
  *
  * Protection policy:
  *   - Over-current / Over-speed: only active when drive output is ON
  *     (RUNNING or CALIB state). No current flows in IDLE — checking would
  *     only trigger on ADC noise.
  *   - Over-voltage / Under-voltage: active full lifecycle, but gated by
  *     v_bus > 1.0 V to skip uninitialized ADC readings at startup.
  *   - Over-temperature: active full lifecycle, gated by temp > 5 °C to skip
  *     invalid NTC readings before safety_task's first adc_read_ordinary().
  *
  * Hard over-current (instantaneous peak) is handled by hardware comparator
  * → TIM1 break input and must not rely on this function.
  *
  * @param  motor  Motor instance.
  */
void foc_fault_check(struct motor_dev *motor)
{
    // const bool drive_active =
    //     (motor->axis_state == AXIS_STATE_RUNNING) || (motor->axis_state == AXIS_STATE_CALIB);

    // /* ── 1. Over-current (soft trip): only when PWM output is active ──────── */
    // if (drive_active) {
    //     float i_total_sq =
    //         motor->foc_calc.i_d * motor->foc_calc.i_d + motor->foc_calc.i_q * motor->foc_calc.i_q;
    //     float i_trip = motor->prot_config.i_rated * 1.5f;

    //     if (i_total_sq > i_trip * i_trip) {
    //         motor->fault_code |= FAULT_OVER_CURR;
    //         goto _fault_trip;
    //     }
    // }

    //     /* ── 2. Bus over-voltage: full lifecycle, requires valid ADC read ───────── */
    //     if (motor->meas_data.v_bus > 1.0f) {
    //         if (motor->meas_data.v_bus > motor->prot_config.v_bus_ov_thr) {
    //             motor->fault_code |= FAULT_OVER_VOLT;
    //             goto _fault_trip;
    //         }
    //     }

    //     /* ── 3. Bus under-voltage: full lifecycle, requires valid ADC read ──────── */
    //     if (motor->meas_data.v_bus > 1.0f) {
    //         if (motor->meas_data.v_bus < motor->prot_config.v_bus_uv_thr) {
    //             motor->fault_code |= FAULT_UNDER_VOLT;
    //             goto _fault_trip;
    //         }
    //     }

    //     /* ── 4. Over-speed: only when drive is active ───────────────────────────── */
    //     if (drive_active) {
    //         if (fabsf(motor->meas_data.out_vel_rads) > motor->prot_config.vel_over_thr) {
    //             motor->fault_code |= FAULT_OVER_VEL;
    //             goto _fault_trip;
    //         }
    //     }

    //     /* ── 5. Motor over-temperature: full lifecycle, requires valid NTC read ─── */
    //     if (motor->meas_data.motor_temp > 5.0f) {
    //         if (motor->meas_data.motor_temp > motor->prot_config.motor_temp_thr) {
    //             motor->fault_code |= FAULT_MOTOR_OVER_TEMP;
    //             goto _fault_trip;
    //         }
    //     }

    //     /* ── 6. Driver over-temperature: same policy as motor temp ─────────────── */
    //     if (motor->meas_data.driver_temp > 5.0f) {
    //         if (motor->meas_data.driver_temp > motor->prot_config.driver_temp_thr) {
    //             motor->fault_code |= FAULT_DRIVER_OVER_TEMP;
    //             goto _fault_trip;
    //         }
    //     }

    //     return;

    // _fault_trip:
    //     motor->axis_state = AXIS_STATE_FAULT;
    //     pwm_out_disable();
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
