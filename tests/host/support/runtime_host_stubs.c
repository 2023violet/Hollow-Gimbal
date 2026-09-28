/**
 * @file    runtime_host_stubs.c
 * @brief   Shared host stubs for FalconFoc runtime-owner unit tests
 * @version v1.0.0
 * @date    2026.05.21
 */

#include <math.h>
#include <string.h>

#include "encoder.h"
#include "foc_calib.h"
#include "foc_motion_runtime.h"
#include "foc_param.h"
#include "motor_fault_runtime.h"
#include "motor_calibration_runtime.h"
#include "motor_fsm.h"
#include "../../../applications/periph/motor_bench_mapping.h"

spi_type  g_host_spi1  = {0};
gpio_type g_host_gpioa = {0};

static uint32_t g_foc_lpf_init_count    = 0U;
static uint32_t g_foc_lpf_refresh_count = 0U;
static uint32_t g_fault_context_clear_count = 0U;
static struct motor_fault_context g_fault_context;
static bool                       g_fault_context_valid = false;
static int      g_motor_load_status     = PARAM_EMPTY;
static float    g_motor_load_curr_bw   = DEFAULT_CURR_BW;
static uint8_t  g_bench_pwm_map        = MOTOR_BENCH_MAP_DEFAULT;
static uint8_t  g_bench_adc_map        = MOTOR_BENCH_MAP_DEFAULT;
static uint16_t g_bench_adc_trigger    = MOTOR_BENCH_ADC_TRIGGER_DEFAULT;

bool motor_drv_diag_set_pwm_map(uint8_t map_id)
{
    if (map_id >= MOTOR_BENCH_MAP_COUNT) {
        return false;
    }

    g_bench_pwm_map = map_id;
    return true;
}

bool motor_drv_diag_set_adc_map(uint8_t map_id)
{
    if (map_id >= MOTOR_BENCH_MAP_COUNT) {
        return false;
    }

    g_bench_adc_map = map_id;
    return true;
}

void motor_drv_diag_reset_maps(void)
{
    g_bench_pwm_map     = MOTOR_BENCH_MAP_DEFAULT;
    g_bench_adc_map     = MOTOR_BENCH_MAP_DEFAULT;
    g_bench_adc_trigger = MOTOR_BENCH_ADC_TRIGGER_DEFAULT;
}

uint8_t motor_drv_diag_get_pwm_map(void)
{
    return g_bench_pwm_map;
}

uint8_t motor_drv_diag_get_adc_map(void)
{
    return g_bench_adc_map;
}

bool motor_drv_diag_set_adc_trigger(uint16_t compare)
{
    if ((compare < MOTOR_BENCH_ADC_TRIGGER_MIN) ||
        (compare > MOTOR_BENCH_ADC_TRIGGER_MAX)) {
        return false;
    }

    g_bench_adc_trigger = compare;
    return true;
}

uint16_t motor_drv_diag_get_adc_trigger(void)
{
    return g_bench_adc_trigger;
}

uint8_t host_runtime_stub_get_pwm_map(void)
{
    return g_bench_pwm_map;
}

uint8_t host_runtime_stub_get_adc_map(void)
{
    return g_bench_adc_map;
}

uint16_t host_runtime_stub_get_adc_trigger(void)
{
    return g_bench_adc_trigger;
}

void host_runtime_stub_reset_bench_maps(void)
{
    motor_drv_diag_reset_maps();
}

void encoder_init(struct encoder_dev *enc, struct encoder_config *cfg)
{
    if (enc == NULL) {
        return;
    }

    memset(enc, 0, sizeof(*enc));
    enc->config = cfg;
}

void encoder_update(struct encoder_dev *enc)
{
    (void)enc;
}

void foc_calib_init(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    motor->calib_state.calib_valid_flags = CALIB_VALID_FLAG;
}

void foc_pid_init(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    (void)pid_reset(&motor->ctrl_config.id_pid);
    (void)pid_reset(&motor->ctrl_config.iq_pid);
    (void)pid_reset(&motor->ctrl_config.vel_pid);
    (void)pid_reset(&motor->ctrl_config.pos_pid);
}

void foc_lpf_init(struct motor_dev *motor)
{
    float curr_bw;
    float cutoff_hz;

    if (motor == NULL) {
        return;
    }

    curr_bw = motor->ctrl_config.curr_bandwidth;
    if ((!isfinite(curr_bw)) || (curr_bw <= 0.0f)) {
        curr_bw = DEFAULT_CURR_BW_HZ;
    }
    cutoff_hz = curr_bw * (DEFAULT_CUTOFF_HZ / DEFAULT_CURR_BW_HZ);

    lpf_init(&motor->ctrl_state.id_filter, cutoff_hz, DEFAULT_SAMPLING_HZ);
    lpf_init(&motor->ctrl_state.iq_filter, cutoff_hz, DEFAULT_SAMPLING_HZ);
    g_foc_lpf_init_count++;
}

void foc_lpf_refresh(struct motor_dev *motor)
{
    float curr_bw;
    float cutoff_hz;
    float alpha;

    if (motor == NULL) {
        return;
    }

    curr_bw = motor->ctrl_config.curr_bandwidth;
    if ((!isfinite(curr_bw)) || (curr_bw <= 0.0f)) {
        curr_bw = DEFAULT_CURR_BW_HZ;
    }
    cutoff_hz = curr_bw * (DEFAULT_CUTOFF_HZ / DEFAULT_CURR_BW_HZ);
    alpha     = FOC_HZ_TO_RADS(cutoff_hz) /
            (FOC_HZ_TO_RADS(cutoff_hz) + DEFAULT_SAMPLING_HZ);
    motor->ctrl_state.id_filter.alpha = alpha;
    motor->ctrl_state.iq_filter.alpha = alpha;
    g_foc_lpf_refresh_count++;
}

void host_runtime_stub_reset_lpf_counters(void)
{
    g_foc_lpf_init_count    = 0U;
    g_foc_lpf_refresh_count = 0U;
}

uint32_t host_runtime_stub_get_lpf_init_count(void)
{
    return g_foc_lpf_init_count;
}

uint32_t host_runtime_stub_get_lpf_refresh_count(void)
{
    return g_foc_lpf_refresh_count;
}

void host_runtime_stub_reset_fault_context_counters(void)
{
    g_fault_context_clear_count = 0U;
}

uint32_t host_runtime_stub_get_fault_context_clear_count(void)
{
    return g_fault_context_clear_count;
}

void host_runtime_stub_set_fault_context(const struct motor_fault_context *context, bool valid)
{
    if ((context == NULL) || !valid) {
        memset(&g_fault_context, 0, sizeof(g_fault_context));
        g_fault_context_valid = false;
        return;
    }

    g_fault_context       = *context;
    g_fault_context_valid = true;
}

bool motor_fault_context_read(struct motor_fault_context *out)
{
    if (out == NULL) {
        return false;
    }

    if (!g_fault_context_valid) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    *out = g_fault_context;
    return true;
}

void motor_fault_context_clear(void)
{
    g_fault_context_clear_count++;
    memset(&g_fault_context, 0, sizeof(g_fault_context));
    g_fault_context_valid = false;
}

void host_runtime_stub_set_motor_load(int status, float curr_bandwidth)
{
    g_motor_load_status   = status;
    g_motor_load_curr_bw = curr_bandwidth;
}

int foc_param_save_motor(const struct motor_dev *motor)
{
    (void)motor;
    return PARAM_OK;
}

int foc_param_load_motor(struct motor_dev *motor)
{
    if (motor == NULL) {
        return PARAM_ERR_FLASH;
    }

    if (g_motor_load_status == PARAM_OK) {
        motor->ctrl_config.curr_bandwidth = g_motor_load_curr_bw;
    }

    return g_motor_load_status;
}

int foc_param_save_enc(const struct motor_dev *motor)
{
    (void)motor;
    return PARAM_OK;
}

int foc_param_load_enc(struct motor_dev *motor)
{
    (void)motor;
    return PARAM_EMPTY;
}

int foc_param_save_sys(const struct motor_dev *motor)
{
    (void)motor;
    return PARAM_OK;
}

int foc_param_load_sys(struct motor_dev *motor)
{
    (void)motor;
    return PARAM_EMPTY;
}

void foc_param_check_all(struct param_status_all *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

bool foc_param_save_layout(uint8_t mask, struct param_save_layout *out)
{
    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    switch (mask) {
    case PARAM_SAVE_MOTOR:
        out->payload_size     = 88U;
        out->flash_total_size = 104U;
        return true;
    case PARAM_SAVE_ENC:
        out->payload_size     = 520U;
        out->flash_total_size = 536U;
        return true;
    case PARAM_SAVE_SYS:
        out->payload_size     = 8U;
        out->flash_total_size = 24U;
        return true;
    default:
        return false;
    }
}

void foc_param_erase_all(void)
{
}

const char *foc_param_status_str(int st)
{
    switch (st) {
    case PARAM_OK:
        return "OK";
    case PARAM_EMPTY:
        return "EMPTY";
    case PARAM_ERR_FLASH:
        return "FLASH";
    default:
        return "ERR";
    }
}

#if !defined(THEMIS_HOST_RUNTIME_STUB_SKIP_CALIB_DISPATCH)
void motor_fsm_dispatch_calib(struct motor_dev *motor, enum foc_calib_state calib_type)
{
    if (motor != NULL) {
        motor->calib_state.state = calib_type;
    }
}
#endif

int motor_fsm_dispatch(struct motor_dev *motor, enum motor_fsm_event event)
{
    if (motor == NULL) {
        return -1;
    }

    switch (event) {
    case EV_CMD_START:
        motor->axis_state = AXIS_STATE_RUNNING;
        break;
    case EV_CMD_STOP:
    case EV_CMD_CLR_FAULT:
    case EV_CMD_CALIB_ABORT:
        motor->axis_state = AXIS_STATE_IDLE;
        if (event == EV_CMD_CLR_FAULT) {
            motor->fault_code = FAULT_NONE;
        }
        break;
    case EV_CMD_DISABLE:
    case EV_FAULT_TRIP:
        motor->axis_state = AXIS_STATE_FAULT;
        break;
    default:
        break;
    }

    return 0;
}

void foc_cmd_passthrough(struct motor_dev *motor,
                         enum foc_ctrl_mode mode,
                         float              target,
                         float              limit)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode  = mode;
    motor->input_mode = INPUT_MODE_PASSTHROUGH;

    if (mode == CTRL_MODE_CURR) {
        motor->ctrl_state.target_iq = target;
    } else if (mode == CTRL_MODE_VEL) {
        motor->ctrl_state.target_vel   = target;
        motor->ctrl_state.dyn_iq_limit = limit;
    } else if (mode == CTRL_MODE_POS) {
        motor->ctrl_state.target_pos    = target;
        motor->ctrl_state.dyn_vel_limit = limit;
    }
}

void foc_cmd_vel_ramp(struct motor_dev *motor, float new_vel, float max_acc)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode             = CTRL_MODE_VEL;
    motor->input_mode            = INPUT_MODE_VEL_RAMP;
    motor->ctrl_state.target_vel = new_vel;
    motor->ctrl_config.ramp_accel = max_acc;
}

void foc_cmd_torque_ramp(struct motor_dev *motor, float new_torque, float max_acc)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode             = CTRL_MODE_CURR;
    motor->input_mode            = INPUT_MODE_TORQUE_RAMP;
    motor->ctrl_state.target_tor = new_torque;
    motor->ctrl_config.ramp_accel = max_acc;
}

void foc_cmd_pos_traj(struct motor_dev *motor,
                      float             new_pos_rad,
                      float             max_vel,
                      float             max_acc,
                      float             max_decel)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode              = CTRL_MODE_POS;
    motor->input_mode             = INPUT_MODE_TRAP_TRAJ;
    motor->ctrl_state.target_pos  = new_pos_rad;
    motor->ctrl_state.dyn_vel_limit = max_vel;
    (void)max_acc;
    (void)max_decel;
}

#if !defined(THEMIS_HOST_RUNTIME_STUB_SKIP_VOLTAGE_SET)
void foc_cmd_voltage_set(struct motor_dev *motor, float vd_set, float vq_set, float we_ramp)
{
    if (motor == NULL) {
        return;
    }

    if (motor->ctrl_mode != CTRL_MODE_VOLT) {
        motor->ctrl_state.theta_sim = motor->meas_data.theta_elec;
    }

    motor->ctrl_mode              = CTRL_MODE_VOLT;
    motor->input_mode             = INPUT_MODE_INACTIVE;
    motor->ctrl_state.vd_set      = vd_set;
    motor->ctrl_state.vq_set      = vq_set;
    motor->ctrl_state.we_volt_ramp = we_ramp;
}
#endif

void foc_cmd_voltage_if(struct motor_dev *motor, float iq_amp, float we)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode               = CTRL_MODE_VOLT;
    motor->input_mode              = INPUT_MODE_INACTIVE;
    motor->ctrl_state.if_enable   = 1U;
    motor->ctrl_state.if_iq_target = iq_amp;
    motor->ctrl_state.we_volt_ramp = we;
}

bool foc_cmd_if_to_vel_handoff(struct motor_dev *motor, float vel_rpm)
{
    (void)motor;
    (void)vel_rpm;
    return true;
}

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
bool foc_cmd_bench_velocity_step(struct motor_dev *motor, float vel_rpm)
{
    (void)motor;
    (void)vel_rpm;
    return true;
}
#endif

void foc_cmd_phase_volt(struct motor_dev *motor, int phase, float volt)
{
    (void)phase;
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode              = CTRL_MODE_VOLT;
    motor->input_mode             = INPUT_MODE_INACTIVE;
    motor->ctrl_state.if_enable   = 0U;
    motor->ctrl_state.vd_set      = volt;
    motor->ctrl_state.vq_set      = 0.0f;
    motor->ctrl_state.we_volt_ramp = 0.0f;
}

#if !defined(THEMIS_HOST_RUNTIME_STUB_SKIP_GAIN_HELPERS)
void foc_curr_gain_calc(struct motor_dev *motor)
{
    (void)motor;
}

void foc_vel_gain_calc(struct motor_dev *motor)
{
    (void)motor;
}

void foc_pos_gain_calc(struct motor_dev *motor)
{
    (void)motor;
}
#endif

void foc_restore_finalize(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_state.dyn_iq_limit  = motor->ctrl_state.iq_max;
    motor->ctrl_state.dyn_vel_limit = motor->ctrl_state.vel_max;
}

void foc_restore_clear_transient_setpoints(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_state.target_vel = 0.0f;
    motor->ctrl_state.target_iq  = 0.0f;
    motor->ctrl_state.target_id  = 0.0f;
    motor->ctrl_state.target_pos = motor->meas_data.out_pos_rad;
    motor->ctrl_state.target_tor = 0.0f;
    motor->ctrl_state.vel_set    = 0.0f;
    motor->ctrl_state.iq_set     = 0.0f;
    motor->ctrl_state.id_set     = 0.0f;
    motor->ctrl_state.pos_set    = motor->meas_data.out_pos_rad;
    motor->ctrl_state.vel_ff     = 0.0f;
    motor->ctrl_state.iq_ff      = 0.0f;
}
