/**
 * @file    foc_ctrl.c
 * @brief   FOC hardware-coupled control engine implementation
 * @version v2.1.0
 * @date    2026.05.22
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_ctrl.h`
 * - owns ISR-facing current/outer-loop steps and direct-drive helpers
 * - does not own product command policy, RTOS command cadence, or fault-policy
 *   decisions after the runtime-layer seam closure
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_ctrl.h"
#include "motor_drv.h"
#include "tm_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private Macros ------------------------------------------------------------------------------- */

#define SVPWM_VBUS_FACTOR (FOC_INV_SQRT3 * 0.95f)

/** Position-loop sub-divisor relative to the velocity-loop ISR.
 *  pos loop runs at  VEL_LOOP_FREQ / POS_LOOP_DIVIDER.
 */
#define POS_LOOP_DIVIDER  (5U)

/* Private State -------------------------------------------------------------------------------- */

/* Position-loop frequency divider counter (incremented each TIM6 tick). */
static uint8_t _s_pos_divider = 0U;

/* Private Helpers ------------------------------------------------------------------------------ */

static void _current_loop_core(struct motor_dev *motor)
{
    foc_clarke(&motor->foc_calc);
    sin_cos_val(&motor->foc_calc);
    foc_park(&motor->foc_calc);

    motor->foc_calc.i_d = lpf_update(&motor->ctrl_state.id_filter, motor->foc_calc.i_d);
    motor->foc_calc.i_q = lpf_update(&motor->ctrl_state.iq_filter, motor->foc_calc.i_q);

    pid_calc_position_fast(
        &motor->ctrl_config.id_pid,
        motor->ctrl_state.id_set,
        motor->foc_calc.i_d,
        0
    );
    pid_calc_position_fast(
        &motor->ctrl_config.iq_pid,
        motor->ctrl_state.iq_set,
        motor->foc_calc.i_q,
        0
    );

    motor->foc_calc.v_d = motor->ctrl_config.id_pid.output;
    motor->foc_calc.v_q = motor->ctrl_config.iq_pid.output;

    /**
      *
      * Cancel the cross-axis disturbance and BEMF before inverse Park:
      *   Vd_ff = -¦Øe ¡¤ Lq ¡¤ Iq   (d-axis sees Lq¡¤Iq coupling from q-axis)
      *   Vq_ff = +¦Øe ¡¤ Ld ¡¤ Id   (q-axis sees Ld¡¤Id coupling from d-axis)
      *           +¦Øe ¡¤ ¦·m        (q-axis BEMF ¡ª dominant at high speed)
      *
      * With id ¡Ö 0 (MTPA, non-salient): Vd_ff ¡Ö -¦Øe¡¤Ls¡¤Iq,  Vq_ff ¡Ö ¦Øe¡¤¦·m
      *
      * This eliminates the need for the PID integrator to fight these terms,
      * improving bandwidth and eliminating steady-state lag at high speed.
      */
#if 1
    float omega_e = motor->meas_data.omega_elec;
    float L_d = (motor->hw_config.l_d > 1e-6f) ? motor->hw_config.l_d : motor->hw_config.ls;
    float L_q = (motor->hw_config.l_q > 1e-6f) ? motor->hw_config.l_q : motor->hw_config.ls;

    motor->foc_calc.v_d -= omega_e * L_q * motor->foc_calc.i_q;
    motor->foc_calc.v_q += omega_e * (L_d * motor->foc_calc.i_d + motor->hw_config.flux);
#endif

    foc_inv_park(&motor->foc_calc);
    foc_svpwm(&motor->foc_calc);
    pwm_out_set_duty(motor);
}

/* ©¤©¤©¤ foc_para_calc ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤ */

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Pre-calculate all derived measurements and dynamic limits.
 *
 * Called as the first action in every ADC ISR tick (20 kHz).  Populates:
 *   ¡¤ foc_calc  ¡ª bus voltage, phase currents, inv_vbus
 *   ¡¤ meas_data ¡ª electrical/mechanical angle, velocity, position
 *   ¡¤ id/iq PID output limits (track Vbus in real time)
 *
 * @note   The id/iq limits here are hard per-tick limits driven by Vbus.
 *         Soft command-level limits (iq_max, vel_max) are set by the
 *         runtime-owned motion path outside this module.
 *
 * @param  motor  Motor instance.
 */
void foc_para_calc(struct motor_dev *motor)
{
    /* Bus voltage & phase current snapshot */
    motor->foc_calc.v_bus = motor->meas_data.v_bus;
    motor->foc_calc.inv_vbus = 1.0f / motor->meas_data.v_bus; /* SVPWM normalisation */
    motor->foc_calc.i_a = motor->meas_data.a;
    motor->foc_calc.i_b = motor->meas_data.b;
    motor->foc_calc.i_c = motor->meas_data.c;

    /* Encoder angle & velocity */
    motor->meas_data.theta_elec = motor->enc_pri.state.angle_elec_rad;
    motor->meas_data.theta_mech = motor->enc_pri.state.angle_mech_rad;
    motor->meas_data.omega_mech = motor->enc_pri.state.velocity_rads;
    motor->meas_data.omega_elec = motor->meas_data.omega_mech * motor->enc_pri.config->pole_pairs;
    motor->meas_data.total_enc = motor->enc_pri.state.total_enc;

    /* Output-shaft observables (after gearbox).
     *
     * Unit-domain boundary (motor shaft ¡ú output shaft):
     *   omega_mech  [rad/s motor] / gear_ratio  ¡ú out_vel_rads [rad/s output]
     *   total_enc   [turns motor] ¡Á 2¦Ð / gear_ratio ¡ú out_pos_rad [rad output]
     *
     * This is the ONLY point where the inner rad/s domain maps to the outer rad/s
     * output domain.  All velocity/position setpoints and feedback downstream of
     * this point are in rad/s and rad respectively.
     */
    motor->meas_data.out_vel_rads = motor->meas_data.omega_mech / motor->hw_config.gear_ratio;
    motor->meas_data.out_pos_rad =
        motor->meas_data.total_enc * FOC_2PI / motor->hw_config.gear_ratio;

    motor->meas_data.out_vel_rpm = FOC_RADS_TO_RPM(motor->meas_data.out_vel_rads);
    motor->meas_data.out_pos_turns = motor->meas_data.out_pos_rad * FOC_INV_2PI;

    /* Dynamic PID output limits ¡ª track Vbus */
    float vbus_limit = motor->meas_data.v_bus * SVPWM_VBUS_FACTOR;

    motor->ctrl_config.id_pid.config.max_output = vbus_limit;
    motor->ctrl_config.id_pid.config.integral_limit = vbus_limit;
    motor->ctrl_config.iq_pid.config.max_output = vbus_limit;
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

    // foc_ctrl_volt(motor, 2.0f, 0.0f, 0.0f);
    // foc_ctrl_volt(motor, 2.0f, 0.0f, FOC_PI);

    /* Check the step response of the D-axis. */
    // foc_ctrl_curr(motor, 0.05f, 0.0f, 0.0f);

    /* When the current is set to 0.5A, the motor starts spinning wildly. */
    // foc_ctrl_curr(motor, 0.0f, 0.05f, motor->meas_data.theta_elec);
}

/* ©¤©¤©¤ foc_current_loop_step (ADC ISR, 20 kHz) ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤ */

/**
 * @brief  Full current-loop step: measurement ¡ú Park ¡ú PID ¡ú SVPWM.
 *
 * Called exclusively from ADC1_2_IRQHandler when axis_state == RUNNING.
 *
 * @param  motor  Motor instance.
 */
void foc_current_loop_step(struct motor_dev *motor)
{
    /* [1] Update all measurements and dynamic limits */
    foc_para_calc(motor);

    /* [2] Voltage open-loop path: bypass current PID and drive the inverter
     * directly from the simulated electrical angle.  This keeps voltage
     * injection (alignment or spin test) from fighting the current loop. */
    if (motor->ctrl_mode == CTRL_MODE_VOLT) {
        motor->ctrl_state.theta_sim += motor->ctrl_state.we_volt_ramp * PWM_DT;
        motor->ctrl_state.theta_sim  = foc_wrap_0_2pi(motor->ctrl_state.theta_sim);

        foc_ctrl_volt(motor,
                      motor->ctrl_state.vd_set,
                      motor->ctrl_state.vq_set,
                      motor->ctrl_state.theta_sim);
        return;
    }

    /* [3] Latch the latest electrical angle from the PLL observer */
    motor->foc_calc.theta = foc_wrap_0_2pi(motor->meas_data.theta_elec);

    /* [4] Execute the inner current-loop pipeline */
    _current_loop_core(motor);
}

/* ©¤©¤©¤ foc_outer_loop_step (TIM6 ISR, 10 kHz) ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤ */

/**
 * @brief  Outer velocity and position PID loops.
 *
 * Called from the platform-selected motor soft-IRQ when axis_state == RUNNING.
 * Produces iq_set (consumed by the inner current loop at 20 kHz).
 *
 * Cascade structure:
 *   [1 kHz] pos_pid  : pos_set ¡ú vel_set
 *   [5 kHz] vel_pid : vel_set ¡ú iq_set
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

            /* pos_pid: error = pos_set [rad] ? out_pos_rad [rad] ¡ú output = vel_set [rad/s] */
            pid_calc_position_fast(
                &motor->ctrl_config.pos_pid,
                motor->ctrl_state.pos_set,
                motor->meas_data.out_pos_rad,
                motor->ctrl_state.vel_ff
            );

            motor->ctrl_state.vel_set =
                FOC_SATURATE(motor->ctrl_config.pos_pid.output, motor->ctrl_state.vel_max);
        }
    }

    /* Velocity loop */
    if (motor->ctrl_mode >= CTRL_MODE_VEL) {
        /* vel_pid: error = vel_set [rad/s] ? out_vel_rads [rad/s] ¡ú output = iq_set [A] */
        pid_calc_position_fast(
            &motor->ctrl_config.vel_pid,
            motor->ctrl_state.vel_set,
            motor->meas_data.out_vel_rads,
            motor->ctrl_state.iq_ff
        );

        /* Direct mapping: positive vel_error ¡ú positive iq ¡ú forward torque.
             * Evidence (state show): iq=+1.763 ¡ú vel_rpm=+991.69, so positive iq
             * drives positive velocity.  DIR_CCW encoder mirroring already aligns
             * encoder direction with motor forward direction, so no extra sign flip
             * is needed here.  The previous negation formed a positive-feedback loop
             * (set positive speed ¡ú negative iq ¡ú reverse ¡ú larger error ¡ú runaway). */
        motor->ctrl_state.iq_set =
            FOC_SATURATE(motor->ctrl_config.vel_pid.output, motor->ctrl_state.iq_max);
    }

    /* Force id_set = 0 (MTPA, non-salient rotor assumption) */
    motor->ctrl_state.id_set = 0.0f;
}

/* ¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T
 * Low-Level Direct-Drive Functions (used by calibration routines)
 * ¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T¨T */

/**
 * @brief  Voltage-mode direct drive: inject (Vd, Vq) at a specified electrical angle.
 *
 * Bypasses both current and velocity PID loops.
 * Used by Rs, Ls, and Flux calibration routines, and for open-loop alignment.
 *
 * @param  motor   Motor instance.
 * @param  vd_set  D-axis voltage setpoint (V).  Clamped to ¡Àv_rated.
 * @param  vq_set  Q-axis voltage setpoint (V).  Clamped to ¡Àv_rated.
 * @param  pos     Electrical angle (rad).  Will be wrapped to [0, 2¦Ð).
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
    float v_limit = motor->meas_data.v_bus * SVPWM_VBUS_FACTOR;
    motor->ctrl_state.vd_set = FOC_SATURATE(vd_set, v_limit);
    motor->ctrl_state.vq_set = FOC_SATURATE(vq_set, v_limit);
    motor->foc_calc.v_d = motor->ctrl_state.vd_set;
    motor->foc_calc.v_q = motor->ctrl_state.vq_set;

    /* Inverse Park ¡ú SVPWM ¡ú PWM output */
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
 * @param  id_set  D-axis current setpoint (A).  Clamped to ¡Ài_rated.
 * @param  iq_set  Q-axis current setpoint (A).  Clamped to ¡Ài_rated.
 * @param  pos     Electrical angle (rad).
 */
void foc_ctrl_curr(struct motor_dev *motor, float id_set, float iq_set, float pos)
{
    motor->ctrl_mode = CTRL_MODE_CURR;
    motor->ctrl_state.id_set = FOC_SATURATE(id_set, motor->prot_config.i_rated);
    motor->ctrl_state.iq_set = FOC_SATURATE(iq_set, motor->prot_config.i_rated);
    motor->foc_calc.theta = foc_wrap_0_2pi(pos);
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
 * @param  pos      Electrical angle snapshot (rad) ¡ª for outer-loop state.
 */
void foc_ctrl_vel(struct motor_dev *motor, float vel_set, float iq_ff, float pos)
{
    motor->ctrl_mode = CTRL_MODE_VEL;
    motor->ctrl_state.vel_ff = vel_set;
    motor->ctrl_state.iq_ff = iq_ff;
    motor->foc_calc.theta = foc_wrap_0_2pi(pos);
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
    motor->ctrl_mode = CTRL_MODE_POS;
    motor->ctrl_state.pos_set = pos_set;
    motor->ctrl_state.vel_ff = vel_ff;
    motor->ctrl_state.iq_ff = iq_ff;
    motor->foc_calc.theta = foc_wrap_0_2pi(pos);
    foc_outer_loop_step(motor);
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
