/**
 * @file    motor_runtime.c
 * @brief   FalconFoc motor product runtime implementation
 * @version v1.1.0
 * @date    2026.05.22
 *
 * Implementation role
 * -------------------
 * - pairs with `motor_runtime.h`
 * - owns defaults, live runtime binding, restore glue, deferred save handling,
 *   and shell-facing read-only snapshot export
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_runtime.h"

#include "foc_calib.h"
#include "foc_motion_runtime.h"
#include "foc_restore.h"
#include "motor_bench_watchdog.h"
#include "motor_calibration_runtime.h"
#include "motor_fault_runtime.h"
#include "motor_drv.h"
#include "motor_fsm.h"
#include "motor_store.h"
#include "motor_task_config.h"
#include "pid.h"
#include "tm_runtime.h"
#include "traj_trap.h"

#include <math.h>
#include <string.h>

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
#include "encoder_bench_trace.h"
#endif

/* Private State -------------------------------------------------------------------------------- */

static struct motor_hw_config _default_hw_config = {
    .poles = DEFAULT_POLE_PAIRS,
    .rs = DEFAULT_RS,
    .ls = DEFAULT_LS,
    .flux = DEFAULT_FLUX,
    .js = DEFAULT_JS,
    .l_d = DEFAULT_ID,
    .l_q = DEFAULT_IQ,
    .l_d_diff = DEFAULT_IDIQ_DIFF,
    .inertia = DEFAULT_INERTIA,
    .gear_ratio = DEFAULT_GEAR_RATIO,
    .gear_eff = DEFAULT_GEAR_EFF,
    .phase_order = DEFAULT_PHASE_ORDER,
};

static struct motor_prot_config _default_prot_config = {
    .v_rated = DEFAULT_VOLT_RATED,
    .v_bus_ov_thr = DEFAULT_V_BUS_OV,
    .v_bus_uv_thr = DEFAULT_V_BUS_UV,
    .i_rated = DEFAULT_CURR_RATED,
    .i_peak = DEFAULT_CURR_PEAK,
    .vel_rated = DEFAULT_VEL_RATED,
    .vel_peak = DEFAULT_VEL_PEAK,
    .vel_over_thr = DEFAULT_VEL_OVER,
    .tor_rated = DEFAULT_TOR_RATED,
    .tor_peak = DEFAULT_TOR_PEAK,
    .motor_temp_thr = DEFAULT_MOTOR_TEMP,
    .driver_temp_thr = DEFAULT_DRIVER_TEMP,
};

static struct motor_ctrl_config _default_ctrl_config = {
    .ramp_vel = DEFAULT_RAMP_VEL,
    .ramp_accel = DEFAULT_RAMP_ACCEL,
    .ramp_decel = DEFAULT_RAMP_DECEL,
    .traj_vel = DEFAULT_TRAJ_VEL,
    .traj_accel = DEFAULT_TRAJ_ACCEL,
    .traj_decel = DEFAULT_TRAJ_DECEL,
    .traj_jerk = DEFAULT_TRAJ_JERK,
    .curr_bandwidth = DEFAULT_CURR_BW,
    .vel_bandwidth = DEFAULT_VEL_BW,
    .pos_bandwidth = DEFAULT_POS_BW,
};

static struct encoder_config _default_encoder_cfg = {
    .spi_instance = SPI1,
    .cs_port = GPIOA,
    .cs_pin = GPIO_PINS_15,
    .dir = DEFAULT_DIR,
    .mech_offset = DEFAULT_OFFSET,
    .pole_pairs = DEFAULT_POLE_PAIRS,
    .pll_bandwidth = ENCODER_PLL_BANDWIDTH,
    .vel_lpf_bandwidth = ENCODER_LPF_BANDWIDTH,
};

static struct motor_dev *_motor_runtime_current = NULL;

/* Private Helpers ------------------------------------------------------------------------------ */

static void _motor_clear_setpoints(struct motor_dev *motor)
{
    motor->ctrl_state.target_vel = 0.0f;
    motor->ctrl_state.target_iq = 0.0f;
    motor->ctrl_state.target_id = 0.0f;
    motor->ctrl_state.target_pos = motor->meas_data.out_pos_rad;
    motor->ctrl_state.target_tor = 0.0f;

    motor->ctrl_state.vel_set = 0.0f;
    motor->ctrl_state.iq_set = 0.0f;
    motor->ctrl_state.id_set = 0.0f;
    motor->ctrl_state.pos_set = motor->meas_data.out_pos_rad;
    motor->ctrl_state.vel_ff = 0.0f;
    motor->ctrl_state.iq_ff = 0.0f;

    motor->ctrl_state.theta_sim = 0.0f;
    motor->ctrl_state.theta_seed_offset_rad = 0.0f;
    motor->ctrl_state.we_volt_ramp = 0.0f;
    motor->ctrl_state.we_volt_now = 0.0f;
    motor->ctrl_state.if_iq_target = 0.0f;
    motor->ctrl_state.if_enable = 0U;
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
    motor->ctrl_state.bench_vel_step_armed = 0U;
#endif

    motor->ctrl_state.dyn_iq_limit = motor->ctrl_state.iq_max;
    motor->ctrl_state.dyn_vel_limit = motor->ctrl_state.vel_max;

    motor->ctrl_config.pos_pid.config.max_output = motor->ctrl_state.vel_max;
    motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.vel_max;
    motor->ctrl_config.vel_pid.config.max_output = motor->ctrl_state.iq_max;
    motor->ctrl_config.vel_pid.config.integral_limit = motor->ctrl_state.iq_max;
}

static int _motor_load_params(struct motor_dev *motor)
{
    int st;
    bool motor_restored = false;
    bool enc_restored = false;
    bool sys_restored = false;

    st = foc_param_load_motor(motor);
    if (st == PARAM_OK) {
        motor_restored = true;
    }

    st = foc_param_load_enc(motor);
    if (st == PARAM_OK) {
        enc_restored = true;
    }

    st = foc_param_load_sys(motor);
    if (st == PARAM_OK) {
        sys_restored = true;
    }

    if ((!isfinite(motor->ctrl_config.curr_bandwidth)) ||
        (motor->ctrl_config.curr_bandwidth <= 0.0f)) {
        motor->ctrl_config.curr_bandwidth = DEFAULT_CURR_BW_HZ;
    }

    if (motor_restored || enc_restored || sys_restored) {
        motor_restore_after_param_load(motor);
    }

    return 0;
}

static struct pid_dev *_motor_runtime_find_pid(struct motor_dev *motor, const char *loop_name)
{
    if ((motor == NULL) || (loop_name == NULL)) {
        return NULL;
    }

    if (strcmp(loop_name, "id") == 0) {
        return &motor->ctrl_config.id_pid;
    }
    if (strcmp(loop_name, "iq") == 0) {
        return &motor->ctrl_config.iq_pid;
    }
    if (strcmp(loop_name, "vel") == 0) {
        return &motor->ctrl_config.vel_pid;
    }
    if (strcmp(loop_name, "pos") == 0) {
        return &motor->ctrl_config.pos_pid;
    }
    if (strcmp(loop_name, "vvel") == 0) {
        return &motor->ctrl_config.vvel_pid;
    }

    return NULL;
}

static bool _motor_runtime_is_idle(const struct motor_dev *motor)
{
    return (motor != NULL) && (motor->axis_state == AXIS_STATE_IDLE);
}

static void _snapshot_output_motion(const struct motor_dev *motor,
                                    float *out_vel_rpm,
                                    float *out_pos_turns)
{
    float gear_ratio;

    if ((motor == NULL) || (out_vel_rpm == NULL) || (out_pos_turns == NULL)) {
        return;
    }

    /* While running, foc_para_calc() refreshes these cached output fields in
     * the same control snapshot.  After PWM is disabled, the encoder PLL can
     * continue settling without another FOC calculation, so use the current
     * encoder state for a stopped diagnostic snapshot. */
    if (motor->axis_state == AXIS_STATE_RUNNING) {
        *out_vel_rpm = motor->meas_data.out_vel_rpm;
        *out_pos_turns = motor->meas_data.out_pos_turns;
        return;
    }

    gear_ratio = motor->hw_config.gear_ratio;
    if (isfinite(gear_ratio) && (gear_ratio > 1.0e-6f)) {
        *out_vel_rpm = FOC_RADS_TO_RPM(motor->enc_pri.state.velocity_rads / gear_ratio);
        *out_pos_turns = motor->enc_pri.state.total_enc / gear_ratio;
    } else {
        *out_vel_rpm = motor->meas_data.out_vel_rpm;
        *out_pos_turns = motor->meas_data.out_pos_turns;
    }
}

/* Public Functions ----------------------------------------------------------------------------- */

void motor_runtime_init(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    motor_fault_context_clear();
    memset(motor, 0, sizeof(struct motor_dev));

    motor->hw_config = _default_hw_config;
    motor->prot_config = _default_prot_config;
    motor->ctrl_config = _default_ctrl_config;

    motor->axis_state = AXIS_STATE_IDLE;
    motor->ctrl_mode = CTRL_MODE_VOLT;
    motor->input_mode = INPUT_MODE_INACTIVE;
    motor->sensor_type = SENSOR_TYPE_MAGNETIC;

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
    motor_drv_diag_reset_maps();
#endif

    encoder_init(&motor->enc_pri, &_default_encoder_cfg);

    foc_calib_init(motor);
    foc_pid_init(motor);

    traj_trap_init(&motor->traj);
    {
        struct traj_trap_config traj_cfg = {
            .max_vel = motor->ctrl_config.traj_vel,
            .max_acc = motor->ctrl_config.traj_accel,
            .max_dec = motor->ctrl_config.traj_decel,
        };
        (void)traj_trap_config_set(&motor->traj, &traj_cfg);
    }

    (void)_motor_load_params(motor);
    foc_lpf_init(motor);
}

struct motor_dev *motor_runtime_current(void)
{
    return _motor_runtime_current;
}

void motor_runtime_attach(struct motor_dev *motor)
{
    _motor_runtime_current = motor;
}

bool motor_runtime_request_axis_action(enum motor_runtime_axis_action action)
{
    struct motor_dev *motor = _motor_runtime_current;
    enum motor_fsm_event event;
    bool               ok;

    if (motor == NULL) {
        return false;
    }

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
    if ((action == TM_MOTOR_RUNTIME_AXIS_START) &&
        (!motor->meas_data.adc_calib_valid ||
         (motor->meas_data.adc_calib_sample_count != CALIB_CURR_DUR) ||
         (motor->meas_data.adc_calib_fresh_count != CALIB_CURR_DUR))) {
        return false;
    }
#endif

    switch (action) {
    case TM_MOTOR_RUNTIME_AXIS_START:
        event = EV_CMD_START;
        break;
    case TM_MOTOR_RUNTIME_AXIS_STOP:
        event = EV_CMD_STOP;
        break;
    case TM_MOTOR_RUNTIME_AXIS_DISABLE:
        event = EV_CMD_DISABLE;
        break;
    case TM_MOTOR_RUNTIME_AXIS_CLEAR_FAULT:
        event = EV_CMD_CLR_FAULT;
        break;
    default:
        return false;
    }

    ok = (motor_fsm_dispatch(motor, event) == 0);
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
    if (ok && (action == TM_MOTOR_RUNTIME_AXIS_DISABLE)) {
        encoder_bench_trace_freeze();
    }
#endif
    if (ok && (action == TM_MOTOR_RUNTIME_AXIS_CLEAR_FAULT)) {
        motor_fault_context_clear();
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
        encoder_bench_trace_resume();
#endif
    }

    return ok;
}

bool motor_runtime_request_ctrl_mode(enum foc_ctrl_mode mode)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (motor == NULL) {
        return false;
    }

    motor->ctrl_mode = mode;
    return true;
}

bool motor_runtime_request_input_mode(enum foc_input_mode mode)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (motor == NULL) {
        return false;
    }

    motor->input_mode = mode;
    return true;
}

bool motor_runtime_request_set_passthrough(enum foc_ctrl_mode mode, float target, float limit)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (!_motor_runtime_is_idle(motor)) {
        return false;
    }

    foc_cmd_passthrough(motor, mode, target, limit);
    return true;
}

bool motor_runtime_request_vel_ramp(float target_rads, float max_acc_rads)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (motor == NULL) {
        return false;
    }

    foc_cmd_vel_ramp(motor, target_rads, max_acc_rads);
    return true;
}

bool motor_runtime_request_torque_ramp(float target_torque, float max_acc)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (motor == NULL) {
        return false;
    }

    foc_cmd_torque_ramp(motor, target_torque, max_acc);
    return true;
}

bool motor_runtime_request_pos_traj(
    float target_pos_rad,
    float max_vel_rads,
    float max_acc_rads,
    float max_decel_rads
)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (motor == NULL) {
        return false;
    }

    foc_cmd_pos_traj(motor, target_pos_rad, max_vel_rads, max_acc_rads, max_decel_rads);
    return true;
}

bool motor_runtime_request_calibration(enum foc_calib_state calib_type)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (motor == NULL) {
        return false;
    }

    motor_fsm_dispatch_calib(motor, calib_type);
    return true;
}

int motor_runtime_request_calibration_abort(void)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (motor == NULL) {
        return -1;
    }

    return motor_fsm_dispatch(motor, EV_CMD_CALIB_ABORT);
}

void motor_runtime_handle_deferred_save(struct motor_dev *motor)
{
    uint8_t req;

    if (motor == NULL) {
        return;
    }

    req = motor->calib_state.param_save_req;
    if (req == PARAM_SAVE_NONE) {
        return;
    }

    motor->calib_state.param_save_req = PARAM_SAVE_NONE;

    if (req & PARAM_SAVE_ENC) {
        (void)foc_param_save_enc(motor);
    }
    if (req & PARAM_SAVE_MOTOR) {
        (void)foc_param_save_motor(motor);
    }
    if (req & PARAM_SAVE_SYS) {
        (void)foc_param_save_sys(motor);
    }
}

void motor_calib_prepare_entry(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    _motor_clear_setpoints(motor);
    pid_reset(&motor->ctrl_config.iq_pid);
    pid_reset(&motor->ctrl_config.id_pid);
    pid_reset(&motor->ctrl_config.vel_pid);
    pid_reset(&motor->ctrl_config.pos_pid);
}

void motor_restore_after_param_load(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    foc_restore_finalize(motor);
    foc_restore_clear_transient_setpoints(motor);
}

void motor_runtime_reset_transient_setpoints(struct motor_dev *motor)
{
    _motor_clear_setpoints(motor);
}

bool motor_runtime_store_status(struct param_status_all *out)
{
    if (out == NULL) {
        return false;
    }

    foc_param_check_all(out);
    return true;
}

bool motor_runtime_store_layout(uint8_t mask, struct param_save_layout *out)
{
    return foc_param_save_layout(mask, out);
}

bool motor_runtime_request_store_save(uint8_t mask)
{
    struct motor_dev *motor = _motor_runtime_current;

    if ((motor == NULL) || (mask == PARAM_SAVE_NONE)) {
        return false;
    }

    motor->calib_state.param_save_req |= mask;
    return true;
}

bool motor_runtime_request_store_load(
    uint8_t mask,
    struct param_status_all *result_out,
    bool *partial_out
)
{
    struct motor_dev *motor = _motor_runtime_current;
    bool ok = true;
    bool needs_runtime_apply = false;
    int status;

    if ((motor == NULL) || (result_out == NULL) || (partial_out == NULL) ||
        (mask == PARAM_SAVE_NONE)) {
        return false;
    }

    memset(result_out, 0, sizeof(*result_out));
    result_out->motor.status = PARAM_EMPTY;
    result_out->enc.status = PARAM_EMPTY;
    result_out->sys.status = PARAM_EMPTY;
    *partial_out = false;

    if ((mask & PARAM_SAVE_MOTOR) != 0U) {
        status = foc_param_load_motor(motor);
        result_out->motor.status = status;
        if (status == PARAM_OK) {
            needs_runtime_apply = true;
        }
        ok = ok && (status == PARAM_OK);
    }
    if ((mask & PARAM_SAVE_ENC) != 0U) {
        status = foc_param_load_enc(motor);
        result_out->enc.status = status;
        ok = ok && (status == PARAM_OK);
    }
    if ((mask & PARAM_SAVE_SYS) != 0U) {
        status = foc_param_load_sys(motor);
        result_out->sys.status = status;
        if (status == PARAM_OK) {
            needs_runtime_apply = true;
        }
        ok = ok && (status == PARAM_OK);
    }

    if (ok && needs_runtime_apply) {
        motor_restore_after_param_load(motor);
    }

    *partial_out = !ok;
    return true;
}

bool motor_runtime_request_store_erase(void)
{
    if (_motor_runtime_current == NULL) {
        return false;
    }

    foc_param_erase_all();
    return true;
}

bool motor_runtime_diag_set_param(const char *name, float value)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (!_motor_runtime_is_idle(motor) || (name == NULL)) {
        return false;
    }

    if (strcmp(name, "rs") == 0) {
        motor->hw_config.rs = value;
        return true;
    }
    if (strcmp(name, "ls") == 0) {
        motor->hw_config.ls = value;
        return true;
    }
    if (strcmp(name, "flux") == 0) {
        motor->hw_config.flux = value;
        return true;
    }
    if (strcmp(name, "poles") == 0) {
        motor->hw_config.poles = (float)(int)value;
        return true;
    }
    if (strcmp(name, "js") == 0) {
        motor->hw_config.js = value;
        return true;
    }
    if (strcmp(name, "gear") == 0) {
        motor->hw_config.gear_ratio = value;
        return true;
    }
    if (strcmp(name, "encoff") == 0) {
        /* Electrical-zero trim: value is ELECTRICAL radians; converted to the
         * mechanical offset domain by the configured pole pairs. */
        motor->enc_pri.config->mech_offset =
            foc_wrap_pm_pi(motor->enc_pri.config->mech_offset + value / (float)motor->enc_pri.config->pole_pairs);
        return true;
    }
    if (strcmp(name, "vov") == 0) {
        motor->prot_config.v_bus_ov_thr = value;
        return true;
    }
    if (strcmp(name, "vuv") == 0) {
        motor->prot_config.v_bus_uv_thr = value;
        return true;
    }
    if (strcmp(name, "imax") == 0) {
        motor->prot_config.i_peak = value;
        return true;
    }
    if (strcmp(name, "velmax") == 0) {
        motor->prot_config.vel_over_thr = FOC_RPM_TO_RADS(value);
        return true;
    }
    if (strcmp(name, "currbw") == 0) {
        if ((!isfinite(value)) || (value <= 0.0f)) {
            return false;
        }
        motor->ctrl_config.curr_bandwidth = value;
        foc_curr_gain_calc(motor);
        foc_lpf_refresh(motor);
        return true;
    }
    if (strcmp(name, "velbw") == 0) {
        if (value <= 0.0f) {
            return false;
        }
        motor->ctrl_config.vel_bandwidth = value;
        foc_vel_gain_calc(motor);
        return true;
    }
    if (strcmp(name, "posbw") == 0) {
        if (value <= 0.0f) {
            return false;
        }
        motor->ctrl_config.pos_bandwidth = value;
        foc_pos_gain_calc(motor);
        return true;
    }

    return false;
}

bool motor_runtime_diag_set_pid(const char *loop_name, const char *field, float value)
{
    struct motor_dev *motor = _motor_runtime_current;
    struct pid_dev *pid;

    if (!_motor_runtime_is_idle(motor) || (field == NULL)) {
        return false;
    }

    pid = _motor_runtime_find_pid(motor, loop_name);
    if (pid == NULL) {
        return false;
    }

    if (strcmp(field, "kp") == 0) {
        pid->config.kp = value;
        return true;
    }
    if (strcmp(field, "ki") == 0) {
        pid->config.ki = value;
        return true;
    }
    if (strcmp(field, "kd") == 0) {
        pid->config.kd = value;
        return true;
    }
    if (strcmp(field, "lim") == 0) {
        pid->config.max_output = value;
        pid->config.integral_limit = value;
        return true;
    }

    return false;
}

bool motor_runtime_diag_reset_pid(const char *loop_name)
{
    struct motor_dev *motor = _motor_runtime_current;
    struct pid_dev *pid;

    if (!_motor_runtime_is_idle(motor)) {
        return false;
    }

    pid = _motor_runtime_find_pid(motor, loop_name);
    if (pid == NULL) {
        return false;
    }

    pid_reset(pid);
    return true;
}

bool motor_runtime_diag_set_voltage(float vd, float vq, float we_ramp)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (!_motor_runtime_is_idle(motor)) {
        return false;
    }

    foc_cmd_voltage_set(motor, vd, vq, we_ramp);
    return true;
}

bool motor_runtime_diag_set_if(float iq_amp, float we)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (!_motor_runtime_is_idle(motor)) {
        return false;
    }

    foc_cmd_voltage_if(motor, iq_amp, we);
    return true;
}

bool motor_runtime_diag_set_voltloop(float vel_rpm, float vq, float *we_elec_out)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (!_motor_runtime_is_idle(motor) || !isfinite(vel_rpm) || !isfinite(vq) ||
        (vq < 0.0f) || (we_elec_out == NULL)) {
        return false;
    }

    /* Guard against absurd open-loop speed targets; the electrical velocity is
     * derived from the output-shaft rpm so the operator commands one unit. */
    if (fabsf(vel_rpm) > FOC_RADS_TO_RPM(motor->prot_config.vel_rated)) {
        return false;
    }

    float we_elec = FOC_RPM_TO_RADS(vel_rpm) * motor->enc_pri.config->pole_pairs;

    foc_cmd_voltage_set(motor, 0.0f, vq, we_elec);
    *we_elec_out = we_elec;
    return true;
}

bool motor_runtime_request_if_to_vel_handoff(float vel_rpm)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (motor == NULL) {
        return false;
    }

    /* The handoff itself validates the running+I-f precondition; unlike the
     * entry commands it must be reachable while the axis is RUNNING. */
    return foc_cmd_if_to_vel_handoff(motor, vel_rpm);
}

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
bool motor_runtime_request_bench_velocity_step(float vel_rpm)
{
    return foc_cmd_bench_velocity_step(_motor_runtime_current, vel_rpm);
}

bool motor_runtime_diag_read_adc(struct motor_runtime_adc_view *view)
{
    struct motor_dev *motor = _motor_runtime_current;
    uint32_t level;

    if ((motor == NULL) || (view == NULL)) {
        return false;
    }

    level = tm_irq_critical_enter();
    view->ia_raw = motor->meas_data.ia_raw;
    view->ib_raw = motor->meas_data.ib_raw;
    view->ic_raw = motor->meas_data.ic_raw;
    view->ia_offset = motor->meas_data.ia_offset;
    view->ib_offset = motor->meas_data.ib_offset;
    view->ic_offset = motor->meas_data.ic_offset;
    view->ia_residual = motor->meas_data.a;
    view->ib_residual = motor->meas_data.b;
    view->ic_residual = motor->meas_data.c;
    view->sample_seq = motor->meas_data.injected_sample_seq;
    view->sample_count = motor->meas_data.adc_calib_sample_count;
    view->fresh_count = motor->meas_data.adc_calib_fresh_count;
    view->calib_valid = motor->meas_data.adc_calib_valid;
    view->clamp_used = motor->meas_data.adc_calib_clamp_used;
    view->pwm_map = motor_drv_diag_get_pwm_map();
    view->adc_map = motor_drv_diag_get_adc_map();
    view->adc_trigger = motor_drv_diag_get_adc_trigger();
    tm_irq_critical_exit(level);
    return true;
}

bool motor_runtime_diag_zero_current_offsets(void)
{
    struct motor_dev *motor = _motor_runtime_current;

    if ((motor == NULL) || (motor->axis_state != AXIS_STATE_IDLE) ||
        (motor->fault_code != FAULT_NONE)) {
        return false;
    }

#if defined(THEMIS_HOST_TEST)
    return false;
#else
    foc_calib_curr(motor);
    return motor->meas_data.adc_calib_valid;
#endif
}

bool motor_runtime_diag_bench_keepalive(void)
{
    if ((_motor_runtime_current == NULL) ||
        (_motor_runtime_current->axis_state != AXIS_STATE_RUNNING)) {
        return false;
    }

    return motor_bench_watchdog_keepalive();
}

bool motor_runtime_diag_bench_status(struct motor_runtime_bench_status *out)
{
    struct motor_bench_watchdog_status status;

    if (out == NULL) {
        return false;
    }

    motor_bench_watchdog_status_read(&status);
    out->heartbeat_age_ms = (status.heartbeat_age_ticks * 1000U) /
                            TM_MOTOR_BENCH_ADC_TICK_HZ;
    out->timeout_count = status.timeout_count;
    out->armed = status.armed;
    out->expired = status.expired;
    return true;
}

bool motor_runtime_bench_watchdog_tick(const struct motor_dev *motor)
{
    return motor_bench_watchdog_tick((motor != NULL) &&
                                     (motor->axis_state == AXIS_STATE_RUNNING));
}
#endif

bool motor_runtime_diag_set_phase_volt(int phase, float volt)
{
    struct motor_dev *motor = _motor_runtime_current;

    if (!_motor_runtime_is_idle(motor) || (phase < 0) || (phase > 2)) {
        return false;
    }

    foc_cmd_phase_volt(motor, phase, volt);
    return true;
}

bool motor_runtime_diag_set_phase_seed_offset(float offset_rad)
{
    struct motor_dev *motor = _motor_runtime_current;

#if (TM_BENCH_DIAG != 0U)
    if (!_motor_runtime_is_idle(motor) || !isfinite(offset_rad) ||
        (offset_rad < -FOC_PI) || (offset_rad > FOC_PI)) {
        return false;
    }

    motor->ctrl_state.theta_seed_offset_rad = offset_rad;
    return true;
#else
    (void)motor;
    (void)offset_rad;
    return false;
#endif
}

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
bool motor_runtime_diag_set_encoder_dir(uint8_t dir)
{
    if (!_motor_runtime_is_idle(_motor_runtime_current) ||
        ((dir != DIR_CW) && (dir != DIR_CCW))) {
        return false;
    }

    _motor_runtime_current->enc_pri.config->dir = dir;
    return true;
}

bool motor_runtime_diag_set_encoder_abs(uint8_t dir, float mech_offset)
{
    if (!_motor_runtime_is_idle(_motor_runtime_current) ||
        ((dir != DIR_CW) && (dir != DIR_CCW)) || !isfinite(mech_offset) ||
        (mech_offset < -FOC_PI) || (mech_offset > FOC_PI)) {
        return false;
    }

    _motor_runtime_current->enc_pri.config->dir = dir;
    _motor_runtime_current->enc_pri.config->mech_offset = mech_offset;
    return true;
}

bool motor_runtime_diag_set_pwm_map(uint8_t map_id)
{
    if (!_motor_runtime_is_idle(_motor_runtime_current)) {
        return false;
    }

    return motor_drv_diag_set_pwm_map(map_id);
}

bool motor_runtime_diag_set_adc_map(uint8_t map_id)
{
    if (!_motor_runtime_is_idle(_motor_runtime_current)) {
        return false;
    }

    return motor_drv_diag_set_adc_map(map_id);
}

void motor_runtime_diag_reset_maps(void)
{
    if (_motor_runtime_is_idle(_motor_runtime_current)) {
        motor_drv_diag_reset_maps();
    }
}

bool motor_runtime_diag_set_adc_trigger(uint16_t compare)
{
    if (!_motor_runtime_is_idle(_motor_runtime_current)) {
        return false;
    }

    return motor_drv_diag_set_adc_trigger(compare);
}

bool motor_runtime_diag_read_bench_maps(uint8_t *pwm_map,
                                        uint8_t *adc_map,
                                        uint16_t *adc_trigger)
{
    if ((pwm_map == NULL) || (adc_map == NULL) || (adc_trigger == NULL) ||
        (_motor_runtime_current == NULL)) {
        return false;
    }

    *pwm_map = motor_drv_diag_get_pwm_map();
    *adc_map = motor_drv_diag_get_adc_map();
    *adc_trigger = motor_drv_diag_get_adc_trigger();
    return true;
}

bool motor_runtime_diag_encoder_trace_clear(void)
{
    if (!_motor_runtime_is_idle(_motor_runtime_current)) {
        return false;
    }

    encoder_bench_trace_clear();
    return true;
}

bool motor_runtime_diag_encoder_trace_read(uint32_t index,
                                           struct encoder_bench_sample *out)
{
    return encoder_bench_trace_read(index, out);
}

uint32_t motor_runtime_diag_encoder_trace_count(void)
{
    return encoder_bench_trace_count();
}
#endif

bool motor_runtime_diag_read_params(struct motor_runtime_diag_param_view *view)
{
    struct motor_dev *motor = _motor_runtime_current;
    uint32_t level;

    if ((motor == NULL) || (view == NULL)) {
        return false;
    }

    level = tm_irq_critical_enter();
    view->poles = motor->hw_config.poles;
    view->rs = motor->hw_config.rs;
    view->ls = motor->hw_config.ls;
    view->flux = motor->hw_config.flux;
    view->js = motor->hw_config.js;
    view->gear_ratio = motor->hw_config.gear_ratio;
    view->v_bus_ov_thr = motor->prot_config.v_bus_ov_thr;
    view->v_bus_uv_thr = motor->prot_config.v_bus_uv_thr;
    view->i_peak = motor->prot_config.i_peak;
    view->vel_over_rpm = FOC_RADS_TO_RPM(motor->prot_config.vel_over_thr);
    view->curr_bandwidth = motor->ctrl_config.curr_bandwidth;
    view->vel_bandwidth = motor->ctrl_config.vel_bandwidth;
    view->pos_bandwidth = motor->ctrl_config.pos_bandwidth;
    tm_irq_critical_exit(level);
    return true;
}

bool motor_runtime_diag_read_pid(const char *loop_name, struct motor_runtime_pid_view *view)
{
    struct motor_dev *motor = _motor_runtime_current;
    struct pid_dev *pid;
    uint32_t level;

    if ((motor == NULL) || (view == NULL)) {
        return false;
    }

    pid = _motor_runtime_find_pid(motor, loop_name);
    if (pid == NULL) {
        return false;
    }

    level = tm_irq_critical_enter();
    view->kp = pid->config.kp;
    view->ki = pid->config.ki;
    view->kd = pid->config.kd;
    view->lim = pid->config.max_output;
    tm_irq_critical_exit(level);
    return true;
}

bool motor_runtime_snapshot(struct motor_runtime_snapshot *snapshot)
{
    struct motor_dev *motor;
    uint32_t level;

    if (snapshot == NULL) {
        return false;
    }

    motor = _motor_runtime_current;
    if (motor == NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        return false;
    }

    level = tm_irq_critical_enter();
    snapshot->axis_state = (uint8_t)motor->axis_state;
    snapshot->ctrl_mode = (uint8_t)motor->ctrl_mode;
    snapshot->input_mode = (uint8_t)motor->input_mode;
    snapshot->sensor_type = (uint8_t)motor->sensor_type;
    snapshot->fault_code = motor->fault_code;
    snapshot->calib_state = (uint8_t)motor->calib_state.state;
    snapshot->rs_state = (uint8_t)motor->calib_state.rs_state;
    snapshot->ls_state = (uint8_t)motor->calib_state.ls_state;
    snapshot->flux_state = (uint8_t)motor->calib_state.flux_state;
    snapshot->js_state = (uint8_t)motor->calib_state.js_state;
    snapshot->enc_state = (uint8_t)motor->calib_state.enc_state;
    snapshot->enc_valid = motor->enc_pri.state.calib_valid;
    snapshot->v_bus = motor->meas_data.v_bus;
    snapshot->i_q = motor->foc_calc.i_q;
    snapshot->i_d = motor->foc_calc.i_d;
    snapshot->ia_raw = motor->meas_data.ia_raw;
    snapshot->ib_raw = motor->meas_data.ib_raw;
    snapshot->ic_raw = motor->meas_data.ic_raw;
    snapshot->ia_offset = motor->meas_data.ia_offset;
    snapshot->ib_offset = motor->meas_data.ib_offset;
    snapshot->ic_offset = motor->meas_data.ic_offset;
    snapshot->ia = motor->meas_data.a;
    snapshot->ib = motor->meas_data.b;
    snapshot->ic = motor->meas_data.c;
    snapshot->v_bus_raw = motor->meas_data.v_bus_raw;
    snapshot->theta_sim = motor->ctrl_state.theta_sim;
    snapshot->theta_elec = motor->enc_pri.state.angle_elec_rad;
    snapshot->we_volt_now = motor->ctrl_state.we_volt_now;
    snapshot->we_volt_ramp = motor->ctrl_state.we_volt_ramp;
    snapshot->vd = motor->foc_calc.v_d;
    snapshot->vq = motor->foc_calc.v_q;
    snapshot->v_alpha = motor->foc_calc.v_alpha;
    snapshot->v_beta = motor->foc_calc.v_beta;
    snapshot->dtc_a = motor->foc_calc.dtc_a;
    snapshot->dtc_b = motor->foc_calc.dtc_b;
    snapshot->dtc_c = motor->foc_calc.dtc_c;
    snapshot->sector = motor->foc_calc.sector;
    _snapshot_output_motion(motor, &snapshot->out_vel_rpm, &snapshot->out_pos_turns);
    snapshot->motor_temp = motor->meas_data.motor_temp;
    snapshot->driver_temp = motor->meas_data.driver_temp;
    snapshot->angle_mech_rad = motor->enc_pri.state.angle_mech_rad;
    snapshot->angle_elec_rad = motor->enc_pri.state.angle_elec_rad;
    snapshot->encoder_vel_rads = motor->enc_pri.state.velocity_rads;
    snapshot->mech_offset = motor->enc_pri.config->mech_offset;
    snapshot->pole_pairs = motor->enc_pri.config->pole_pairs;
    snapshot->save_req = motor->calib_state.param_save_req;
    tm_irq_critical_exit(level);
    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
