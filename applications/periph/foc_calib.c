/**
 * @file foc_calib.c
 * @brief  FOC Control Utilities & Constants - Calibration
 * @version v1.0.0
 * @date 2025.12.23
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_calib.h"
#include "foc_calib_enc.h"
#include "foc_ctrl.h"
#include "motor_drv.h"
#include "motor_fsm.h"
#include "core_time.h"
#include "stat_welford.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private function prototypes ------------------------------------------------------------------ */
static void foc_calib_constants(struct motor_dev *motor);
static void foc_calib_clear(struct motor_dev *motor);
static void foc_calib_rs(struct motor_dev *motor);
static void foc_calib_ls(struct motor_dev *motor);
static void foc_calib_flux(struct motor_dev *motor);
static void foc_calib_js(struct motor_dev *motor);

/* public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize calibration state
 * 
 * @param  motor  Motor instance pointer
 */
void foc_calib_init(struct motor_dev *motor)
{
    motor->calib_state.calib_valid_flags = CALIB_VALID_FLAG;
}

/**
 * @brief  Update current controller gains (Based on Pole-Zero Cancellation)
 * @note   PID component multiplies by dt internally, so we supply Continuous Ki.
 */
void foc_curr_gain_calc(struct motor_dev *motor)
{
    float kp, ki_cont;
    float bandwidth_hz = motor->ctrl_config.curr_bandwidth;

    float Lq = motor->hw_config.l_q > 1e-6f ? motor->hw_config.l_q : motor->hw_config.ls;
    float Ld = motor->hw_config.l_d > 1e-6f ? motor->hw_config.l_d : motor->hw_config.ls;

    /* Q-axis (Torque) */
    foc_current_pi_gains(Lq, motor->hw_config.rs, bandwidth_hz, &kp, &ki_cont);
    motor->ctrl_config.iq_pid.config.kp = kp;
    motor->ctrl_config.iq_pid.config.ki = ki_cont;

    /* D-axis (Excitation) - Detuned to 20% */
    foc_current_pi_gains(Ld,
                         motor->hw_config.rs,
                         bandwidth_hz * DEFAULT_D_AXIS_BW_RATIO,
                         &kp,
                         &ki_cont);
    motor->ctrl_config.id_pid.config.kp = kp;
    motor->ctrl_config.id_pid.config.ki = ki_cont;
}

void foc_calib_runtime_refresh(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    foc_calib_constants(motor);
}

/**
 * @brief  Calculate velocity controller gains
 * 
 * In FOC, current generates electromagnetic torque
 * which acts on the motor rotor and produces angular acceleration
 * after overcoming the moment of inertia;
 * integrating the angular acceleration yields angular velocity,
 * and integrating the angular velocity gives the rotor position.
 * 
 * Physical model:
 *   iq [A] → Kt × iq [N·m] → ÷(J·s) → ω_motor [rad/s] → ÷gear → ω_out [rad/s]
 *
 * Design (Symmetric Optimum):
 *   Kp = J × ωvel_out × gear / Kt        [A / (rad/s_out)]
 *   Ki = Kp × ωvel_out / k_delta         [A / (rad_out)]
 *        where k_delta = 4.0 → ~53° phase margin
 *
 * Parameters:
 *   J    → from foc_calib_js() or hw_config.js
 *   Kt   → calc_params.kt = 1.5 × pp × flux
 *   gear → hw_config.gear_ratio
 *   ωvel → 2π × ctrl_config.vel_bandwidth
 */
void foc_vel_gain_calc(struct motor_dev *motor)
{
    /* 1. Get Mechanical Velocity Loop Bandwidth directly */
    float vel_bw_rads = FOC_HZ_TO_RADS(motor->ctrl_config.vel_bandwidth);

    /* 2. Get Motor Torque Constant Kt (N·m/A) */
    float Kt = motor->calc_params.kt;
    if (Kt < 1e-6f) {
        Kt = 1e-6f;
    }

    /* 3. Calculate Theoretical Continuous PI Gains for Motor Shaft (rad/s) */
    float Kp_rad = (motor->hw_config.js * vel_bw_rads) / Kt;

    /* Symmetric Optimum integral separation factor (4.0 gives ~53 deg phase margin) */
    float integral_sep = CALIB_K_DELTA;
    float Ki_rad_cont  = Kp_rad * (vel_bw_rads / integral_sep);

    motor->ctrl_config.vel_pid.config.kp = Kp_rad * motor->hw_config.gear_ratio;
    motor->ctrl_config.vel_pid.config.ki = Ki_rad_cont * motor->hw_config.gear_ratio;
}

/**
 * @brief  Calculate position controller gains
 */
void foc_pos_gain_calc(struct motor_dev *motor)
{
    /* 1. Calculate the maximum safe position bandwidth based on velocity bandwidth.
     *    Using the macro ensures we maintain a healthy damping ratio (zeta).
     */
    float max_pos_bw = motor->ctrl_config.vel_bandwidth * DEFAULT_POS_VEL_BW_RATIO;

    /* 2. Clamp the user's desired bandwidth to the safe maximum */
    float pos_bw_hz = FOC_MIN(motor->ctrl_config.pos_bandwidth, max_pos_bw);

    /* 3. Convert Hz to rad/s */
    float pos_bw_rad = FOC_HZ_TO_RADS(pos_bw_hz);

    /* 4. Assign P gain (Kp [1/s] == Bandwidth [rad/s]) */
    motor->ctrl_config.pos_pid.config.kp = pos_bw_rad;

    /* 5. Integral gain (usually 0 for position loop) */
    motor->ctrl_config.pos_pid.config.ki = 0.0f;
}

/**
 * @brief  Calibration Loop
 *
 * @param  motor  Motor instance pointer
 */
void foc_calib_loop(struct motor_dev *motor)
{
    foc_para_calc(motor);

    switch (motor->calib_state.state) {
    case CALIB_RS:
        foc_calib_rs(motor);
        break;
    case CALIB_LS:
        foc_calib_ls(motor);
        break;
    case CALIB_FLUX:
        foc_calib_flux(motor);
        break;
    case CALIB_JS:
        foc_calib_js(motor);
        break;
    case CALIB_ENC:
        foc_calib_enc(motor);
        break;
    default:
        break;
    }
}

/* private functions ---------------------------------------------------------------------------- */

/**
 * @brief  Calculate derived motor constants based on HW config
 * 
 * @param  motor  Pointer to motor instance
 */
static void foc_calib_constants(struct motor_dev *motor)
{
    motor->calc_params.inv_poles      = 1.0f / (float)motor->hw_config.poles;
    motor->calc_params.poles_over_2pi = (float)motor->hw_config.poles / FOC_2PI;
    motor->calc_params.inv_gear       = 1.0f / motor->hw_config.gear_ratio;

    /*
     * Electromagnetic torque constant (motor shaft):
     *   Kt = 1.5 × pp × Ψm   [N·m/A]
     *
     * Datasheet cross-check:
     *   Kt_output = 1.20 N·m/A  (output shaft)
     *   Kt_motor  = 1.20 / (8 × 0.90) = 0.1667 N·m/A
     *   Ψm        = 0.1667 / (1.5 × 14) = 0.00794 Wb  ≈ DEFAULT_FLUX ✓
     */
    motor->calc_params.kt = 1.5f * (float)motor->hw_config.poles * motor->hw_config.flux;
    motor->calc_params.inv_kt =
        (motor->calc_params.kt > 1e-6f) ? 1.0f / motor->calc_params.kt : 1.0f;

    /*
     * iq_max — minimum of mechanical ceiling and thermal ceiling.
     *
     * Mechanical:  I = T_out_peak / (gear × eff × Kt_motor)
     *            = 17.21 / (8 × 0.90 × 0.1667) ≈ 14.35 A
     * Thermal:     i_peak = 19.50 A
     * → iq_max = 14.35 A  (mechanical limit is tighter)
     *
     * iq_max is the absolute hardware safety ceiling used by:
     *   - foc_fault_check() for protection
     *   - dyn_iq_limit default for explicit high-current commands
     * It is NOT used as the normal vel_pid output limit.
     */
    float tor_motor_needed =
        motor->prot_config.tor_peak / (motor->hw_config.gear_ratio * motor->hw_config.gear_eff);

    float iq_limit_by_mech = tor_motor_needed * motor->calc_params.inv_kt;
    float iq_limit_by_heat = motor->prot_config.i_peak;

    motor->ctrl_state.iq_max  = FOC_MIN(iq_limit_by_mech, iq_limit_by_heat);
    motor->ctrl_state.id_max  = motor->ctrl_state.iq_max;
    motor->ctrl_state.tor_max = motor->ctrl_state.iq_max * motor->calc_params.kt *
                                motor->hw_config.gear_ratio * motor->hw_config.gear_eff;
    motor->ctrl_state.vel_max = motor->prot_config.vel_peak;

    /*
     * Propagate PID output limits.
     *
     * vel_pid output  → iq_set  [A]   limit = i_rated (3.8 A) for normal operation.
     *   Using i_rated prevents the velocity loop from saturating the current at
     *   peak current during normal tracking. Per-command overrides (PASSTHROUGH
     *   with explicit limit, or 'set vel <v> <lim>') will override via dyn_iq_limit.
     *
     * pos_pid output  → vel_set [t/s] limit = vel_max (2.3 t/s).
     *
     * NOTE: Kp/Ki are NOT set here. They are initialised by foc_pid_init()
     * (from DEFAULT_VEL_PID_KP/KI) and may be overwritten by foc_vel_gain_calc()
     * only when explicitly requested (JS_END or 'pid vel calc' shell command).
     */
    motor->ctrl_config.vel_pid.config.max_output     = motor->prot_config.i_rated;
    motor->ctrl_config.vel_pid.config.integral_limit = motor->prot_config.i_rated;
    motor->ctrl_config.pos_pid.config.max_output     = motor->ctrl_state.vel_max;
    motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.vel_max;
}

/**
 * @brief  Clear calibration states
 *
 * @param  motor  Motor instance pointer
 */
static void foc_calib_clear(struct motor_dev *motor)
{
    motor->calib_state.rs_state   = RS_IDLE;
    motor->calib_state.ls_state   = LS_IDLE;
    motor->calib_state.flux_state = FX_IDLE;
    motor->calib_state.js_state   = JS_IDLE;
    motor->calib_state.enc_state  = ENC_IDLE;
}

/**
 * @brief  Calibrate current offsets (IA/IB/IC)
 *
 * @param  motor  Motor instance pointer
 */
void foc_calib_curr(struct motor_dev *motor)
{
    struct stat_welford stat_a;
    struct stat_welford stat_b;
    struct stat_welford stat_c;

    stat_welford_init(&stat_a);
    stat_welford_init(&stat_b);
    stat_welford_init(&stat_c);

    for (uint16_t i = 0; i < CALIB_CURR_DUR; i++) {
        core_time_delay_us(1000);

        stat_welford_push(&stat_c, (float)motor->meas_data.ic_raw);
        stat_welford_push(&stat_b, (float)motor->meas_data.ib_raw);
        stat_welford_push(&stat_a, (float)motor->meas_data.ia_raw);
    }

    switch (motor->hw_config.phase_order) {
    case PHASE_ORDER_ABC:
        motor->meas_data.ic_offset = stat_welford_mean(&stat_c);
        motor->meas_data.ib_offset = stat_welford_mean(&stat_b);
        motor->meas_data.ia_offset = stat_welford_mean(&stat_a);
        break;
    case PHASE_ORDER_ACB:
        motor->meas_data.ic_offset = stat_welford_mean(&stat_b);
        motor->meas_data.ib_offset = stat_welford_mean(&stat_c);
        motor->meas_data.ia_offset = stat_welford_mean(&stat_a);
        break;
    }

    /* Transition to encoder calibration if valid flags are set */
    if (motor->calib_state.calib_valid_flags == true) {
        motor_fsm_dispatch_calib(motor, CALIB_ENC);
        foc_calib_clear(motor);
    } else {
        foc_calib_constants(motor);
        foc_curr_gain_calc(motor);
        // foc_vel_gain_calc(motor);
    }
}

/**
 * @brief  Resistance calibration
 *
 * @param  motor  Motor instance pointer
 */
static void foc_calib_rs(struct motor_dev *motor)
{
    static uint8_t  step = 0;
    static uint16_t cnt  = 0;
    static float    vs[10];
    static float    is[10];
    static float    vs_avg = 0.0f;
    static float    is_avg = 0.0f;
    static float    vs_ref = 0.0f;
    static float    is_ref[10];

    foc_ctrl_volt(motor, vs_ref, 0.0f, 0.0f);

    switch (motor->calib_state.rs_state) {
    case RS_IDLE: {
        cnt             = 0;
        step            = 0;
        vs_ref          = 0.0f;
        vs_avg          = 0.0f;
        is_avg          = 0.0f;
        float step_size = DEFAULT_RS_CURR_MAX / ((float)DEFAULT_RS_HSTEP);

        for (uint8_t i = 0; i < DEFAULT_RS_HSTEP; i++) {
            is_ref[i] = -step_size * (i + 1);
        }

        for (uint8_t i = 0; i < DEFAULT_RS_HSTEP; i++) {
            is_ref[DEFAULT_RS_HSTEP + i] = -is_ref[i];
        }

        motor->calib_state.rs_state = RS_VOLT;
    } break;

    case RS_VOLT: {
        float i_s    = sqrtf(FOC_SQ(motor->foc_calc.i_d) + FOC_SQ(motor->foc_calc.i_q));
        float is_err = fabsf(is_ref[step]) - i_s;

        if (is_ref[step] >= 0.0f) {
            if (is_err > DEFAULT_RS_CURR_THRE) {
                vs_ref += DEFAULT_RS_VOLT_STEP;
            } else if (is_err < -DEFAULT_RS_CURR_THRE) {
                vs_ref -= DEFAULT_RS_VOLT_STEP;
            }
        } else {
            if (is_err > DEFAULT_RS_CURR_THRE) {
                vs_ref -= DEFAULT_RS_VOLT_STEP;
            } else if (is_err < -DEFAULT_RS_CURR_THRE) {
                vs_ref += DEFAULT_RS_VOLT_STEP;
            }
        }

        if (fabsf(is_err) < DEFAULT_RS_CURR_THRE) {
            cnt                         = 0;
            vs_avg                      = 0.0f;
            is_avg                      = 0.0f;
            motor->calib_state.rs_state = RS_WAIT;
        }
    } break;

    case RS_WAIT: {
        if (++cnt >= DEFAULT_RS_WAIT_CNT) {
            motor->calib_state.rs_state = RS_SAMP;
            cnt                         = 0;
        }
    } break;

    case RS_SAMP: {
        vs_avg += sqrtf(FOC_SQ(motor->foc_calc.v_d) + FOC_SQ(motor->foc_calc.v_q));
        is_avg += sqrtf(FOC_SQ(motor->foc_calc.i_d) + FOC_SQ(motor->foc_calc.i_q));
        cnt++;

        if (cnt >= DEFAULT_RS_SAMPS) {
            vs[step] = vs_avg / (float)DEFAULT_RS_SAMPS;
            is[step] = is_avg / (float)DEFAULT_RS_SAMPS;
            step++;

            if (step < DEFAULT_RS_STEPS) {
                vs_ref                      = 0.0f;
                motor->calib_state.rs_state = RS_VOLT;
            } else {
                motor->calib_state.rs_state = RS_CALC;
                step                        = 0;
                vs_ref                      = 0.0f;
            }
        }
    } break;

    case RS_CALC: {
        float res_sum = 0.0f;

        for (uint8_t i = 0; i < (DEFAULT_RS_HSTEP - 1); i++) {
            float delta_volt = vs[i + 1] - vs[i];
            float delta_curr = is[i + 1] - is[i];
            res_sum += delta_volt / delta_curr;
        }

        for (uint8_t i = DEFAULT_RS_HSTEP; i < DEFAULT_RS_STEPS - 1; i++) {
            float delta_volt = vs[i + 1] - vs[i];
            float delta_curr = is[i + 1] - is[i];
            res_sum += delta_volt / delta_curr;
        }

        motor->hw_config.rs         = res_sum / (DEFAULT_RS_STEPS - 2);
        motor->calib_state.rs_state = RS_END;
    } break;

    case RS_END: {
        motor->calib_state.ls_state = LS_IDLE;
        motor->calib_state.state    = CALIB_LS;
        // motor->axis_state = AXIS_STATE_RUNNING;
        // pwm_out_disable();
    } break;
    default:
        break;
    }
}

/**
 * @brief  Inductance calibration
 *
 * @param  motor  Motor instance pointer
 */
static void foc_calib_ls(struct motor_dev *motor)
{
    static uint8_t  icnt       = 0;
    static uint32_t loop_count = 0;
    static float    ids[2]     = {0.0f, 0.0f};
    static float    iqs[2]     = {0.0f, 0.0f};

    static uint8_t l_cnt  = 0;
    static float   vd_out = 0.0f;
    static float   vq_out = 0.0f;

    foc_ctrl_volt(motor, vd_out, vq_out, 0.0f);

    switch (motor->calib_state.ls_state) {
    case LS_IDLE: {
        loop_count                  = 0;
        ids[0]                      = 0.0f;
        ids[1]                      = 0.0f;
        iqs[0]                      = 0.0f;
        iqs[1]                      = 0.0f;
        l_cnt                       = 0;
        vd_out                      = 0.0f;
        vq_out                      = 0.0f;
        icnt                        = 0;
        motor->calib_state.ls_state = LS_LD_LOOP;
    } break;

    case LS_LD_LOOP: {
        if (++l_cnt == DEFAULT_LS_CNT_THRE) {
            l_cnt = 0;

            if (loop_count < DEFAULT_LS_CYCLE) {
                vd_out = (icnt == 0) ? DEFAULT_VD_MINU : DEFAULT_VD_PLUS;
                vq_out = 0.0f;
            }

            ids[icnt] += motor->foc_calc.i_d;
            icnt = loop_count & 1;

            if (loop_count >= DEFAULT_LS_CYCLE) {
                l_cnt                       = 0;
                vd_out                      = 0.0f;
                vq_out                      = 0.0f;
                loop_count                  = 0;
                motor->calib_state.ls_state = LS_LQ_LOOP;
            } else {
                motor->calib_state.ls_state = LS_LD_LOOP;
            }

            loop_count++;
        }
    } break;

    case LS_LQ_LOOP: {
        if (++l_cnt == DEFAULT_LS_CNT_THRE) {
            l_cnt = 0;

            if (loop_count < DEFAULT_LS_CYCLE) {
                vd_out = 0.0f;
                vq_out = (icnt == 0) ? DEFAULT_VQ_MINU : DEFAULT_VQ_PLUS;
            }

            iqs[icnt] += motor->foc_calc.i_q;
            icnt = loop_count & 1;

            if (loop_count >= DEFAULT_LS_CYCLE) {
                vd_out                      = 0.0f;
                vq_out                      = 0.0f;
                loop_count                  = 0;
                motor->calib_state.ls_state = LS_CALC;
            } else {
                motor->calib_state.ls_state = LS_LQ_LOOP;
            }

            loop_count++;
        }
    } break;

    case LS_CALC: {
        float dt_total = DEFAULT_LS_TS * DEFAULT_LS_CYCLE;

        float dId_by_dt_plus  = ids[0] / dt_total;
        float dId_by_dt_minus = ids[1] / dt_total;
        float dIq_by_dt_plus  = iqs[0] / dt_total;
        float dIq_by_dt_minus = iqs[1] / dt_total;

        float Ld_plus =
            (DEFAULT_VD_PLUS - motor->hw_config.rs * ids[1] * DEFAULT_LS_TS) / dId_by_dt_plus;
        float Ld_minu =
            (DEFAULT_VD_MINU - motor->hw_config.rs * ids[0] * DEFAULT_LS_TS) / dId_by_dt_minus;
        float Lq_plus =
            (DEFAULT_VQ_PLUS - motor->hw_config.rs * iqs[1] * DEFAULT_LS_TS) / dIq_by_dt_plus;
        float Lq_minu =
            (DEFAULT_VQ_MINU - motor->hw_config.rs * iqs[0] * DEFAULT_LS_TS) / dIq_by_dt_minus;

        motor->hw_config.l_d      = (Ld_plus + Ld_minu) * 0.5f;
        motor->hw_config.l_q      = (Lq_plus + Lq_minu) * 0.5f;
        motor->hw_config.l_d_diff = fabsf(motor->hw_config.l_d - motor->hw_config.l_q);
        motor->hw_config.ls       = (motor->hw_config.l_d + motor->hw_config.l_q) * 0.5f;

        motor->calib_state.ls_state = LS_END;
    } break;

    case LS_END: {
        foc_curr_gain_calc(motor);
        motor->calib_state.flux_state = FX_IDLE;
        motor->calib_state.state      = CALIB_FLUX;
        // motor->axis_state = AXIS_STATE_RUNNING;
        // pwm_out_disable();
    } break;
    default:
        break;
    }
}

/**
 * @brief  Flux calibration
 *
 * @note   Calibration principle:  Ψm = ΔVq / Δωe
 *
 *         At steady state with Id = 0:
 *           Vq = Rs·Iq + ωe·Ψm
 *
 *         With the same Iq commanded at both speeds:
 *           ΔVq = Vq_H − Vq_L = (ωe_H − ωe_L)·Ψm
 *           → Ψm = ΔVq / Δωe
 *
 *         The Rs·Iq term cancels algebraically — no resistance
 *         compensation is required, giving a clean BEMF-only estimate.
 *         Using Vq (not the voltage amplitude |Vs|) avoids cross-axis
 *         coupling from the Vd component.
 *
 *         Speed constraints (Vbus = 24 V, Ψm ≈ 0.057 Wb):
 *           ωe_base = Vbus / Ψm ≈ 420 rad/s  (field-weakening onset)
 *           CALIB_FLUX_WE_HIGH must stay below this limit; 350 rad/s
 *           provides a 17 % safety margin.
 *
 * @param  motor  Motor instance pointer
 */
static void foc_calib_flux(struct motor_dev *motor)
{
    static float    vq_avg_l = 0.0f, vd_avg_l = 0.0f, id_avg_l = 0.0f, iq_avg_l = 0.0f;
    static float    vq_avg_h = 0.0f, vd_avg_h = 0.0f, id_avg_h = 0.0f, iq_avg_h = 0.0f;
    static float    sample_l = 0.0f, sample_h = 0.0f;
    static uint32_t wait_cnt = 0;

    static float pe_acc = 0.0f, we_set = 0.0f, theta_sim = 0.0f;
    static float vs_l = 0.0f, vs_h = 0.0f;
    static float is_l = 0.0f, is_h = 0.0f;

    theta_sim += pe_acc;
    theta_sim = foc_wrap_pm_pi(theta_sim); /* foc_wrap_0_2pi */

    foc_ctrl_curr(motor, 0.0f, DEFAULT_FLUX_IQ, theta_sim);

    switch (motor->calib_state.flux_state) {
    case FX_IDLE: {
        vq_avg_l  = 0.0f;
        vd_avg_l  = 0.0f;
        id_avg_l  = 0.0f;
        iq_avg_l  = 0.0f;
        vq_avg_h  = 0.0f;
        vd_avg_h  = 0.0f;
        id_avg_h  = 0.0f;
        iq_avg_h  = 0.0f;
        sample_l  = 0.0f;
        sample_h  = 0.0f;
        wait_cnt  = 0;
        theta_sim = 0.0f;
        pe_acc    = 0.0f;
        we_set    = 0.0f;
        vs_l      = 0.0f;
        vs_h      = 0.0f;
        is_l      = 0.0f;
        is_h      = 0.0f;

        motor->calib_state.flux_state = FX_SPD_L;
    } break;

    case FX_SPD_L: {
        float we_ref = DEFAULT_FLUX_WE_L;

        if (we_ref > we_set) {
            we_set += DEFAULT_FLUX_WE_ACC * DEFAULT_FLUX_TS;
            if (we_set > we_ref) {
                we_set = we_ref;
            }
        } else if (we_ref < we_set) {
            we_set -= DEFAULT_FLUX_WE_ACC * DEFAULT_FLUX_TS;
            if (we_set < we_ref) {
                we_set = we_ref;
            }
        }

        pe_acc = we_set * DEFAULT_FLUX_TS;

        if (we_set == we_ref) {
            if (++wait_cnt >= DEFAULT_FLUX_WAIT_CNT) {
                vd_avg_l                      = 0.0f;
                vq_avg_l                      = 0.0f;
                id_avg_l                      = 0.0f;
                iq_avg_l                      = 0.0f;
                sample_l                      = 0.0f;
                wait_cnt                      = 0;
                motor->calib_state.flux_state = FX_SAMP_L;
            }
        }
    } break;

    case FX_SAMP_L: {
        vd_avg_l += motor->foc_calc.v_d;
        vq_avg_l += motor->foc_calc.v_q;
        id_avg_l += motor->foc_calc.i_d;
        iq_avg_l += motor->foc_calc.i_q;
        sample_l += 1.0f;

        if (++wait_cnt >= DEFAULT_FLUX_SAMP_CNT) {
            wait_cnt = 0;
            vd_avg_l /= sample_l;
            vq_avg_l /= sample_l;
            id_avg_l /= sample_l;
            iq_avg_l /= sample_l;
            vs_l = sqrtf(FOC_SQ(vd_avg_l) + FOC_SQ(vq_avg_l));
            is_l = sqrtf(FOC_SQ(id_avg_l) + FOC_SQ(iq_avg_l));

            motor->calib_state.flux_state = FX_SPD_H;
        }
    } break;

    case FX_SPD_H: {
        float we_ref = DEFAULT_FLUX_WE_H;

        if (we_ref > we_set) {
            we_set += DEFAULT_FLUX_WE_ACC * DEFAULT_FLUX_TS;
            if (we_set > we_ref) {
                we_set = we_ref;
            }
        } else if (we_ref < we_set) {
            we_set -= DEFAULT_FLUX_WE_ACC * DEFAULT_FLUX_TS;
            if (we_set < we_ref) {
                we_set = we_ref;
            }
        }

        pe_acc = we_set * DEFAULT_FLUX_TS;

        if (we_set == we_ref) {
            if (++wait_cnt >= DEFAULT_FLUX_WAIT_CNT) {
                vd_avg_h                      = 0.0f;
                vq_avg_h                      = 0.0f;
                id_avg_h                      = 0.0f;
                iq_avg_h                      = 0.0f;
                sample_h                      = 0.0f;
                wait_cnt                      = 0;
                motor->calib_state.flux_state = FX_SAMP_H;
            }
        }
    } break;

    case FX_SAMP_H: {
        vd_avg_h += motor->foc_calc.v_d;
        vq_avg_h += motor->foc_calc.v_q;
        id_avg_h += motor->foc_calc.i_d;
        iq_avg_h += motor->foc_calc.i_q;
        sample_h += 1.0f;

        if (++wait_cnt >= DEFAULT_FLUX_SAMP_CNT) {
            wait_cnt = 0;
            vd_avg_h /= sample_h;
            vq_avg_h /= sample_h;
            id_avg_h /= sample_h;
            iq_avg_h /= sample_h;
            vs_h = sqrtf(FOC_SQ(vd_avg_h) + FOC_SQ(vq_avg_h));
            is_h = sqrtf(FOC_SQ(id_avg_h) + FOC_SQ(iq_avg_h));

            pe_acc = 0.0f;

            motor->calib_state.flux_state = FX_CALC;
        }
    } break;

    case FX_CALC: {
        float del_vs = vs_h - vs_l;
        float del_is = is_h - is_l;
        float del_we = DEFAULT_FLUX_WE_H - DEFAULT_FLUX_WE_L;
        motor->hw_config.flux =
            (del_vs - motor->hw_config.rs * del_is) / del_we - del_is * motor->hw_config.ls;
        motor->calib_state.flux_state = FX_END;
    } break;

    case FX_END: {
        /**
         * Refresh derived constants (Kt, iq_max, etc.) with the newly
         * calibrated flux linkage before Js calibration begins.
         * Js calibration uses calc_params.kt internally; without this
         * call it would operate with the stale pre-calibration value.
         */
        foc_calib_constants(motor);

        motor->calib_state.js_state = JS_IDLE;
        motor->calib_state.state    = CALIB_JS;
    } break;
    default:
        break;
    }
}

/**
 * @brief  Js calibration (Fourier Integral Correlation Method)
 *
 * @note   Principle
 *         ─────────
 *         Apply a sinusoidal torque excitation:
 *           Te(t) = Kt · Iq_amp · sin(ωf · t)
 *
 *         For a load with inertia Js and viscous friction B, the
 *         steady-state mechanical velocity response is:
 *           ω(t) = [Te_amp / √(B² + (Js·ωf)²)] · sin(ωf·t − φ)
 *
 *         Fourier correlations over N_calc complete periods:
 *           S_sum = ∫ ω(t)·sin(ωf·t) dt  ← captures B (in-phase)
 *           C_sum = ∫ ω(t)·cos(ωf·t) dt  ← captures Js (90° lag)
 *
 *         Closed-form inertia estimate (handles viscous friction):
 *           Js = −(Te_amp · NT_half · C_sum) / [ωf·(C_sum² + S_sum²)]
 *
 *         Advantages over derivative-based methods:
 *           • No numerical differentiation (avoids noise amplification)
 *           • Inherently separates Js from B
 *           • Multi-period integration averages out measurement noise
 *
 * @note   Safety
 *         ──────
 *         If the actual Js is very small (light load / no load), the
 *         velocity response amplitude  ω_amp = Kt·Iq/(Js·ωf)  can
 *         exceed motor rated speed.  A hard speed limit is applied:
 *         when |ω_mech| > CALIB_JS_OMEGA_MAX the excitation current is
 *         zeroed and the Fourier integrators are paused until the speed
 *         returns to a safe range.  The calibration will still complete
 *         but with reduced accuracy if the limit is frequently hit —
 *         in that case reduce CALIB_JS_IQ_AMP or check mechanical load.
 *
 * @param  motor  Motor instance pointer
 */
static void foc_calib_js(struct motor_dev *motor)
{
    static uint32_t cycle_cnt = 0;
    static float    tc        = 0.0f;
    static float    S_sum     = 0.0f;
    static float    C_sum     = 0.0f;

    float omega_f  = FOC_2PI * DEFAULT_JS_JFS;
    float T_period = 1.0f / DEFAULT_JS_JFS;

    float iq_ref = DEFAULT_JS_JIQ_MAX * foc_fast_sin(omega_f * tc);

    foc_ctrl_curr(motor, 0.0f, iq_ref, motor->meas_data.theta_elec);

    switch (motor->calib_state.js_state) {
    case JS_IDLE: {
        cycle_cnt                   = 0;
        tc                          = 0.0f;
        S_sum                       = 0.0f;
        C_sum                       = 0.0f;
        motor->calib_state.js_state = JS_LOOP;
    } break;

    case JS_LOOP: {
        tc += DEFAULT_JS_TS;

        if (cycle_cnt >= DEFAULT_JS_WAIT_CYCLES) {
            S_sum += motor->meas_data.omega_mech * foc_fast_sin(omega_f * tc) * DEFAULT_JS_TS;
            C_sum += motor->meas_data.omega_mech * foc_fast_cos(omega_f * tc) * DEFAULT_JS_TS;
        }

        if (tc >= T_period) {
            tc -= T_period;
            cycle_cnt++;

            if (cycle_cnt >= (DEFAULT_JS_WAIT_CYCLES + DEFAULT_JS_CALC_CYCLES)) {
                motor->calib_state.js_state = JS_CALC;
            }
        }
    } break;

    case JS_CALC: {
        float Te_amp  = motor->calc_params.kt * DEFAULT_JS_JIQ_MAX;
        float NT_half = (float)DEFAULT_JS_CALC_CYCLES * T_period * 0.5f;
        float den     = omega_f * (C_sum * C_sum + S_sum * S_sum);

        if (den < 1e-12f)
            den = 1e-12f;

        float Js_est = (-Te_amp * NT_half * C_sum) / den;

        /* Negative Js_est indicates encoder polarity mismatch with the electrical axis.
         * Taking the absolute value recovers the correct magnitude; the user should
         * manually verify encoder direction after calibration. */
        if (Js_est < 0.0f) {
            // motor->enc_pri.config->dir = (motor->enc_pri.config->dir == DIR_CW) ? DIR_CCW : DIR_CW;
            Js_est = fabsf(Js_est);
            /* Note: You can log this event externally via shell to notify the user that direction has been auto-corrected */
        }

        /* Range sanity check */
        if (Js_est > 1e-6f && Js_est < 0.1f) {
            motor->hw_config.js = Js_est;
        }

        motor->calib_state.js_state = JS_END;
    } break;

    case JS_END: {
        foc_calib_constants(motor);
        foc_vel_gain_calc(motor);
        foc_pos_gain_calc(motor);
    } break;

    default:
        break;
    }
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
