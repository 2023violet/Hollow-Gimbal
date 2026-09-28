/**
 * @file    test_app_shell_diag_contract.c
 * @brief   Host contract tests for FalconFoc `diag` shell ownership boundaries
 * @version v1.0.0
 * @date    2026.05.15
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell_commands.h"
#include "foc_common.h"
#include "foc_motion_runtime.h"
#include "motor_fault_runtime.h"
#include "motor_calibration_runtime.h"
#include "motor_bench_mapping.h"
#include "encoder_bench_trace.h"
#include "motor_runtime.h"
#include "tm_shell.h"

#define TEST_ASSERT(expr)                                                        \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(                                                             \
                stderr,                                                          \
                "[test_app_shell_diag_contract] assertion failed: %s (%s:%d)\n", \
                #expr,                                                           \
                __FILE__,                                                        \
                __LINE__                                                         \
            );                                                                   \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

struct motor_dev _motor_ins;

static char g_output[1024];
static size_t g_output_len;
static int g_passthrough_calls;
static int g_vel_ramp_calls;
static int g_torque_ramp_calls;
static int g_pos_traj_calls;
static int g_request_param_calls;
static int g_request_pid_calls;
static int g_request_voltage_calls;
static int g_request_set_calls;
static int g_request_ramp_calls;
static int g_last_pid_loop;
static int g_last_pid_field;
static float g_last_pid_value;
static int g_last_param_field;
static float g_last_param_value;
static float g_last_voltage_vd;
static float g_last_voltage_vq;
static float g_last_voltage_we;
static int g_last_set_mode;
static float g_last_set_target;
static float g_last_set_limit;
static int g_last_ramp_kind;
static float g_last_ramp_value0;
static float g_last_ramp_value1;
static float g_last_ramp_value2;
static float g_last_ramp_value3;
static struct motor_bench_trace g_trace;
static bool                    g_trace_valid;
static int                     g_trace_clear_calls;
static struct motor_bench_sample g_trace_samples[MOTOR_BENCH_SAMPLE_RING_DEPTH];
static uint32_t                  g_trace_sample_count;
static int                     g_phase_seed_offset_calls;
static float                   g_phase_seed_offset;
static uint8_t                 g_encoder_dir;
static int                     g_encoder_dir_set_calls;
static int                     g_encoder_abs_set_calls;
static float                   g_encoder_abs_offset;
static uint8_t                  g_pwm_map;
static uint8_t                  g_adc_map;
static uint16_t                 g_adc_trigger;
static int                      g_pwm_map_set_calls;
static int                      g_adc_map_set_calls;
static int                      g_map_reset_calls;
static int                      g_adc_trigger_set_calls;
static struct encoder_bench_sample g_encoder_trace_samples[ENCODER_BENCH_TRACE_DEPTH];
static uint32_t                  g_encoder_trace_sample_count;
static int                       g_encoder_trace_clear_calls;
static int                       g_velocity_step_calls;
static float                     g_last_velocity_step_rpm;
static bool                      g_velocity_step_result;
static int                       g_adc_show_calls;
static int                       g_adc_zero_calls;
static bool                      g_adc_zero_result;
static int                       g_bench_keepalive_calls;
static bool                      g_bench_keepalive_result;
static struct motor_runtime_bench_status g_bench_status;

static void test_env_reset(void)
{
    memset(&_motor_ins, 0, sizeof(_motor_ins));
    motor_runtime_attach(&_motor_ins);
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    g_passthrough_calls = 0;
    g_vel_ramp_calls = 0;
    g_torque_ramp_calls = 0;
    g_pos_traj_calls = 0;
    g_request_param_calls = 0;
    g_request_pid_calls = 0;
    g_request_voltage_calls = 0;
    g_request_set_calls = 0;
    g_request_ramp_calls = 0;
    g_last_pid_loop = -1;
    g_last_pid_field = -1;
    g_last_pid_value = 0.0f;
    g_last_param_field = -1;
    g_last_param_value = 0.0f;
    g_last_voltage_vd = 0.0f;
    g_last_voltage_vq = 0.0f;
    g_last_voltage_we = 0.0f;
    g_last_set_mode = -1;
    g_last_set_target = 0.0f;
    g_last_set_limit = 0.0f;
    g_last_ramp_kind = -1;
    g_last_ramp_value0 = 0.0f;
    g_last_ramp_value1 = 0.0f;
    g_last_ramp_value2 = 0.0f;
    g_last_ramp_value3 = 0.0f;
    memset(&g_trace, 0, sizeof(g_trace));
    g_trace_valid = false;
    g_trace_clear_calls = 0;
    memset(g_trace_samples, 0, sizeof(g_trace_samples));
    g_trace_sample_count = 0U;
    g_phase_seed_offset_calls = 0;
    g_phase_seed_offset = 0.0f;
    g_encoder_dir = DIR_CCW;
    g_encoder_dir_set_calls = 0;
    g_encoder_abs_set_calls = 0;
    g_encoder_abs_offset = 0.0f;
    g_pwm_map = 0U;
    g_adc_map = 0U;
    g_adc_trigger = MOTOR_BENCH_ADC_TRIGGER_DEFAULT;
    g_pwm_map_set_calls = 0;
    g_adc_map_set_calls = 0;
    g_map_reset_calls = 0;
    g_adc_trigger_set_calls = 0;
    memset(g_encoder_trace_samples, 0, sizeof(g_encoder_trace_samples));
    g_encoder_trace_sample_count = 0U;
    g_encoder_trace_clear_calls = 0;
    g_velocity_step_calls = 0;
    g_last_velocity_step_rpm = 0.0f;
    g_velocity_step_result = true;
    g_adc_show_calls = 0;
    g_adc_zero_calls = 0;
    g_adc_zero_result = true;
    g_bench_keepalive_calls = 0;
    g_bench_keepalive_result = true;
    memset(&g_bench_status, 0, sizeof(g_bench_status));

    _motor_ins.ctrl_state.iq_max = 10.0f;
    _motor_ins.ctrl_state.vel_max = 100.0f;
    _motor_ins.ctrl_state.dyn_iq_limit = 10.0f;
    _motor_ins.ctrl_state.dyn_vel_limit = 100.0f;
    _motor_ins.hw_config.gear_ratio = 1.0f;
    _motor_ins.calc_params.inv_kt = 1.0f;
    _motor_ins.ctrl_mode = CTRL_MODE_VEL;
    _motor_ins.input_mode = INPUT_MODE_PASSTHROUGH;
}

void shell_printf(const char *fmt, ...)
{
    va_list args;
    int written;

    TEST_ASSERT(fmt != NULL);
    TEST_ASSERT(g_output_len < sizeof(g_output));

    va_start(args, fmt);
    written = vsnprintf(&g_output[g_output_len], sizeof(g_output) - g_output_len, fmt, args);
    va_end(args);

    TEST_ASSERT(written >= 0);
    TEST_ASSERT((size_t)written < (sizeof(g_output) - g_output_len));
    g_output_len += (size_t)written;
}

const char *app_shell_axis_state_name(uint8_t state)
{
    switch (state) {
    case AXIS_STATE_IDLE:
        return "idle";
    case AXIS_STATE_CALIB:
        return "calib";
    case AXIS_STATE_RUNNING:
        return "running";
    case AXIS_STATE_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

const char *app_shell_ctrl_mode_name(uint8_t mode)
{
    switch (mode) {
    case CTRL_MODE_VOLT:
        return "volt";
    case CTRL_MODE_CURR:
        return "curr";
    case CTRL_MODE_VEL:
        return "vel";
    case CTRL_MODE_POS:
        return "pos";
    default:
        return "unknown";
    }
}

const char *app_shell_input_mode_name(uint8_t mode)
{
    switch (mode) {
    case INPUT_MODE_INACTIVE:
        return "inactive";
    case INPUT_MODE_PASSTHROUGH:
        return "pass";
    case INPUT_MODE_VEL_RAMP:
        return "vel-ramp";
    case INPUT_MODE_TORQUE_RAMP:
        return "torque-ramp";
    case INPUT_MODE_POS_FILTER:
        return "pos-filter";
    case INPUT_MODE_VEL_FILTER:
        return "vel-filter";
    case INPUT_MODE_TRAP_TRAJ:
        return "traj";
    case INPUT_MODE_TUNING:
        return "tuning";
    default:
        return "unknown";
    }
}

int app_shell_parse_float_arg(const char *text, float *out)
{
    char *end = NULL;

    TEST_ASSERT(text != NULL);
    TEST_ASSERT(out != NULL);

    *out = strtof(text, &end);
    if (end == text) {
        return -1;
    }

    return (*end == '\0') ? 0 : -1;
}

const char *
app_shell_format_float_or_dash(char *buffer, size_t buffer_size, float value, uint8_t frac_digits)
{
    int32_t scaled;

    (void)frac_digits;
    TEST_ASSERT(buffer != NULL);
    TEST_ASSERT(buffer_size > 0U);
    /* Keep the test double deterministic and match the product formatter's
     * round-half-up rule instead of delegating a tie to host libc. */
    scaled = (int32_t)(((value < 0.0f) ? -value : value) * 100.0f + 0.5f);
    if (value < 0.0f) {
        (void)snprintf(buffer,
                       buffer_size,
                       "-%ld.%02ld",
                       (long)(scaled / 100),
                       (long)(scaled % 100));
    } else {
        (void)snprintf(buffer,
                       buffer_size,
                       "%ld.%02ld",
                       (long)(scaled / 100),
                       (long)(scaled % 100));
    }
    return buffer;
}

bool motor_runtime_request_set_passthrough(enum foc_ctrl_mode mode, float target, float limit)
{
    g_request_set_calls++;
    g_passthrough_calls++;
    g_last_set_mode = (int)mode;
    g_last_set_target = target;
    g_last_set_limit = limit;
    _motor_ins.ctrl_mode = mode;
    _motor_ins.input_mode = INPUT_MODE_PASSTHROUGH;

    if (mode == CTRL_MODE_VEL) {
        _motor_ins.ctrl_state.target_vel = target;
        _motor_ins.ctrl_state.dyn_iq_limit = limit;
    } else if (mode == CTRL_MODE_CURR) {
        _motor_ins.ctrl_state.target_iq = target;
    } else if (mode == CTRL_MODE_POS) {
        _motor_ins.ctrl_state.target_pos = target;
        _motor_ins.ctrl_state.dyn_vel_limit = limit;
    }

    return true;
}

bool motor_runtime_request_if_to_vel_handoff(float vel_rpm)
{
    (void)vel_rpm;
    return true;
}

bool motor_runtime_request_bench_velocity_step(float vel_rpm)
{
    g_velocity_step_calls++;
    g_last_velocity_step_rpm = vel_rpm;
    return g_velocity_step_result;
}

bool motor_runtime_request_vel_ramp(float new_vel, float max_acc)
{
    g_request_ramp_calls++;
    g_vel_ramp_calls++;
    g_last_ramp_kind = 0;
    g_last_ramp_value0 = new_vel;
    g_last_ramp_value1 = max_acc;
    _motor_ins.ctrl_mode = CTRL_MODE_VEL;
    _motor_ins.input_mode = INPUT_MODE_VEL_RAMP;
    _motor_ins.ctrl_state.target_vel = new_vel;
    _motor_ins.ctrl_config.ramp_accel = max_acc;
    return true;
}

bool motor_runtime_request_torque_ramp(float new_torque, float max_acc)
{
    g_request_ramp_calls++;
    g_torque_ramp_calls++;
    g_last_ramp_kind = 1;
    g_last_ramp_value0 = new_torque;
    g_last_ramp_value1 = max_acc;
    _motor_ins.ctrl_mode = CTRL_MODE_CURR;
    _motor_ins.input_mode = INPUT_MODE_TORQUE_RAMP;
    _motor_ins.ctrl_state.target_tor = new_torque;
    _motor_ins.ctrl_config.ramp_accel = max_acc;
    return true;
}

bool motor_runtime_request_pos_traj(
    float new_pos_rad,
    float max_vel,
    float max_acc,
    float max_decel
)
{
    g_request_ramp_calls++;
    g_pos_traj_calls++;
    g_last_ramp_kind = 2;
    g_last_ramp_value0 = new_pos_rad;
    g_last_ramp_value1 = max_vel;
    g_last_ramp_value2 = max_acc;
    g_last_ramp_value3 = max_decel;
    _motor_ins.ctrl_mode = CTRL_MODE_POS;
    _motor_ins.input_mode = INPUT_MODE_TRAP_TRAJ;
    _motor_ins.ctrl_state.target_pos = new_pos_rad;
    _motor_ins.ctrl_state.dyn_vel_limit = max_vel;
    return true;
}

bool motor_runtime_diag_set_param(const char *name, float value)
{
    TEST_ASSERT(name != NULL);
    g_request_param_calls++;

    if (strcmp(name, "rs") == 0) {
        g_last_param_field = 0;
        _motor_ins.hw_config.rs = value;
    } else if (strcmp(name, "ls") == 0) {
        g_last_param_field = 1;
        _motor_ins.hw_config.ls = value;
    } else if (strcmp(name, "flux") == 0) {
        g_last_param_field = 2;
        _motor_ins.hw_config.flux = value;
    } else if (strcmp(name, "poles") == 0) {
        g_last_param_field = 3;
        _motor_ins.hw_config.poles = (float)(int)value;
    } else if (strcmp(name, "js") == 0) {
        g_last_param_field = 4;
        _motor_ins.hw_config.js = value;
    } else if (strcmp(name, "gear") == 0) {
        g_last_param_field = 5;
        _motor_ins.hw_config.gear_ratio = value;
    } else if (strcmp(name, "vov") == 0) {
        g_last_param_field = 6;
        _motor_ins.prot_config.v_bus_ov_thr = value;
    } else if (strcmp(name, "vuv") == 0) {
        g_last_param_field = 7;
        _motor_ins.prot_config.v_bus_uv_thr = value;
    } else if (strcmp(name, "imax") == 0) {
        g_last_param_field = 8;
        _motor_ins.prot_config.i_peak = value;
    } else if (strcmp(name, "velmax") == 0) {
        g_last_param_field = 9;
        _motor_ins.prot_config.vel_over_thr = FOC_RPM_TO_RADS(value);
    } else if (strcmp(name, "currbw") == 0) {
        if (value <= 0.0f) {
            return false;
        }
        g_last_param_field = 10;
        _motor_ins.ctrl_config.curr_bandwidth = value;
    } else if (strcmp(name, "velbw") == 0) {
        if (value <= 0.0f) {
            return false;
        }
        g_last_param_field = 11;
        _motor_ins.ctrl_config.vel_bandwidth = value;
    } else if (strcmp(name, "posbw") == 0) {
        if (value <= 0.0f) {
            return false;
        }
        g_last_param_field = 12;
        _motor_ins.ctrl_config.pos_bandwidth = value;
    } else {
        return false;
    }

    g_last_param_value = value;
    return true;
}

bool motor_runtime_diag_set_pid(const char *loop_name, const char *field, float value)
{
    struct pid_dev *pid = NULL;

    TEST_ASSERT(loop_name != NULL);
    TEST_ASSERT(field != NULL);
    g_request_pid_calls++;

    if (strcmp(loop_name, "id") == 0) {
        pid = &_motor_ins.ctrl_config.id_pid;
        g_last_pid_loop = 0;
    } else if (strcmp(loop_name, "iq") == 0) {
        pid = &_motor_ins.ctrl_config.iq_pid;
        g_last_pid_loop = 1;
    } else if (strcmp(loop_name, "vel") == 0) {
        pid = &_motor_ins.ctrl_config.vel_pid;
        g_last_pid_loop = 2;
    } else if (strcmp(loop_name, "pos") == 0) {
        pid = &_motor_ins.ctrl_config.pos_pid;
        g_last_pid_loop = 3;
    } else {
        return false;
    }

    if (strcmp(field, "kp") == 0) {
        pid->config.kp = value;
        g_last_pid_field = 0;
    } else if (strcmp(field, "ki") == 0) {
        pid->config.ki = value;
        g_last_pid_field = 1;
    } else if (strcmp(field, "kd") == 0) {
        pid->config.kd = value;
        g_last_pid_field = 2;
    } else if (strcmp(field, "lim") == 0) {
        pid->config.max_output = value;
        pid->config.integral_limit = value;
        g_last_pid_field = 3;
    } else {
        return false;
    }

    g_last_pid_value = value;
    return true;
}

bool motor_runtime_diag_reset_pid(const char *loop_name)
{
    struct pid_dev *pid = NULL;

    TEST_ASSERT(loop_name != NULL);
    g_request_pid_calls++;

    if (strcmp(loop_name, "id") == 0) {
        pid = &_motor_ins.ctrl_config.id_pid;
        g_last_pid_loop = 0;
    } else if (strcmp(loop_name, "iq") == 0) {
        pid = &_motor_ins.ctrl_config.iq_pid;
        g_last_pid_loop = 1;
    } else if (strcmp(loop_name, "vel") == 0) {
        pid = &_motor_ins.ctrl_config.vel_pid;
        g_last_pid_loop = 2;
    } else if (strcmp(loop_name, "pos") == 0) {
        pid = &_motor_ins.ctrl_config.pos_pid;
        g_last_pid_loop = 3;
    } else {
        return false;
    }

    g_last_pid_field = 4;
    pid->config.kp = 0.0f;
    pid->config.ki = 0.0f;
    pid->config.kd = 0.0f;
    return true;
}

bool motor_runtime_diag_set_voltage(float vd, float vq, float we_ramp)
{
    g_request_voltage_calls++;
    g_last_voltage_vd = vd;
    g_last_voltage_vq = vq;
    g_last_voltage_we = we_ramp;
    _motor_ins.ctrl_mode = CTRL_MODE_VOLT;
    return true;
}

bool motor_runtime_diag_set_if(float iq_amp, float we)
{
    g_request_voltage_calls++;
    g_last_voltage_vd = iq_amp;
    g_last_voltage_vq = 0.0f;
    g_last_voltage_we = we;
    _motor_ins.ctrl_mode = CTRL_MODE_VOLT;
    return true;
}

bool motor_runtime_diag_set_voltloop(float vel_rpm, float vq, float *we_elec_out)
{
    g_request_voltage_calls++;
    g_last_voltage_vd = 0.0f;
    g_last_voltage_vq = vq;
    g_last_voltage_we = 0.0f;
    if (we_elec_out != NULL) {
        *we_elec_out = vel_rpm;
    }
    _motor_ins.ctrl_mode = CTRL_MODE_VOLT;
    return true;
}

bool motor_runtime_diag_set_phase_volt(int phase, float volt)
{
    (void)phase;
    g_request_voltage_calls++;
    g_last_voltage_vd = volt;
    g_last_voltage_vq = 0.0f;
    g_last_voltage_we = 0.0f;
    _motor_ins.ctrl_mode = CTRL_MODE_VOLT;
    return true;
}

bool motor_runtime_diag_read_params(struct motor_runtime_diag_param_view *view)
{
    TEST_ASSERT(view != NULL);
    view->poles = _motor_ins.hw_config.poles;
    view->rs = _motor_ins.hw_config.rs;
    view->ls = _motor_ins.hw_config.ls;
    view->flux = _motor_ins.hw_config.flux;
    view->js = _motor_ins.hw_config.js;
    view->gear_ratio = _motor_ins.hw_config.gear_ratio;
    view->v_bus_ov_thr = _motor_ins.prot_config.v_bus_ov_thr;
    view->v_bus_uv_thr = _motor_ins.prot_config.v_bus_uv_thr;
    view->i_peak = _motor_ins.prot_config.i_peak;
    view->vel_over_rpm = FOC_RADS_TO_RPM(_motor_ins.prot_config.vel_over_thr);
    view->curr_bandwidth = _motor_ins.ctrl_config.curr_bandwidth;
    view->vel_bandwidth = _motor_ins.ctrl_config.vel_bandwidth;
    view->pos_bandwidth = _motor_ins.ctrl_config.pos_bandwidth;
    return true;
}

bool motor_runtime_diag_read_pid(const char *loop_name, struct motor_runtime_pid_view *view)
{
    const struct pid_dev *pid = NULL;

    TEST_ASSERT(loop_name != NULL);
    TEST_ASSERT(view != NULL);

    if (strcmp(loop_name, "id") == 0) {
        pid = &_motor_ins.ctrl_config.id_pid;
    } else if (strcmp(loop_name, "iq") == 0) {
        pid = &_motor_ins.ctrl_config.iq_pid;
    } else if (strcmp(loop_name, "vel") == 0) {
        pid = &_motor_ins.ctrl_config.vel_pid;
    } else if (strcmp(loop_name, "pos") == 0) {
        pid = &_motor_ins.ctrl_config.pos_pid;
    } else if (strcmp(loop_name, "vvel") == 0) {
        pid = &_motor_ins.ctrl_config.vvel_pid;
    } else {
        return false;
    }

    view->kp = pid->config.kp;
    view->ki = pid->config.ki;
    view->kd = pid->config.kd;
    view->lim = pid->config.max_output;
    return true;
}

struct motor_dev *motor_runtime_current(void)
{
    return NULL;
}

void motor_runtime_attach(struct motor_dev *motor)
{
    (void)motor;
}

bool motor_runtime_snapshot(struct motor_runtime_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->axis_state = (uint8_t)_motor_ins.axis_state;
    return true;
}

bool motor_bench_trace_read(struct motor_bench_trace *out)
{
    TEST_ASSERT(out != NULL);
    *out = g_trace;
    return g_trace_valid;
}

void motor_bench_trace_clear(void)
{
    g_trace_clear_calls++;
    memset(&g_trace, 0, sizeof(g_trace));
    g_trace_valid = false;
    memset(g_trace_samples, 0, sizeof(g_trace_samples));
    g_trace_sample_count = 0U;
}

bool motor_bench_trace_sample_read(uint32_t index, struct motor_bench_sample *out)
{
    if ((out == NULL) || (index >= g_trace_sample_count)) {
        return false;
    }

    *out = g_trace_samples[index];
    return true;
}

uint32_t motor_bench_trace_sample_count(void)
{
    return g_trace_sample_count;
}

void motor_bench_trace_sample_clear(void)
{
    motor_bench_trace_clear();
}

bool motor_runtime_diag_set_phase_seed_offset(float offset_rad)
{
    g_phase_seed_offset_calls++;
    g_phase_seed_offset = offset_rad;
    return true;
}

bool motor_runtime_diag_set_encoder_dir(uint8_t dir)
{
    if ((_motor_ins.axis_state != AXIS_STATE_IDLE) ||
        ((dir != DIR_CW) && (dir != DIR_CCW))) {
        return false;
    }
    g_encoder_dir_set_calls++;
    g_encoder_dir = dir;
    return true;
}

bool motor_runtime_diag_set_pwm_map(uint8_t map_id)
{
    if ((_motor_ins.axis_state != AXIS_STATE_IDLE) || (map_id >= 6U)) {
        return false;
    }
    g_pwm_map_set_calls++;
    g_pwm_map = map_id;
    return true;
}

bool motor_runtime_diag_set_adc_map(uint8_t map_id)
{
    if ((_motor_ins.axis_state != AXIS_STATE_IDLE) || (map_id >= 6U)) {
        return false;
    }
    g_adc_map_set_calls++;
    g_adc_map = map_id;
    return true;
}

void motor_runtime_diag_reset_maps(void)
{
    if (_motor_ins.axis_state == AXIS_STATE_IDLE) {
        g_map_reset_calls++;
        g_pwm_map = 0U;
        g_adc_map = 0U;
        g_adc_trigger = MOTOR_BENCH_ADC_TRIGGER_DEFAULT;
    }
}

bool motor_runtime_diag_set_adc_trigger(uint16_t compare)
{
    if ((_motor_ins.axis_state != AXIS_STATE_IDLE) ||
        !motor_bench_adc_trigger_is_valid(compare)) {
        return false;
    }
    g_adc_trigger_set_calls++;
    g_adc_trigger = compare;
    return true;
}

bool motor_runtime_diag_read_bench_maps(uint8_t *pwm_map,
                                        uint8_t *adc_map,
                                        uint16_t *adc_trigger)
{
    TEST_ASSERT(pwm_map != NULL);
    TEST_ASSERT(adc_map != NULL);
    TEST_ASSERT(adc_trigger != NULL);
    *pwm_map = g_pwm_map;
    *adc_map = g_adc_map;
    *adc_trigger = g_adc_trigger;
    return true;
}

bool motor_runtime_diag_encoder_trace_clear(void)
{
    if (_motor_ins.axis_state != AXIS_STATE_IDLE) {
        return false;
    }
    g_encoder_trace_clear_calls++;
    g_encoder_trace_sample_count = 0U;
    return true;
}

bool motor_runtime_diag_encoder_trace_read(uint32_t index,
                                           struct encoder_bench_sample *out)
{
    if ((out == NULL) || (index >= g_encoder_trace_sample_count)) {
        return false;
    }
    *out = g_encoder_trace_samples[index];
    return true;
}

uint32_t motor_runtime_diag_encoder_trace_count(void)
{
    return g_encoder_trace_sample_count;
}

static void test_diag_encoder_trace_is_stopped_only_and_bounded(void)
{
    char *show[] = {"diag", "enc", "trace", "show"};
    char *clear[] = {"diag", "enc", "trace", "clear"};
    char *sample[] = {"diag", "enc", "trace", "sample", "0"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(app_shell_diag_command(4, show) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=enc_trace action=show result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=stopped_only") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    g_encoder_trace_sample_count = 1U;
    g_encoder_trace_samples[0].sequence = 8U;
    g_encoder_trace_samples[0].raw_word = 0x1234U;
    g_encoder_trace_samples[0].corrected_count = 321;
    g_encoder_trace_samples[0].angle_mech_rad = 0.25f;
    g_encoder_trace_samples[0].angle_elec_rad = 1.75f;
    g_encoder_trace_samples[0].velocity_rads = -2.0f;
    g_encoder_trace_samples[0].direction = -1;
    g_encoder_trace_samples[0].error_flags = ENCODER_BENCH_TRACE_ERROR_PLL;

    TEST_ASSERT(app_shell_diag_command(4, show) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=enc_trace action=show result=ok retained=1") != NULL);

    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(5, sample) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=enc_trace action=sample result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "raw=4660 corrected=321") != NULL);
    TEST_ASSERT(strstr(g_output, "direction=-1 error=0x02") != NULL);

    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(4, clear) == SHELL_OK);
    TEST_ASSERT(g_encoder_trace_clear_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=enc_trace action=clear result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);

    _motor_ins.axis_state = AXIS_STATE_IDLE;
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(4, clear) == SHELL_OK);
    TEST_ASSERT(g_encoder_trace_clear_calls == 1);
    TEST_ASSERT(strstr(g_output, "cmd=enc_trace action=clear result=ok") != NULL);
}

static void test_diag_bench_map_commands_are_bounded_and_stopped_only(void)
{
    char *set_pwm[] = {"diag", "map", "pwm", "4"};
    char *set_adc[] = {"diag", "map", "adc", "5"};
    char *show[] = {"diag", "map", "show"};
    char *reset[] = {"diag", "map", "reset"};
    char *bad[] = {"diag", "map", "pwm", "6"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(app_shell_diag_command(4, set_pwm) == SHELL_OK);
    TEST_ASSERT(g_pwm_map_set_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=map action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    TEST_ASSERT(app_shell_diag_command(4, set_pwm) == SHELL_OK);
    TEST_ASSERT(g_pwm_map_set_calls == 1);
    TEST_ASSERT(g_pwm_map == 4U);
    TEST_ASSERT(strstr(g_output, "kind=pwm map=4") != NULL);

    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(4, set_adc) == SHELL_OK);
    TEST_ASSERT(g_adc_map_set_calls == 1);
    TEST_ASSERT(g_adc_map == 5U);
    TEST_ASSERT(strstr(g_output, "kind=adc map=5") != NULL);

    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(3, show) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=map action=show result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "pwm=4 adc=5 adc_trig=3740") != NULL);

    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(4, bad) == SHELL_OK);
    TEST_ASSERT(g_pwm_map_set_calls == 1);
    TEST_ASSERT(strstr(g_output, "reason=invalid_value") != NULL);

    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(3, show) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "reason=stopped_only") != NULL);

    _motor_ins.axis_state = AXIS_STATE_IDLE;
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(3, reset) == SHELL_OK);
    TEST_ASSERT(g_map_reset_calls == 1);
    TEST_ASSERT(g_pwm_map == 0U);
    TEST_ASSERT(g_adc_map == 0U);
    TEST_ASSERT(strstr(g_output, "cmd=map action=reset result=ok") != NULL);
}

static void test_diag_adc_trigger_command_is_bounded_and_idle_only(void)
{
    char *valid[] = {"diag", "set", "adc_trig", "512"};
    char *invalid_low[] = {"diag", "set", "adc_trig", "255"};
    char *invalid_high[] = {"diag", "set", "adc_trig", "3741"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    TEST_ASSERT(app_shell_diag_command(4, valid) == SHELL_OK);
    TEST_ASSERT(g_adc_trigger_set_calls == 1);
    TEST_ASSERT(g_adc_trigger == 512U);
    TEST_ASSERT(strstr(g_output, "field=adc_trig") != NULL);
    TEST_ASSERT(strstr(g_output, "value=512") != NULL);

    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(4, invalid_low) == SHELL_OK);
    TEST_ASSERT(g_adc_trigger_set_calls == 1);
    TEST_ASSERT(strstr(g_output, "reason=invalid_value") != NULL);

    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(4, invalid_high) == SHELL_OK);
    TEST_ASSERT(g_adc_trigger_set_calls == 1);
    TEST_ASSERT(strstr(g_output, "reason=invalid_value") != NULL);

    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(4, valid) == SHELL_OK);
    TEST_ASSERT(g_adc_trigger_set_calls == 1);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
}

static void test_diag_set_vel_is_rejected_while_running(void)
{
    char *argv[] = {"diag", "set", "vel", "50"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    _motor_ins.ctrl_mode = CTRL_MODE_CURR;
    _motor_ins.input_mode = INPUT_MODE_INACTIVE;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_passthrough_calls == 0);
    TEST_ASSERT(_motor_ins.ctrl_mode == CTRL_MODE_CURR);
    TEST_ASSERT(_motor_ins.input_mode == INPUT_MODE_INACTIVE);
    TEST_ASSERT(strstr(g_output, "cmd=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=running") != NULL);
}

static void test_diag_set_iq_is_rejected_while_running(void)
{
    char *argv[] = {"diag", "set", "iq", "0.100"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_passthrough_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
}

static void test_diag_set_phase_is_rejected_while_running(void)
{
    char *argv[] = {"diag", "set", "dp0", "0.100"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_voltage_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
}

static void test_diag_pid_write_is_rejected_while_running(void)
{
    char *argv[] = {"diag", "pid", "vel", "kp", "0.1000"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    _motor_ins.ctrl_config.vel_pid.config.kp = 1.0f;

    TEST_ASSERT(app_shell_diag_command(5, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.ctrl_config.vel_pid.config.kp == 1.0f);
    TEST_ASSERT(g_request_pid_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=running") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=vel") != NULL);
    TEST_ASSERT(strstr(g_output, "field=kp") != NULL);
}

static void test_diag_param_write_is_rejected_while_calib(void)
{
    char *argv[] = {"diag", "param", "rs", "0.2300"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_CALIB;
    _motor_ins.hw_config.rs = 0.5f;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.hw_config.rs == 0.5f);
    TEST_ASSERT(g_request_param_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=param action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=calib") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
    TEST_ASSERT(strstr(g_output, "name=rs") != NULL);
}

static void test_diag_pid_show_remains_available_while_running(void)
{
    char *argv[] = {"diag", "pid", "all", "show"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=show result=ok loop=all") != NULL);
}

static void test_diag_param_write_uses_runtime_request_in_idle(void)
{
    char *argv[] = {"diag", "param", "currbw", "2500"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_config.curr_bandwidth = 1000.0f;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_param_calls == 1);
    TEST_ASSERT(g_last_param_field == 10);
    TEST_ASSERT(_motor_ins.ctrl_config.curr_bandwidth == 2500.0f);
    TEST_ASSERT(strstr(g_output, "cmd=param action=set result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "name=currbw") != NULL);
    TEST_ASSERT(strstr(g_output, "value=2500.00 unit=raw") != NULL);
}

static void test_diag_pid_write_uses_runtime_request_in_idle(void)
{
    char *argv[] = {"diag", "pid", "vel", "kp", "0.1250"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(5, argv) == SHELL_OK);
    TEST_ASSERT(g_request_pid_calls == 1);
    TEST_ASSERT(g_last_pid_loop == 2);
    TEST_ASSERT(g_last_pid_field == 0);
    TEST_ASSERT(_motor_ins.ctrl_config.vel_pid.config.kp == 0.1250f);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=set result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=vel") != NULL);
    TEST_ASSERT(strstr(g_output, "field=kp") != NULL);
    TEST_ASSERT(strstr(g_output, "value=0.13 unit=raw") != NULL);
}

static void test_diag_pid_reset_uses_runtime_request_in_idle(void)
{
    char *argv[] = {"diag", "pid", "vel", "reset"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_config.vel_pid.config.kp = 2.0f;
    _motor_ins.ctrl_config.vel_pid.config.ki = 3.0f;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_pid_calls == 1);
    TEST_ASSERT(g_last_pid_loop == 2);
    TEST_ASSERT(g_last_pid_field == 4);
    TEST_ASSERT(_motor_ins.ctrl_config.vel_pid.config.kp == 0.0f);
    TEST_ASSERT(_motor_ins.ctrl_config.vel_pid.config.ki == 0.0f);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=reset result=ok loop=vel") != NULL);
}

static void test_diag_pid_show_bad_loop_reports_reason(void)
{
    char *argv[] = {"diag", "pid", "flux", "show"};

    test_env_reset();

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=show result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_loop") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=flux") != NULL);
}

static void test_diag_pid_reset_bad_loop_reports_reason(void)
{
    char *argv[] = {"diag", "pid", "flux", "reset"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_pid_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=reset result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_loop") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=flux") != NULL);
}

static void test_diag_pid_set_bad_field_reports_reason(void)
{
    char *argv[] = {"diag", "pid", "vel", "foo", "0.5000"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(5, argv) == SHELL_OK);
    TEST_ASSERT(g_request_pid_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_field") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=vel") != NULL);
    TEST_ASSERT(strstr(g_output, "field=foo") != NULL);
}

static void test_diag_param_bad_name_reports_reason(void)
{
    char *argv[] = {"diag", "param", "foo", "1.0000"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_param_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=param action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_name") != NULL);
    TEST_ASSERT(strstr(g_output, "name=foo") != NULL);
}

static void test_diag_param_bad_value_reports_reason(void)
{
    char *argv[] = {"diag", "param", "currbw", "0.0000"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_param_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=param action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_value") != NULL);
    TEST_ASSERT(strstr(g_output, "name=currbw") != NULL);
    TEST_ASSERT(strstr(g_output, "value=0.00 unit=raw") != NULL);
}

static void test_diag_set_vd_uses_runtime_voltage_request(void)
{
    char *argv[] = {"diag", "set", "vd", "1.250"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_mode = CTRL_MODE_CURR;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_voltage_calls == 1);
    TEST_ASSERT(g_last_voltage_vd == 1.250f);
    TEST_ASSERT(g_last_voltage_vq == 0.0f);
    TEST_ASSERT(g_last_voltage_we == 0.0f);
    TEST_ASSERT(_motor_ins.ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(strstr(g_output, "cmd=set result=ok vd=1.25 unit=V") != NULL);
}

static void test_diag_set_vd_is_rejected_while_running(void)
{
    char *argv[] = {"diag", "set", "vd", "1.250"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_voltage_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
}

static void test_diag_set_if_is_rejected_while_running(void)
{
    char *argv[] = {"diag", "set", "if", "0.100"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_voltage_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
}

static void test_diag_trace_show_is_rejected_while_running(void)
{
    char *argv[] = {"diag", "trace", "show"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_diag_command(3, argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=trace action=show result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=stopped_only") != NULL);
}

static void test_diag_trace_clear_is_idle_only(void)
{
    char *argv[] = {"diag", "trace", "clear"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_diag_command(3, argv) == SHELL_OK);
    TEST_ASSERT(g_trace_clear_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=trace action=clear result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    TEST_ASSERT(app_shell_diag_command(3, argv) == SHELL_OK);
    TEST_ASSERT(g_trace_clear_calls == 1);
    TEST_ASSERT(strstr(g_output, "cmd=trace action=clear result=ok") != NULL);
}

static void test_diag_trace_show_is_available_after_stop(void)
{
    char *argv[] = {"diag", "trace", "show"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    g_trace.sequence = 7U;
    g_trace.sample_count = 12U;
    g_trace.valid = 1U;
    g_trace.first_sector = 2U;
    g_trace.last_sector = 5U;
    g_trace.ia_peak_abs = 0.04f;
    g_trace.ib_peak_abs = 0.05f;
    g_trace.ic_peak_abs = 0.06f;
    g_trace.v_bus_min = 11.8f;
    g_trace.v_bus_max = 12.1f;
    g_trace_valid = true;

    TEST_ASSERT(app_shell_diag_command(3, argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=trace action=show result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "sequence=7 samples=12") != NULL);
    TEST_ASSERT(strstr(g_output, "sector=2/5") != NULL);
}

static void test_diag_theta_seed_offset_is_available_only_in_idle(void)
{
    char *argv[] = {"diag", "set", "theta0", "1.5708"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_phase_seed_offset_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
    TEST_ASSERT(strstr(g_output, "field=theta0") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_phase_seed_offset_calls == 1);
    TEST_ASSERT(fabsf(g_phase_seed_offset - 1.5708f) < 0.0001f);
    TEST_ASSERT(strstr(g_output, "cmd=set result=ok field=theta0 unit=rad") != NULL);
}

static void test_diag_encoder_direction_is_bench_idle_only(void)
{
    char *cw[] = {"diag", "set", "enc_dir", "0"};
    char *ccw[] = {"diag", "set", "enc_dir", "1"};
    char *invalid[] = {"diag", "set", "enc_dir", "2"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    TEST_ASSERT(app_shell_diag_command(4, cw) == SHELL_OK);
    TEST_ASSERT(g_encoder_dir_set_calls == 1);
    TEST_ASSERT(g_encoder_dir == DIR_CW);
    TEST_ASSERT(strstr(g_output, "cmd=set result=ok field=enc_dir value=0 direction=cw") != NULL);

    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(4, invalid) == SHELL_OK);
    TEST_ASSERT(g_encoder_dir_set_calls == 1);
    TEST_ASSERT(strstr(g_output, "cmd=set result=error reason=invalid_value field=enc_dir") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(app_shell_diag_command(4, ccw) == SHELL_OK);
    TEST_ASSERT(g_encoder_dir_set_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=set result=error reason=idle_only") != NULL);
    TEST_ASSERT(strstr(g_output, "field=enc_dir") != NULL);
}

bool motor_runtime_diag_set_encoder_abs(uint8_t dir, float mech_offset)
{
    if ((_motor_ins.axis_state != AXIS_STATE_IDLE) ||
        ((dir != DIR_CW) && (dir != DIR_CCW)) || !isfinite(mech_offset) ||
        (mech_offset < -FOC_PI) || (mech_offset > FOC_PI)) {
        return false;
    }
    g_encoder_abs_set_calls++;
    g_encoder_dir = dir;
    g_encoder_abs_offset = mech_offset;
    return true;
}

bool motor_runtime_diag_read_adc(struct motor_runtime_adc_view *view)
{
    TEST_ASSERT(view != NULL);
    g_adc_show_calls++;
    memset(view, 0, sizeof(*view));
    view->ia_raw = 2050.0f;
    view->ib_raw = 2051.0f;
    view->ic_raw = 2052.0f;
    view->ia_offset = 2050.0f;
    view->ib_offset = 2051.0f;
    view->ic_offset = 2052.0f;
    view->sample_seq = 1234U;
    view->sample_count = 2000U;
    view->fresh_count = 2000U;
    view->pwm_map = 0U;
    view->adc_map = 0U;
    view->adc_trigger = MOTOR_BENCH_ADC_TRIGGER_DEFAULT;
    view->calib_valid = true;
    view->clamp_used = true;
    return true;
}

bool motor_runtime_diag_zero_current_offsets(void)
{
    g_adc_zero_calls++;
    return g_adc_zero_result;
}

bool motor_runtime_diag_bench_keepalive(void)
{
    g_bench_keepalive_calls++;
    return g_bench_keepalive_result;
}

bool motor_runtime_diag_bench_status(struct motor_runtime_bench_status *out)
{
    TEST_ASSERT(out != NULL);
    *out = g_bench_status;
    return true;
}

static void test_diag_bench_keepalive_and_status_are_machine_readable(void)
{
    char *keepalive[] = {"diag", "bench", "keepalive"};
    char *status[] = {"diag", "bench", "status"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(app_shell_diag_command(3, keepalive) == SHELL_OK);
    TEST_ASSERT(g_bench_keepalive_calls == 1);
    TEST_ASSERT(strstr(g_output, "cmd=bench action=keepalive result=ok") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    g_bench_keepalive_result = false;
    TEST_ASSERT(app_shell_diag_command(3, keepalive) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=bench action=keepalive result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=running_only") != NULL);

    test_env_reset();
    g_bench_status.armed = true;
    g_bench_status.heartbeat_age_ms = 200U;
    g_bench_status.timeout_count = 1U;
    g_bench_status.expired = true;
    TEST_ASSERT(app_shell_diag_command(3, status) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=bench action=status result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "age_ms=200") != NULL);
    TEST_ASSERT(strstr(g_output, "timeout_count=1") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=heartbeat_timeout") != NULL);
}

static void test_diag_velocity_step_uses_bench_runtime_and_reports_precondition(void)
{
    char *argv[] = {"diag", "velstep", "60.0"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(app_shell_diag_command(3, argv) == SHELL_OK);
    TEST_ASSERT(g_velocity_step_calls == 1);
    TEST_ASSERT(g_last_velocity_step_rpm == 60.0f);
    TEST_ASSERT(strstr(g_output, "cmd=velstep result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=accepted") != NULL);
    TEST_ASSERT(strstr(g_output, "vel_rpm=60.00 unit=rpm") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    g_velocity_step_result = false;
    TEST_ASSERT(app_shell_diag_command(3, argv) == SHELL_OK);
    TEST_ASSERT(g_velocity_step_calls == 1);
    TEST_ASSERT(strstr(g_output, "cmd=velstep result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=precondition") != NULL);
    TEST_ASSERT(strstr(g_output, "vel_rpm=60.00 unit=rpm") != NULL);
}

static void test_diag_encoder_setabs_is_idle_only_and_absolute(void)
{
    char *argv[] = {"diag", "enc", "setabs", "1", "0.0000"};

    test_env_reset();
    TEST_ASSERT(app_shell_diag_command(5, argv) == SHELL_OK);
    TEST_ASSERT(g_encoder_abs_set_calls == 1);
    TEST_ASSERT(g_encoder_dir == DIR_CCW);
    TEST_ASSERT(fabsf(g_encoder_abs_offset) < 0.0001f);
    TEST_ASSERT(strstr(g_output, "cmd=enc action=setabs result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "mech_offset=0.00") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(app_shell_diag_command(5, argv) == SHELL_OK);
    TEST_ASSERT(g_encoder_abs_set_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=enc action=setabs result=error reason=idle_only") != NULL);
}

static void test_diag_adc_show_and_zero_are_bench_safe(void)
{
    char *show[] = {"diag", "adc", "show"};
    char *zero[] = {"diag", "adc", "zero"};

    test_env_reset();
    TEST_ASSERT(app_shell_diag_command(3, show) == SHELL_OK);
    TEST_ASSERT(g_adc_show_calls == 1);
    TEST_ASSERT(strstr(g_output, "cmd=adc action=show result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "fresh=2000") != NULL);
    TEST_ASSERT(strstr(g_output, "adc_trig=3740") != NULL);

    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    TEST_ASSERT(app_shell_diag_command(3, zero) == SHELL_OK);
    TEST_ASSERT(g_adc_zero_calls == 1);
    TEST_ASSERT(strstr(g_output, "cmd=adc action=zero result=ok") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(app_shell_diag_command(3, zero) == SHELL_OK);
    TEST_ASSERT(g_adc_zero_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=adc action=zero result=error reason=idle_only") != NULL);

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_FAULT;
    _motor_ins.fault_code = FAULT_OVER_CURR;
    TEST_ASSERT(app_shell_diag_command(3, zero) == SHELL_OK);
    TEST_ASSERT(g_adc_zero_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=adc action=zero result=error reason=idle_only") != NULL);
}

int main(void)
{
    test_diag_set_vel_is_rejected_while_running();
    test_diag_set_iq_is_rejected_while_running();
    test_diag_set_phase_is_rejected_while_running();
    test_diag_pid_write_is_rejected_while_running();
    test_diag_param_write_is_rejected_while_calib();
    test_diag_pid_show_remains_available_while_running();
    test_diag_param_write_uses_runtime_request_in_idle();
    test_diag_pid_write_uses_runtime_request_in_idle();
    test_diag_pid_reset_uses_runtime_request_in_idle();
    test_diag_pid_show_bad_loop_reports_reason();
    test_diag_pid_reset_bad_loop_reports_reason();
    test_diag_pid_set_bad_field_reports_reason();
    test_diag_param_bad_name_reports_reason();
    test_diag_param_bad_value_reports_reason();
    test_diag_set_vd_uses_runtime_voltage_request();
    test_diag_set_vd_is_rejected_while_running();
    test_diag_set_if_is_rejected_while_running();
    test_diag_trace_show_is_rejected_while_running();
    test_diag_trace_clear_is_idle_only();
    test_diag_trace_show_is_available_after_stop();
    test_diag_theta_seed_offset_is_available_only_in_idle();
    test_diag_encoder_direction_is_bench_idle_only();
    test_diag_velocity_step_uses_bench_runtime_and_reports_precondition();
    test_diag_encoder_setabs_is_idle_only_and_absolute();
    test_diag_adc_show_and_zero_are_bench_safe();
    test_diag_bench_keepalive_and_status_are_machine_readable();
    test_diag_bench_map_commands_are_bounded_and_stopped_only();
    test_diag_adc_trigger_command_is_bounded_and_idle_only();
    test_diag_encoder_trace_is_stopped_only_and_bounded();

    puts("[test_app_shell_diag_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
