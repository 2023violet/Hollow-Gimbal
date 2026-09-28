/**
 * @file    motor_calibration_runtime.c
 * @brief   FalconFoc calibration runtime implementation
 * @version v1.1.0
 * @date    2026.05.22
 *
 * Implementation role
 * -------------------
 * - pairs with `motor_calibration_runtime.h`
 * - owns calibration-derived runtime refresh, gain rebinding, and deferred
 *   save/abort glue after hardware-coupled calibration steps complete
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_calibration_runtime.h"

#include "foc_calib.h"
#include "foc_calib_enc.h"
#include "foc_ctrl.h"
#include "foc_restore.h"
#include "motor_fsm.h"
#include "traj_trap.h"

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private Helpers ------------------------------------------------------------------------------ */

static void _motor_calibration_clear_substates(struct motor_dev *motor)
{
    motor->calib_state.rs_state   = RS_IDLE;
    motor->calib_state.ls_state   = LS_IDLE;
    motor->calib_state.flux_state = FX_IDLE;
    motor->calib_state.js_state   = JS_IDLE;
    motor->calib_state.enc_state  = ENC_IDLE;
}

static void _motor_calibration_update_constants(struct motor_dev *motor)
{
    motor->calc_params.inv_poles      = 1.0f / (float)motor->hw_config.poles;
    motor->calc_params.poles_over_2pi = (float)motor->hw_config.poles / FOC_2PI;
    motor->calc_params.inv_gear       = 1.0f / motor->hw_config.gear_ratio;

    motor->calc_params.kt = 1.5f * (float)motor->hw_config.poles * motor->hw_config.flux;
    motor->calc_params.inv_kt =
        (motor->calc_params.kt > 1e-6f) ? 1.0f / motor->calc_params.kt : 1.0f;

    float tor_motor_needed =
        motor->prot_config.tor_peak / (motor->hw_config.gear_ratio * motor->hw_config.gear_eff);
    float iq_limit_by_mech = tor_motor_needed * motor->calc_params.inv_kt;
    float iq_limit_by_heat = motor->prot_config.i_peak;

    motor->ctrl_state.iq_max  = FOC_MIN(iq_limit_by_mech, iq_limit_by_heat);
    motor->ctrl_state.id_max  = motor->ctrl_state.iq_max;
    motor->ctrl_state.tor_max = motor->ctrl_state.iq_max * motor->calc_params.kt *
                                motor->hw_config.gear_ratio * motor->hw_config.gear_eff;
    motor->ctrl_state.vel_max = motor->prot_config.vel_peak;

    motor->ctrl_config.vel_pid.config.max_output     = motor->prot_config.i_rated;
    motor->ctrl_config.vel_pid.config.integral_limit = motor->prot_config.i_rated;
    motor->ctrl_config.pos_pid.config.max_output     = motor->ctrl_state.vel_max;
    motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.vel_max;
}

/* Public Functions ----------------------------------------------------------------------------- */

void foc_calib_runtime_refresh(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    /* Rebind bandwidth-derived current-loop PI gains so paths that rely on
     * the inner loop (I-f open-loop spin, closed-current mode) work from a
     * fresh boot without requiring a prior `cal motor ls` run. */
    foc_curr_gain_calc(motor);

    _motor_calibration_update_constants(motor);
}

void foc_curr_gain_calc(struct motor_dev *motor)
{
    float kp, ki_cont;
    float bandwidth_hz;

    if (motor == NULL) {
        return;
    }

    bandwidth_hz = motor->ctrl_config.curr_bandwidth;
    if ((!isfinite(bandwidth_hz)) || (bandwidth_hz <= 0.0f)) {
        bandwidth_hz = DEFAULT_CURR_BW_HZ;
    }

    float Lq = motor->hw_config.l_q > 1e-6f ? motor->hw_config.l_q : motor->hw_config.ls;
    float Ld = motor->hw_config.l_d > 1e-6f ? motor->hw_config.l_d : motor->hw_config.ls;

    foc_current_pi_gains(Lq, motor->hw_config.rs, bandwidth_hz, &kp, &ki_cont);
    motor->ctrl_config.iq_pid.config.kp = kp;
    motor->ctrl_config.iq_pid.config.ki = ki_cont;

    foc_current_pi_gains(Ld,
                         motor->hw_config.rs,
                         bandwidth_hz * DEFAULT_D_AXIS_BW_RATIO,
                         &kp,
                         &ki_cont);
    motor->ctrl_config.id_pid.config.kp = kp;
    motor->ctrl_config.id_pid.config.ki = ki_cont;
}

void foc_vel_gain_calc(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    float vel_bw_rads = FOC_HZ_TO_RADS(motor->ctrl_config.vel_bandwidth);
    float Kt          = motor->calc_params.kt;
    if (Kt < 1e-6f) {
        Kt = 1e-6f;
    }

    float Kp_rad     = (motor->hw_config.js * vel_bw_rads) / Kt;
    float Ki_rad_cont = Kp_rad * (vel_bw_rads / CALIB_K_DELTA);

    motor->ctrl_config.vel_pid.config.kp = Kp_rad * motor->hw_config.gear_ratio;
    motor->ctrl_config.vel_pid.config.ki = Ki_rad_cont * motor->hw_config.gear_ratio;
}

void foc_pos_gain_calc(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    float max_pos_bw = motor->ctrl_config.vel_bandwidth * DEFAULT_POS_VEL_BW_RATIO;
    float pos_bw_hz  = FOC_MIN(motor->ctrl_config.pos_bandwidth, max_pos_bw);
    float pos_bw_rad = FOC_HZ_TO_RADS(pos_bw_hz);

    motor->ctrl_config.pos_pid.config.kp = pos_bw_rad;
    motor->ctrl_config.pos_pid.config.ki = 0.0f;
}

void motor_calibration_runtime_after_current_offsets(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    if (motor->calib_state.calib_valid_flags == true) {
        motor_fsm_dispatch_calib(motor, CALIB_ENC);
    } else {
        foc_calib_runtime_refresh(motor);
        foc_curr_gain_calc(motor);
        /* 2026.6.30 */
        foc_vel_gain_calc(motor);
    }

    _motor_calibration_clear_substates(motor);
}

void motor_calibration_runtime_after_flux_complete(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    foc_calib_runtime_refresh(motor);
    motor->calib_state.js_state = JS_IDLE;
    motor->calib_state.state    = CALIB_JS;
}

void motor_calibration_runtime_after_js_complete(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    foc_calib_runtime_refresh(motor);
    foc_vel_gain_calc(motor);
    foc_pos_gain_calc(motor);
}

void motor_calibration_runtime_request_encoder_save(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    motor->calib_state.param_save_req |= PARAM_SAVE_ENC;
}

int motor_calibration_runtime_restore_encoder_after_abort(struct motor_dev *motor)
{
    if (motor == NULL) {
        return PARAM_ERR_FLASH;
    }

    return foc_param_load_enc(motor);
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
