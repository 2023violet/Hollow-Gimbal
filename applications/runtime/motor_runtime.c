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
#include "motor_calibration_runtime.h"
#include "motor_drv.h"
#include "motor_fsm.h"
#include "motor_store.h"
#include "pid.h"
#include "tm_runtime.h"
#include "traj_trap.h"

#include <string.h>

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
    motor->ctrl_state.we_volt_ramp = 0.0f;

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

    return NULL;
}

/* Public Functions ----------------------------------------------------------------------------- */

void motor_runtime_init(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    memset(motor, 0, sizeof(struct motor_dev));

    motor->hw_config = _default_hw_config;
    motor->prot_config = _default_prot_config;
    motor->ctrl_config = _default_ctrl_config;

    motor->axis_state = AXIS_STATE_IDLE;
    motor->ctrl_mode = CTRL_MODE_VOLT;
    motor->input_mode = INPUT_MODE_INACTIVE;
    motor->sensor_type = SENSOR_TYPE_MAGNETIC;

    encoder_init(&motor->enc_pri, &_default_encoder_cfg);

    foc_calib_init(motor);
    foc_pid_init(motor);
    foc_lpf_init(motor);

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

    if (motor == NULL) {
        return false;
    }

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

    return (motor_fsm_dispatch(motor, event) == 0);
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

    if (motor == NULL) {
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

    if ((motor == NULL) || (name == NULL)) {
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
        if (value <= 0.0f) {
            return false;
        }
        motor->ctrl_config.curr_bandwidth = value;
        foc_curr_gain_calc(motor);
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

    if ((motor == NULL) || (field == NULL)) {
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

    if (motor == NULL) {
        return false;
    }

    foc_cmd_voltage_set(motor, vd, vq, we_ramp);
    return true;
}

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
    snapshot->out_vel_rpm = motor->meas_data.out_vel_rpm;
    snapshot->out_pos_turns = motor->meas_data.out_pos_turns;
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
