/**
 * @file    foc_calib.c
 * @brief   FOC hardware-coupled calibration engine implementation
 * @version v1.1.0
 * @date    2026.05.22
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_calib.h`
 * - owns current-offset and motor-parameter calibration step engines
 * - delegates post-step runtime policy and save/restore glue to
 *   `applications/runtime/`
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_calib.h"
#include "foc_calib_enc.h"
#include "foc_ctrl.h"
#include "motor_calibration_runtime.h"
#include "motor_drv.h"
#include "core_time.h"
#include "stat_welford.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private Helpers ------------------------------------------------------------------------------ */
static void foc_calib_rs(struct motor_dev *motor);
static void foc_calib_ls(struct motor_dev *motor);
static void foc_calib_flux(struct motor_dev *motor);
static void foc_calib_js(struct motor_dev *motor);

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the hardware-coupled calibration state.
 *
 * @param  motor  Motor instance.
 */
void foc_calib_init(struct motor_dev *motor)
{
    motor->calib_state.calib_valid_flags = CALIB_VALID_FLAG;
}

/**
 * @brief  Advance the active calibration step machine.
 *
 * @param  motor  Motor instance.
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

/* Private Helpers ------------------------------------------------------------------------------ */

/**
 * @brief  Calibrate current offsets (IA/IB/IC)
 *
 * @param  motor  Motor instance.
 */
void foc_calib_curr(struct motor_dev *motor)
{
    // struct stat_welford stat_a;
    struct stat_welford stat_b;
    struct stat_welford stat_c;

    // stat_welford_init(&stat_a);
    stat_welford_init(&stat_b);
    stat_welford_init(&stat_c);

    for (uint16_t i = 0; i < CALIB_CURR_DUR; i++) {
        core_time_delay_us(1000);

        stat_welford_push(&stat_c, (float)motor->meas_data.ic_raw);
        stat_welford_push(&stat_b, (float)motor->meas_data.ib_raw);
        // stat_welford_push(&stat_a, (float)motor->meas_data.ia_raw);
    }

    switch (motor->hw_config.phase_order) {
    case PHASE_ORDER_ABC:
        motor->meas_data.ic_offset = stat_welford_mean(&stat_c);
        motor->meas_data.ib_offset = stat_welford_mean(&stat_b);
        // motor->meas_data.ia_offset = stat_welford_mean(&stat_a);
        break;
    case PHASE_ORDER_ACB:
        motor->meas_data.ic_offset = stat_welford_mean(&stat_b);
        motor->meas_data.ib_offset = stat_welford_mean(&stat_c);
        // motor->meas_data.ia_offset = stat_welford_mean(&stat_a);
        break;
    }

    motor_calibration_runtime_after_current_offsets(motor);
}

/**
 * @brief  Resistance calibration
 *
 * @param  motor  Motor instance pointer
 */
static void foc_calib_rs(struct motor_dev *motor)
{
    static uint8_t step = 0;
    static uint16_t cnt = 0;
    static float vs[10];
    static float is[10];
    static float vs_avg = 0.0f;
    static float is_avg = 0.0f;
    static float vs_ref = 0.0f;
    static float is_ref[10];

    foc_ctrl_volt(motor, vs_ref, 0.0f, 0.0f);

    switch (motor->calib_state.rs_state) {
    case RS_IDLE: {
        cnt = 0;
        step = 0;
        vs_ref = 0.0f;
        vs_avg = 0.0f;
        is_avg = 0.0f;
        float step_size = DEFAULT_RS_CURR_MAX / ((float)DEFAULT_RS_HSTEP);

        for (uint8_t i = 0; i < DEFAULT_RS_HSTEP; i++) {
            is_ref[i] = -step_size * (i + 1);
        }

        for (uint8_t i = 0; i < DEFAULT_RS_HSTEP; i++) {
            is_ref[DEFAULT_RS_HSTEP + i] = -is_ref[i];
        }

        motor->calib_state.rs_state = RS_ALIGN;
    } break;

    case RS_ALIGN: {
        vs_ref = DEFAULT_RS_ALIGN_VOLT;

        if (++cnt >= DEFAULT_RS_ALIGN_CNT) {
            cnt = 0;
            vs_ref = 0.0f;
            motor->calib_state.rs_state = RS_VOLT;
        }
    } break;

    case RS_VOLT: {
        float i_s = FOC_SQRT(FOC_SQ(motor->foc_calc.i_d) + FOC_SQ(motor->foc_calc.i_q));
        float is_err = FOC_ABS(is_ref[step]) - i_s;

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

        if (FOC_ABS(is_err) < DEFAULT_RS_CURR_THRE) {
            cnt = 0;
            vs_avg = 0.0f;
            is_avg = 0.0f;
            motor->calib_state.rs_state = RS_WAIT;
        }
    } break;

    case RS_WAIT: {
        if (++cnt >= DEFAULT_RS_WAIT_CNT) {
            motor->calib_state.rs_state = RS_SAMP;
            cnt = 0;
        }
    } break;

    case RS_SAMP: {
        vs_avg += FOC_SQRT(FOC_SQ(motor->foc_calc.v_d) + FOC_SQ(motor->foc_calc.v_q));
        is_avg += FOC_SQRT(FOC_SQ(motor->foc_calc.i_d) + FOC_SQ(motor->foc_calc.i_q));
        cnt++;

        if (cnt >= DEFAULT_RS_SAMPS) {
            vs[step] = vs_avg / (float)DEFAULT_RS_SAMPS;
            is[step] = is_avg / (float)DEFAULT_RS_SAMPS;
            step++;

            if (step < DEFAULT_RS_STEPS) {
                vs_ref = 0.0f;
                motor->calib_state.rs_state = RS_VOLT;
            } else {
                motor->calib_state.rs_state = RS_CALC;
                step = 0;
                vs_ref = 0.0f;
            }
        }
    } break;

    case RS_CALC: {
        float res_sum = 0.0f;

        for (uint8_t i = 0; i < (DEFAULT_RS_HSTEP - 1); i++) {
            float delta_volt = vs[i + 1] - vs[i];
            float delta_curr = is[i + 1] - is[i];
            if (FOC_ABS(delta_curr) < 1e-6f)
                delta_curr = 1e-6f;
            res_sum += delta_volt / delta_curr;
        }

        for (uint8_t i = DEFAULT_RS_HSTEP; i < DEFAULT_RS_STEPS - 1; i++) {
            float delta_volt = vs[i + 1] - vs[i];
            float delta_curr = is[i + 1] - is[i];
            if (FOC_ABS(delta_curr) < 1e-6f)
                delta_curr = 1e-6f;
            res_sum += delta_volt / delta_curr;
        }

        motor->hw_config.rs = res_sum / (DEFAULT_RS_STEPS - 2);
        motor->calib_state.rs_state = RS_END;
    } break;

    case RS_END: {
        motor->calib_state.ls_state = LS_IDLE;
        motor->calib_state.state = CALIB_LS;
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
    static uint8_t icnt = 0;
    static uint32_t loop_count = 0;
    static float ids[2] = {0.0f, 0.0f};
    static float iqs[2] = {0.0f, 0.0f};

    static uint8_t l_cnt = 0;
    static float vd_out = 0.0f;
    static float vq_out = 0.0f;

    foc_ctrl_volt(motor, vd_out, vq_out, 0.0f);

    switch (motor->calib_state.ls_state) {
    case LS_IDLE: {
        loop_count = 0;
        ids[0] = 0.0f;
        ids[1] = 0.0f;
        iqs[0] = 0.0f;
        iqs[1] = 0.0f;
        l_cnt = 0;
        vd_out = 0.0f;
        vq_out = 0.0f;
        icnt = 0;
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
                l_cnt = 0;
                vd_out = 0.0f;
                vq_out = 0.0f;
                loop_count = 0;
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
                vd_out = 0.0f;
                vq_out = 0.0f;
                loop_count = 0;
                motor->calib_state.ls_state = LS_CALC;
            } else {
                motor->calib_state.ls_state = LS_LQ_LOOP;
            }

            loop_count++;
        }
    } break;

    case LS_CALC: {
        float dt_total = DEFAULT_LS_TS * DEFAULT_LS_CYCLE;

#if 0
        float dId_by_dt_plus  = ids[0] / dt_total;
        float dId_by_dt_minus = ids[1] / dt_total;
        float dIq_by_dt_plus  = iqs[0] / dt_total;
        float dIq_by_dt_minus = iqs[1] / dt_total;
#else
        float dId_by_dt_plus = ids[1] / dt_total;
        float dId_by_dt_minus = ids[0] / dt_total;
        float dIq_by_dt_plus = iqs[1] / dt_total;
        float dIq_by_dt_minus = iqs[0] / dt_total;
#endif

        float Ld_plus =
            (DEFAULT_VD_PLUS - motor->hw_config.rs * ids[1] * DEFAULT_LS_TS) / dId_by_dt_plus;
        float Ld_minu =
            (DEFAULT_VD_MINU - motor->hw_config.rs * ids[0] * DEFAULT_LS_TS) / dId_by_dt_minus;
        float Lq_plus =
            (DEFAULT_VQ_PLUS - motor->hw_config.rs * iqs[1] * DEFAULT_LS_TS) / dIq_by_dt_plus;
        float Lq_minu =
            (DEFAULT_VQ_MINU - motor->hw_config.rs * iqs[0] * DEFAULT_LS_TS) / dIq_by_dt_minus;

        motor->hw_config.l_d = (Ld_plus + Ld_minu) * 0.5f;
        motor->hw_config.l_q = (Lq_plus + Lq_minu) * 0.5f;
        motor->hw_config.l_d_diff = FOC_ABS(motor->hw_config.l_d - motor->hw_config.l_q);
        motor->hw_config.ls = (motor->hw_config.l_d + motor->hw_config.l_q) * 0.5f;

        motor->calib_state.ls_state = LS_END;
    } break;

    case LS_END: {
        foc_curr_gain_calc(motor);
        motor->calib_state.flux_state = FX_IDLE;
        motor->calib_state.state = CALIB_FLUX;
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
 * @note   Calibration principle:  ��m = ��Vq / ����e
 *
 *         At steady state with Id = 0:
 *           Vq = Rs��Iq + ��e����m
 *
 *         With the same Iq commanded at both speeds:
 *           ��Vq = Vq_H ? Vq_L = (��e_H ? ��e_L)����m
 *           �� ��m = ��Vq / ����e
 *
 *         The Rs��Iq term cancels algebraically �� no resistance
 *         compensation is required, giving a clean BEMF-only estimate.
 *         Using Vq (not the voltage amplitude |Vs|) avoids cross-axis
 *         coupling from the Vd component.
 *
 *         Speed constraints (Vbus = 24 V, ��m �� 0.057 Wb):
 *           ��e_base = Vbus / ��m �� 420 rad/s  (field-weakening onset)
 *           CALIB_FLUX_WE_HIGH must stay below this limit; 350 rad/s
 *           provides a 17 % safety margin.
 *
 * @param  motor  Motor instance pointer
 */
static void foc_calib_flux(struct motor_dev *motor)
{
    static float vq_avg_l = 0.0f, vd_avg_l = 0.0f, id_avg_l = 0.0f, iq_avg_l = 0.0f;
    static float vq_avg_h = 0.0f, vd_avg_h = 0.0f, id_avg_h = 0.0f, iq_avg_h = 0.0f;
    static float sample_l = 0.0f, sample_h = 0.0f;
    static uint32_t wait_cnt = 0;

    static float pe_acc = 0.0f, we_set = 0.0f, theta_sim = 0.0f;
    static float vs_l = 0.0f, vs_h = 0.0f;
    static float is_l = 0.0f, is_h = 0.0f;

    theta_sim += pe_acc;
    theta_sim = foc_wrap_pm_pi(theta_sim); /* foc_wrap_0_2pi */

    foc_ctrl_curr(motor, 0.0f, DEFAULT_FLUX_IQ, theta_sim);

    switch (motor->calib_state.flux_state) {
    case FX_IDLE: {
        vq_avg_l = 0.0f;
        vd_avg_l = 0.0f;
        id_avg_l = 0.0f;
        iq_avg_l = 0.0f;
        vq_avg_h = 0.0f;
        vd_avg_h = 0.0f;
        id_avg_h = 0.0f;
        iq_avg_h = 0.0f;
        sample_l = 0.0f;
        sample_h = 0.0f;
        wait_cnt = 0;
        theta_sim = 0.0f;
        pe_acc = 0.0f;
        we_set = 0.0f;
        vs_l = 0.0f;
        vs_h = 0.0f;
        is_l = 0.0f;
        is_h = 0.0f;

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
                vd_avg_l = 0.0f;
                vq_avg_l = 0.0f;
                id_avg_l = 0.0f;
                iq_avg_l = 0.0f;
                sample_l = 0.0f;
                wait_cnt = 0;
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
            vs_l = FOC_SQRT(FOC_SQ(vd_avg_l) + FOC_SQ(vq_avg_l));
            is_l = FOC_SQRT(FOC_SQ(id_avg_l) + FOC_SQ(iq_avg_l));

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
                vd_avg_h = 0.0f;
                vq_avg_h = 0.0f;
                id_avg_h = 0.0f;
                iq_avg_h = 0.0f;
                sample_h = 0.0f;
                wait_cnt = 0;
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
            vs_h = FOC_SQRT(FOC_SQ(vd_avg_h) + FOC_SQ(vq_avg_h));
            is_h = FOC_SQRT(FOC_SQ(id_avg_h) + FOC_SQ(iq_avg_h));

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
        motor_calibration_runtime_after_flux_complete(motor);
    } break;
    default:
        break;
    }
}

/**
 * @brief  Js calibration (Fourier Integral Correlation Method)
 *
 * @note   Principle
 *         ������������������
 *         Apply a sinusoidal torque excitation:
 *           Te(t) = Kt �� Iq_amp �� sin(��f �� t)
 *
 *         For a load with inertia Js and viscous friction B, the
 *         steady-state mechanical velocity response is:
 *           ��(t) = [Te_amp / ��(B? + (Js����f)?)] �� sin(��f��t ? ��)
 *
 *         Fourier correlations over N_calc complete periods:
 *           S_sum = �� ��(t)��sin(��f��t) dt  �� captures B (in-phase)
 *           C_sum = �� ��(t)��cos(��f��t) dt  �� captures Js (90�� lag)
 *
 *         Closed-form inertia estimate (handles viscous friction):
 *           Js = ?(Te_amp �� NT_half �� C_sum) / [��f��(C_sum? + S_sum?)]
 *
 *         Advantages over derivative-based methods:
 *           ? No numerical differentiation (avoids noise amplification)
 *           ? Inherently separates Js from B
 *           ? Multi-period integration averages out measurement noise
 *
 * @note   Safety
 *         ������������
 *         If the actual Js is very small (light load / no load), the
 *         velocity response amplitude  ��_amp = Kt��Iq/(Js����f)  can
 *         exceed motor rated speed.  A hard speed limit is applied:
 *         when |��_mech| > CALIB_JS_OMEGA_MAX the excitation current is
 *         zeroed and the Fourier integrators are paused until the speed
 *         returns to a safe range.  The calibration will still complete
 *         but with reduced accuracy if the limit is frequently hit ��
 *         in that case reduce CALIB_JS_IQ_AMP or check mechanical load.
 *
 * @param  motor  Motor instance pointer
 */
static void foc_calib_js(struct motor_dev *motor)
{
    static uint32_t cycle_cnt = 0;
    static float tc = 0.0f;
    static float S_sum = 0.0f;
    static float C_sum = 0.0f;

    float omega_f = FOC_2PI * DEFAULT_JS_JFS;
    float T_period = 1.0f / DEFAULT_JS_JFS;

    float iq_ref = DEFAULT_JS_JIQ_MAX * foc_fast_sin(omega_f * tc);

    foc_ctrl_curr(motor, 0.0f, iq_ref, motor->meas_data.theta_elec);

    switch (motor->calib_state.js_state) {
    case JS_IDLE: {
        cycle_cnt = 0;
        tc = 0.0f;
        S_sum = 0.0f;
        C_sum = 0.0f;
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
        float Te_amp = motor->calc_params.kt * DEFAULT_JS_JIQ_MAX;
        float NT_half = (float)DEFAULT_JS_CALC_CYCLES * T_period * 0.5f;
        float den = omega_f * (C_sum * C_sum + S_sum * S_sum);

        if (den < 1e-12f)
            den = 1e-12f;

        float Js_est = (-Te_amp * NT_half * C_sum) / den;

        /* Negative Js_est indicates encoder polarity mismatch with the electrical axis.
         * Taking the absolute value recovers the correct magnitude; the user should
         * manually verify encoder direction after calibration. */
        if (Js_est < 0.0f) {
            // motor->enc_pri.config->dir = (motor->enc_pri.config->dir == DIR_CW) ? DIR_CCW : DIR_CW;
            Js_est = FOC_ABS(Js_est);
            /* Note: You can log this event externally via shell to notify the user that direction has been auto-corrected */
        }

        /* Range sanity check */
        if (Js_est > 1e-6f && Js_est < 0.1f) {
            motor->hw_config.js = Js_est;
        }

        motor->calib_state.js_state = JS_END;
    } break;

    case JS_END: {
        motor_calibration_runtime_after_js_complete(motor);
    } break;

    default:
        break;
    }
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
