/**
 * @file    test_motor_bench_trace_contract.c
 * @brief   Host contract tests for bench-only RAM motor trace samples
 * @version v2.0.0
 * @date    2026.08.29
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell_commands.h"
#include "config/tm_core_config.h"
#include "foc_common.h"
#include "foc_config.h"
#include "motor_fault_runtime.h"
#include "motor_runtime.h"
#include "tm_shell.h"

#define TEST_ASSERT(expr)                                                               \
    do {                                                                                \
        if (!(expr)) {                                                                  \
            fprintf(stderr,                                                             \
                    "[test_motor_bench_trace_contract] assertion failed: %s (%s:%d)\n", \
                    #expr,                                                              \
                    __FILE__,                                                           \
                    __LINE__);                                                          \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

static char                       g_output[4096];
static size_t                     g_output_len;
static uint8_t                    g_snapshot_axis_state;
static uint32_t                   g_pwm_disable_calls;
static bool                       g_context_visible_before_pwm_disable;
static struct motor_fault_context g_context_at_pwm_disable;

static void test_capture_hw(uint32_t index)
{
    struct motor_hw_snapshot hw;

    memset(&hw, 0, sizeof(hw));
    hw.tmr1_ists      = 0x1000U + index;
    hw.tmr1_cval      = 0x2000U + index;
    hw.tmr1_pr        = 0x3000U + index;
    hw.tmr1_ccr1      = 0x4000U + index;
    hw.tmr1_ccr2      = 0x5000U + index;
    hw.tmr1_ccr3      = 0x6000U + index;
    hw.tmr1_ccr4      = 0x7000U + index;
    hw.tmr1_cctrl     = 0x8000U + index;
    hw.tmr1_brk       = 0x9000U + index;
    hw.tmr1_count_dir = (uint8_t)(index & 0x1U);
    hw.adc_sts        = 0xA000U + index;
    motor_fault_runtime_stage_hw_snapshot(&hw);
}

static void test_motor_init(struct motor_dev *motor)
{
    memset(motor, 0, sizeof(*motor));
    motor->axis_state                 = AXIS_STATE_RUNNING;
    motor->ctrl_mode                  = CTRL_MODE_VOLT;
    motor->prot_config.v_bus_ov_thr   = DEFAULT_V_BUS_OV;
    motor->prot_config.v_bus_uv_thr   = DEFAULT_V_BUS_UV;
    motor->prot_config.vel_over_thr   = DEFAULT_VEL_OVER;
    motor->prot_config.motor_temp_thr = DEFAULT_MOTOR_TEMP;
    motor->prot_config.driver_temp_thr = DEFAULT_DRIVER_TEMP;
    motor->prot_config.i_peak         = DEFAULT_CURR_PEAK;
    motor->meas_data.v_bus            = DEFAULT_VOLT_RATED;
}

static void test_env_reset(void)
{
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    g_snapshot_axis_state = AXIS_STATE_IDLE;
    g_pwm_disable_calls = 0U;
    g_context_visible_before_pwm_disable = false;
    memset(&g_context_at_pwm_disable, 0, sizeof(g_context_at_pwm_disable));
    motor_bench_trace_sample_clear();
    motor_bench_trace_clear();
    motor_fault_context_clear();
}

void shell_printf(const char *fmt, ...)
{
    va_list args;
    int     written;

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

const char *app_shell_format_float_or_dash(char *buffer, size_t buffer_size, float value, uint8_t frac_digits)
{
    char format[8];

    TEST_ASSERT(buffer != NULL);
    TEST_ASSERT(buffer_size > 0U);
    if (!isfinite(value)) {
        (void)snprintf(buffer, buffer_size, "-");
        return buffer;
    }
    (void)snprintf(format, sizeof(format), "%%.%uf", (unsigned)frac_digits);
    (void)snprintf(buffer, buffer_size, format, value);
    return buffer;
}

bool motor_runtime_snapshot(struct motor_runtime_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->axis_state = g_snapshot_axis_state;
    return true;
}

bool motor_runtime_request_set_passthrough(enum foc_ctrl_mode mode, float target, float limit)
{
    (void)mode;
    (void)target;
    (void)limit;
    return false;
}

bool motor_runtime_request_vel_ramp(float target_rads, float max_acc_rads)
{
    (void)target_rads;
    (void)max_acc_rads;
    return false;
}

bool motor_runtime_request_if_to_vel_handoff(float vel_rpm)
{
    (void)vel_rpm;
    return true;
}

bool motor_runtime_request_bench_velocity_step(float vel_rpm)
{
    (void)vel_rpm;
    return false;
}

bool motor_runtime_request_torque_ramp(float target_torque, float max_acc)
{
    (void)target_torque;
    (void)max_acc;
    return false;
}

bool motor_runtime_request_pos_traj(float target_pos_rad,
                                    float max_vel_rads,
                                    float max_acc_rads,
                                    float max_decel_rads)
{
    (void)target_pos_rad;
    (void)max_vel_rads;
    (void)max_acc_rads;
    (void)max_decel_rads;
    return false;
}

bool motor_runtime_diag_set_param(const char *name, float value)
{
    (void)name;
    (void)value;
    return false;
}

bool motor_runtime_diag_set_pid(const char *loop_name, const char *field, float value)
{
    (void)loop_name;
    (void)field;
    (void)value;
    return false;
}

bool motor_runtime_diag_reset_pid(const char *loop_name)
{
    (void)loop_name;
    return false;
}

bool motor_runtime_diag_set_voltage(float vd, float vq, float we_ramp)
{
    (void)vd;
    (void)vq;
    (void)we_ramp;
    return false;
}

bool motor_runtime_diag_set_if(float iq_amp, float we)
{
    (void)iq_amp;
    (void)we;
    return false;
}

bool motor_runtime_diag_set_voltloop(float vel_rpm, float vq, float *we_elec_out)
{
    (void)vel_rpm;
    (void)vq;
    (void)we_elec_out;
    return false;
}

bool motor_runtime_diag_set_phase_volt(int phase, float volt)
{
    (void)phase;
    (void)volt;
    return false;
}

bool motor_runtime_diag_set_phase_seed_offset(float offset_rad)
{
    (void)offset_rad;
    return false;
}

bool motor_runtime_diag_set_pwm_map(uint8_t map_id)
{
    (void)map_id;
    return false;
}

bool motor_runtime_diag_set_adc_map(uint8_t map_id)
{
    (void)map_id;
    return false;
}

void motor_runtime_diag_reset_maps(void)
{
}

bool motor_runtime_diag_set_adc_trigger(uint16_t compare)
{
    (void)compare;
    return false;
}

bool motor_runtime_diag_set_encoder_dir(uint8_t dir)
{
    (void)dir;
    return false;
}

bool motor_runtime_diag_set_encoder_abs(uint8_t dir, float mech_offset)
{
    (void)dir;
    (void)mech_offset;
    return false;
}

bool motor_runtime_diag_read_adc(struct motor_runtime_adc_view *view)
{
    (void)view;
    return false;
}

bool motor_runtime_diag_zero_current_offsets(void)
{
    return false;
}

bool motor_runtime_diag_bench_keepalive(void)
{
    return false;
}

bool motor_runtime_diag_bench_status(struct motor_runtime_bench_status *out)
{
    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    return true;
}

bool motor_runtime_diag_read_bench_maps(uint8_t *pwm_map,
                                        uint8_t *adc_map,
                                        uint16_t *adc_trigger)
{
    (void)pwm_map;
    (void)adc_map;
    (void)adc_trigger;
    return false;
}

bool motor_runtime_diag_encoder_trace_clear(void)
{
    return false;
}

bool motor_runtime_diag_encoder_trace_read(uint32_t index,
                                           struct encoder_bench_sample *out)
{
    (void)index;
    (void)out;
    return false;
}

uint32_t motor_runtime_diag_encoder_trace_count(void)
{
    return 0U;
}

bool motor_runtime_diag_read_params(struct motor_runtime_diag_param_view *view)
{
    if (view == NULL) {
        return false;
    }

    memset(view, 0, sizeof(*view));
    return false;
}

bool motor_runtime_diag_read_pid(const char *loop_name, struct motor_runtime_pid_view *view)
{
    (void)loop_name;
    if (view == NULL) {
        return false;
    }

    memset(view, 0, sizeof(*view));
    return false;
}

void pwm_out_disable(void)
{
    g_pwm_disable_calls++;
    g_context_visible_before_pwm_disable = motor_fault_context_read(&g_context_at_pwm_disable);
}

static void test_trace_captures_first_last_ranges_peaks_and_samples(void)
{
    struct motor_dev         motor;
    struct motor_bench_trace trace;
    struct motor_bench_sample sample0;
    struct motor_bench_sample sample1;

    test_env_reset();
    test_motor_init(&motor);
    TEST_ASSERT(motor_bench_trace_read(&trace) == false);
    TEST_ASSERT(motor_bench_trace_sample_count() == 0U);

    motor.meas_data.v_bus_raw = 1200.0f;
    motor.meas_data.ia_raw = 100.0f;
    motor.meas_data.ib_raw = 200.0f;
    motor.meas_data.ic_raw = 300.0f;
    motor.meas_data.a = 0.10f;
    motor.meas_data.b = -0.20f;
    motor.meas_data.c = 0.30f;
    motor.meas_data.v_bus = 11.8f;
    motor.ctrl_state.theta_sim = 0.25f;
    motor.enc_pri.state.angle_elec_rad = 0.50f;
    motor.ctrl_state.we_volt_now = 0.10f;
    motor.ctrl_state.we_volt_ramp = 1.00f;
    motor.ctrl_state.if_enable = 1U;
    motor.foc_calc.v_d = 0.01f;
    motor.foc_calc.v_q = 0.02f;
    motor.foc_calc.dtc_a = 0.40f;
    motor.foc_calc.dtc_b = 0.50f;
    motor.foc_calc.dtc_c = 0.60f;
    motor.foc_calc.sector = 2U;
    test_capture_hw(1U);
    motor_bench_trace_update(&motor);

    motor.meas_data.v_bus_raw = 1210.0f;
    motor.meas_data.ia_raw = 90.0f;
    motor.meas_data.ib_raw = 220.0f;
    motor.meas_data.ic_raw = 250.0f;
    motor.meas_data.a = -0.40f;
    motor.meas_data.b = 0.50f;
    motor.meas_data.c = -0.60f;
    motor.meas_data.v_bus = 12.2f;
    motor.ctrl_state.theta_sim = 0.75f;
    motor.enc_pri.state.angle_elec_rad = 0.90f;
    motor.ctrl_state.we_volt_now = 0.30f;
    motor.ctrl_state.we_volt_ramp = 1.50f;
    motor.ctrl_state.if_enable = 0U;
    motor.foc_calc.v_d = -0.03f;
    motor.foc_calc.v_q = 0.04f;
    motor.foc_calc.dtc_a = 0.35f;
    motor.foc_calc.dtc_b = 0.55f;
    motor.foc_calc.dtc_c = 0.65f;
    motor.foc_calc.sector = 5U;
    test_capture_hw(2U);
    motor_bench_trace_update(&motor);

    TEST_ASSERT(motor_bench_trace_read(&trace) == true);
    TEST_ASSERT(trace.valid == 1U);
    TEST_ASSERT(trace.sample_count == 2U);
    TEST_ASSERT(trace.first_sector == 2U);
    TEST_ASSERT(trace.last_sector == 5U);
    TEST_ASSERT(trace.ia_raw_min == 90U);
    TEST_ASSERT(trace.ia_raw_max == 100U);
    TEST_ASSERT(trace.ib_raw_min == 200U);
    TEST_ASSERT(trace.ib_raw_max == 220U);
    TEST_ASSERT(trace.ic_raw_min == 250U);
    TEST_ASSERT(trace.ic_raw_max == 300U);
    TEST_ASSERT(fabsf(trace.ia_peak_abs - 0.40f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.ib_peak_abs - 0.50f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.ic_peak_abs - 0.60f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.ia_first - 0.10f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.ia_last + 0.40f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.ib_first + 0.20f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.ib_last - 0.50f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.ic_first - 0.30f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.ic_last + 0.60f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.v_bus_min - 11.8f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.v_bus_max - 12.2f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.theta_sim_first - 0.25f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.theta_sim_last - 0.75f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.theta_elec_first - 0.50f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.theta_elec_last - 0.90f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.we_volt_now_first - 0.10f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.we_volt_now_last - 0.30f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.we_volt_ramp_first - 1.00f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.we_volt_ramp_last - 1.50f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.vd_first - 0.01f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.vd_last + 0.03f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.vq_first - 0.02f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.vq_last - 0.04f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.dtc_a_min - 0.35f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.dtc_a_max - 0.40f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.dtc_b_min - 0.50f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.dtc_b_max - 0.55f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.dtc_c_min - 0.60f) < 0.0001f);
    TEST_ASSERT(fabsf(trace.dtc_c_max - 0.65f) < 0.0001f);

    TEST_ASSERT(motor_bench_trace_sample_count() == 2U);
    TEST_ASSERT(motor_bench_trace_sample_read(0U, &sample0) == true);
    TEST_ASSERT(motor_bench_trace_sample_read(1U, &sample1) == true);

    TEST_ASSERT(sample0.sequence == 1U);
    TEST_ASSERT(sample0.tmr1_ists == 0x1001U);
    TEST_ASSERT(sample0.tmr1_cval == 0x2001U);
    TEST_ASSERT(sample0.tmr1_pr == 0x3001U);
    TEST_ASSERT(sample0.tmr1_ccr1 == 0x4001U);
    TEST_ASSERT(sample0.tmr1_ccr2 == 0x5001U);
    TEST_ASSERT(sample0.tmr1_ccr3 == 0x6001U);
    TEST_ASSERT(sample0.tmr1_ccr4 == 0x7001U);
    TEST_ASSERT(sample0.tmr1_cctrl == 0x8001U);
    TEST_ASSERT(sample0.tmr1_brk == 0x9001U);
    TEST_ASSERT(sample0.tmr1_count_dir == 1U);
    TEST_ASSERT(sample0.adc_sts == 0xA001U);
    TEST_ASSERT(sample0.vbus_raw == 1200U);
    TEST_ASSERT(sample0.ic_raw == 300U);
    TEST_ASSERT(sample0.ib_raw == 200U);
    TEST_ASSERT(sample0.ia_raw == 100U);
    TEST_ASSERT(sample0.vbus_mv == 11800);
    TEST_ASSERT(sample0.ia_ma == 100);
    TEST_ASSERT(sample0.ib_ma == -200);
    TEST_ASSERT(sample0.ic_ma == 300);
    TEST_ASSERT(sample0.theta_sim_mrad == 250);
    TEST_ASSERT(sample0.theta_elec_mrad == 500);
    TEST_ASSERT(sample0.we_now_mrad_s == 100);
    TEST_ASSERT(sample0.we_ramp_mrad_s == 1000);
    TEST_ASSERT(sample0.vd_mv == 10);
    TEST_ASSERT(sample0.vq_mv == 20);
    TEST_ASSERT(sample0.duty_a_permille == 400U);
    TEST_ASSERT(sample0.duty_b_permille == 500U);
    TEST_ASSERT(sample0.duty_c_permille == 600U);
    TEST_ASSERT(sample0.sector == 2U);
    TEST_ASSERT(sample0.axis_state == (uint8_t)AXIS_STATE_RUNNING);
    TEST_ASSERT((sample0.flags & MOTOR_BENCH_SAMPLE_FLAG_IF_ENABLE) != 0U);
    TEST_ASSERT(sample0.fault_code == FAULT_NONE);

    TEST_ASSERT(sample1.sequence == 2U);
    TEST_ASSERT(sample1.tmr1_cval == 0x2002U);
    TEST_ASSERT(sample1.vbus_raw == 1210U);
    TEST_ASSERT(sample1.ic_raw == 250U);
    TEST_ASSERT(sample1.ib_raw == 220U);
    TEST_ASSERT(sample1.ia_raw == 90U);
    TEST_ASSERT(sample1.vbus_mv == 12200);
    TEST_ASSERT(sample1.ia_ma == -400);
    TEST_ASSERT(sample1.ib_ma == 500);
    TEST_ASSERT(sample1.ic_ma == -600);
    TEST_ASSERT(sample1.theta_sim_mrad == 750);
    TEST_ASSERT(sample1.theta_elec_mrad == 900);
    TEST_ASSERT(sample1.we_now_mrad_s == 300);
    TEST_ASSERT(sample1.we_ramp_mrad_s == 1500);
    TEST_ASSERT(sample1.vd_mv == -30);
    TEST_ASSERT(sample1.vq_mv == 40);
    TEST_ASSERT(sample1.duty_a_permille == 350U);
    TEST_ASSERT(sample1.duty_b_permille == 550U);
    TEST_ASSERT(sample1.duty_c_permille == 650U);
    TEST_ASSERT(sample1.sector == 5U);
    TEST_ASSERT(sample1.axis_state == (uint8_t)AXIS_STATE_RUNNING);
    TEST_ASSERT((sample1.flags & MOTOR_BENCH_SAMPLE_FLAG_IF_ENABLE) == 0U);
}

static void test_trace_ring_wrap_keeps_the_latest_64_samples(void)
{
    struct motor_dev          motor;
    struct motor_bench_sample oldest;
    struct motor_bench_sample newest;
    uint32_t                  index;

    test_env_reset();
    test_motor_init(&motor);

    for (index = 0U; index < 70U; ++index) {
        motor.meas_data.v_bus_raw = 1000.0f + (float)index;
        motor.meas_data.ia_raw = 100.0f + (float)index;
        motor.meas_data.ib_raw = 200.0f + (float)index;
        motor.meas_data.ic_raw = 300.0f + (float)index;
        motor.meas_data.a = (float)index * 0.001f;
        motor.meas_data.b = -(float)index * 0.002f;
        motor.meas_data.c = (float)index * 0.003f;
        motor.meas_data.v_bus = 10.0f + (float)index * 0.01f;
        motor.ctrl_state.theta_sim = (float)index * 0.01f;
        motor.enc_pri.state.angle_elec_rad = (float)index * 0.02f;
        motor.ctrl_state.we_volt_now = (float)index * 0.03f;
        motor.ctrl_state.we_volt_ramp = (float)index * 0.04f;
        motor.foc_calc.v_d = (float)index * 0.01f;
        motor.foc_calc.v_q = (float)index * 0.02f;
        motor.foc_calc.dtc_a = 0.100f + (float)index * 0.001f;
        motor.foc_calc.dtc_b = 0.200f + (float)index * 0.001f;
        motor.foc_calc.dtc_c = 0.300f + (float)index * 0.001f;
        motor.foc_calc.sector = (uint8_t)((index % 6U) + 1U);
        test_capture_hw(index + 1U);
        motor_bench_trace_update(&motor);
    }

    TEST_ASSERT(motor_bench_trace_sample_count() == 64U);
    TEST_ASSERT(motor_bench_trace_sample_read(0U, &oldest) == true);
    TEST_ASSERT(motor_bench_trace_sample_read(63U, &newest) == true);
    TEST_ASSERT(oldest.sequence == 7U);
    TEST_ASSERT(newest.sequence == 70U);
    TEST_ASSERT(oldest.tmr1_cval == 0x2007U);
    TEST_ASSERT(newest.tmr1_cval == 0x2046U);
}

static void test_trace_sample_read_rejects_invalid_index_and_null_output(void)
{
    struct motor_dev          motor;
    struct motor_bench_sample sample;

    test_env_reset();
    test_motor_init(&motor);
    test_capture_hw(1U);
    motor_bench_trace_update(&motor);

    memset(&sample, 0xA5, sizeof(sample));
    TEST_ASSERT(motor_bench_trace_sample_read(1U, &sample) == false);
    TEST_ASSERT(sample.sequence == 0xA5A5A5A5U);
    TEST_ASSERT(motor_bench_trace_sample_read(0U, NULL) == false);
}

static void test_trace_sample_saturates_fixed_point_and_flags_conversion(void)
{
    struct motor_dev          motor;
    struct motor_bench_sample sample;
    struct motor_hw_snapshot  hw;

    test_env_reset();
    test_motor_init(&motor);

    memset(&hw, 0, sizeof(hw));
    hw.tmr1_cval = UINT32_MAX;
    hw.tmr1_pr   = 1000U;
    hw.adc_sts   = 0x12345678U;
    motor_fault_runtime_stage_hw_snapshot(&hw);

    motor.meas_data.v_bus_raw = 70000.0f;
    motor.meas_data.ic_raw = -1.0f;
    motor.meas_data.ib_raw = NAN;
    motor.meas_data.ia_raw = 2048.0f;
    motor.meas_data.v_bus = 100.0f;
    motor.meas_data.a = 100.0f;
    motor.meas_data.b = -100.0f;
    motor.meas_data.c = NAN;
    motor.ctrl_state.theta_sim = 100.0f;
    motor.enc_pri.state.angle_elec_rad = -100.0f;
    motor.ctrl_state.we_volt_now = INFINITY;
    motor.ctrl_state.we_volt_ramp = -INFINITY;
    motor.foc_calc.v_d = 100.0f;
    motor.foc_calc.v_q = -100.0f;
    motor.foc_calc.dtc_a = 0.25f;
    motor.foc_calc.dtc_b = 0.50f;
    motor.foc_calc.dtc_c = 0.75f;

    motor_bench_trace_update(&motor);

    TEST_ASSERT(MOTOR_BENCH_TRACE_DEPTH == 64U);
    TEST_ASSERT((MOTOR_BENCH_TRACE_DEPTH * sizeof(struct motor_bench_sample)) <= (5U * 1024U));
    TEST_ASSERT(motor_bench_trace_sample_read(0U, &sample) == true);
    TEST_ASSERT(sample.tmr1_cval == UINT16_MAX);
    TEST_ASSERT(sample.tmr1_pr == 1000U);
    TEST_ASSERT(sample.adc_ists == 0x12345678U);
    TEST_ASSERT(sample.adc_vbus_raw == UINT16_MAX);
    TEST_ASSERT(sample.adc_ic_raw == 0U);
    TEST_ASSERT(sample.adc_ib_raw == 0U);
    TEST_ASSERT(sample.adc_ia_raw == 2048U);
    TEST_ASSERT(sample.vbus_mV == UINT16_MAX);
    TEST_ASSERT(sample.ia_mA == INT16_MAX);
    TEST_ASSERT(sample.ib_mA == INT16_MIN);
    TEST_ASSERT(sample.ic_mA == 0);
    TEST_ASSERT(sample.theta_sim_mrad == INT16_MAX);
    TEST_ASSERT(sample.theta_elec_mrad == INT16_MIN);
    TEST_ASSERT(sample.we_now_mrad_s == 0);
    TEST_ASSERT(sample.we_ramp_mrad_s == 0);
    TEST_ASSERT(sample.vd_mV == INT16_MAX);
    TEST_ASSERT(sample.vq_mV == INT16_MIN);
    TEST_ASSERT((sample.flags & MOTOR_BENCH_SAMPLE_FLAG_CONVERSION) != 0U);
}

static void test_trace_sample_accepts_exact_fixed_point_limits_without_saturation(void)
{
    struct motor_dev          motor;
    struct motor_bench_sample sample;
    struct motor_hw_snapshot  hw;

    test_env_reset();
    test_motor_init(&motor);
    memset(&hw, 0, sizeof(hw));
    hw.tmr1_cval = UINT16_MAX;
    motor_fault_runtime_stage_hw_snapshot(&hw);
    motor.meas_data.v_bus_raw = (float)UINT16_MAX;
    motor.meas_data.v_bus = 1.0f;
    motor.meas_data.a = 32.767f;
    motor.meas_data.b = -32.768f;

    motor_bench_trace_update(&motor);

    TEST_ASSERT(motor_bench_trace_sample_read(0U, &sample) == true);
    TEST_ASSERT(sample.tmr1_cval == UINT16_MAX);
    TEST_ASSERT(sample.adc_vbus_raw == UINT16_MAX);
    TEST_ASSERT(sample.ia_mA == INT16_MAX);
    TEST_ASSERT(sample.ib_mA == INT16_MIN);
    TEST_ASSERT((sample.flags & MOTOR_BENCH_SAMPLE_FLAG_CONVERSION) == 0U);
}

static void test_trace_clear_resets_validity_count_and_sequence(void)
{
    struct motor_dev          motor;
    struct motor_bench_trace  trace;
    struct motor_bench_sample sample;

    test_env_reset();
    test_motor_init(&motor);
    test_capture_hw(1U);
    motor_bench_trace_update(&motor);

    TEST_ASSERT(motor_bench_trace_sample_count() == 1U);
    motor_bench_trace_sample_clear();
    motor_bench_trace_clear();

    TEST_ASSERT(motor_bench_trace_sample_count() == 0U);
    TEST_ASSERT(motor_bench_trace_sample_read(0U, &sample) == false);
    TEST_ASSERT(motor_bench_trace_read(&trace) == false);
    TEST_ASSERT(trace.valid == 0U);

    test_capture_hw(2U);
    motor_bench_trace_update(&motor);
    TEST_ASSERT(motor_bench_trace_sample_read(0U, &sample) == true);
    TEST_ASSERT(sample.sequence == 1U);
}

static void test_fault_context_captures_staged_register_snapshot_before_pwm_disable(void)
{
    struct motor_dev           motor;
    struct motor_fault_context context;

    test_env_reset();
    test_motor_init(&motor);
    test_capture_hw(9U);
    motor.foc_calc.sector = 4U;
    motor.foc_calc.v_d = 0.11f;
    motor.foc_calc.v_q = 0.22f;
    motor.foc_calc.v_alpha = 0.33f;
    motor.foc_calc.v_beta = -0.44f;
    motor.foc_calc.dtc_a = 0.45f;
    motor.foc_calc.dtc_b = 0.55f;
    motor.foc_calc.dtc_c = 0.65f;
    motor.foc_calc.i_d = -0.12f;
    motor.foc_calc.i_q = 0.18f;
    motor.meas_data.ia_raw = 2100.0f;
    motor.meas_data.ib_raw = 2200.0f;
    motor.meas_data.ic_raw = 2300.0f;
    motor.meas_data.a = FOC_FAST_CURRENT_TRIP_A + 0.01f;
    motor.meas_data.b = -0.03f;
    motor.meas_data.c = 0.02f;
    motor.meas_data.v_bus_raw = 1350.0f;
    motor.meas_data.theta_elec = 1.75f;
    motor.ctrl_state.theta_sim = 2.25f;
    motor.ctrl_state.we_volt_now = 1.25f;
    motor.ctrl_state.we_volt_ramp = 3.14f;

    foc_fault_check_fast(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_OVER_CURR) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1U);
    TEST_ASSERT(g_context_visible_before_pwm_disable == true);
    TEST_ASSERT(motor_fault_context_read(&context) == true);
    TEST_ASSERT(context.tmr1_ists == 0x1009U);
    TEST_ASSERT(context.tmr1_cval == 0x2009U);
    TEST_ASSERT(context.tmr1_pr == 0x3009U);
    TEST_ASSERT(context.tmr1_ccr1 == 0x4009U);
    TEST_ASSERT(context.tmr1_ccr2 == 0x5009U);
    TEST_ASSERT(context.tmr1_ccr3 == 0x6009U);
    TEST_ASSERT(context.tmr1_ccr4 == 0x7009U);
    TEST_ASSERT(context.tmr1_cctrl == 0x8009U);
    TEST_ASSERT(context.tmr1_brk == 0x9009U);
    TEST_ASSERT(context.tmr1_count_dir == 1U);
    TEST_ASSERT(context.adc_sts == 0xA009U);
    TEST_ASSERT(g_context_at_pwm_disable.tmr1_cval == 0x2009U);
}

static void test_trace_does_not_clear_or_overwrite_fault_context(void)
{
    struct motor_dev           fault_motor;
    struct motor_dev           sample_motor;
    struct motor_fault_context first;
    struct motor_fault_context second;

    test_env_reset();
    test_motor_init(&fault_motor);
    test_capture_hw(3U);
    fault_motor.meas_data.a = FOC_FAST_CURRENT_TRIP_A + 0.01f;
    foc_fault_check_fast(&fault_motor);
    TEST_ASSERT(motor_fault_context_read(&first) == true);

    test_motor_init(&sample_motor);
    sample_motor.meas_data.a = 0.01f;
    sample_motor.meas_data.b = 0.02f;
    sample_motor.meas_data.c = 0.03f;
    sample_motor.meas_data.v_bus = 12.0f;
    sample_motor.foc_calc.dtc_a = 0.25f;
    sample_motor.foc_calc.dtc_b = 0.50f;
    sample_motor.foc_calc.dtc_c = 0.75f;
    test_capture_hw(4U);
    motor_bench_trace_update(&sample_motor);
    motor_bench_trace_sample_clear();

    TEST_ASSERT(motor_fault_context_read(&second) == true);
    TEST_ASSERT(second.sequence == first.sequence);
    TEST_ASSERT(second.fault_code == first.fault_code);
    TEST_ASSERT(second.tmr1_cval == first.tmr1_cval);
}

static void test_diag_trace_reads_are_stopped_only(void)
{
    char *show_argv[] = {"diag", "trace", "show"};
    char *sample_argv[] = {"diag", "trace", "sample", "0"};

    test_env_reset();
    g_snapshot_axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(app_shell_diag_command(3, show_argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=trace action=show result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=stopped_only") != NULL);

    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    g_snapshot_axis_state = AXIS_STATE_CALIB;
    TEST_ASSERT(app_shell_diag_command(4, sample_argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=trace action=sample result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=stopped_only") != NULL);
}

static void test_diag_trace_sample_reads_one_record_after_stop(void)
{
    struct motor_dev motor;
    char            *argv[] = {"diag", "trace", "sample", "1"};

    test_env_reset();
    test_motor_init(&motor);

    motor.meas_data.v_bus_raw = 1200.0f;
    motor.meas_data.ia_raw = 100.0f;
    motor.meas_data.ib_raw = 200.0f;
    motor.meas_data.ic_raw = 300.0f;
    motor.meas_data.a = 0.10f;
    motor.meas_data.b = 0.20f;
    motor.meas_data.c = 0.30f;
    motor.meas_data.v_bus = 11.8f;
    motor.ctrl_state.theta_sim = 0.25f;
    motor.enc_pri.state.angle_elec_rad = 0.50f;
    motor.ctrl_state.we_volt_now = 0.10f;
    motor.ctrl_state.we_volt_ramp = 1.00f;
    motor.foc_calc.v_d = 0.01f;
    motor.foc_calc.v_q = 0.02f;
    motor.foc_calc.dtc_a = 0.40f;
    motor.foc_calc.dtc_b = 0.50f;
    motor.foc_calc.dtc_c = 0.60f;
    motor.foc_calc.sector = 2U;
    test_capture_hw(1U);
    motor_bench_trace_update(&motor);

    motor.meas_data.v_bus_raw = 1210.0f;
    motor.meas_data.ia_raw = 110.0f;
    motor.meas_data.ib_raw = 210.0f;
    motor.meas_data.ic_raw = 310.0f;
    motor.meas_data.a = 0.11f;
    motor.meas_data.b = 0.21f;
    motor.meas_data.c = 0.31f;
    motor.meas_data.v_bus = 12.1f;
    motor.ctrl_state.theta_sim = 0.55f;
    motor.enc_pri.state.angle_elec_rad = 0.80f;
    motor.ctrl_state.we_volt_now = 0.20f;
    motor.ctrl_state.we_volt_ramp = 1.30f;
    motor.foc_calc.v_d = 0.03f;
    motor.foc_calc.v_q = 0.04f;
    motor.foc_calc.dtc_a = 0.45f;
    motor.foc_calc.dtc_b = 0.55f;
    motor.foc_calc.dtc_c = 0.65f;
    motor.foc_calc.sector = 4U;
    test_capture_hw(2U);
    motor_bench_trace_update(&motor);

    g_snapshot_axis_state = AXIS_STATE_IDLE;
    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=trace action=sample result=ok index=1") != NULL);
    TEST_ASSERT(strstr(g_output, "sequence=2") != NULL);
    TEST_ASSERT(strstr(g_output, "tmr1_cval=8194") != NULL);
    TEST_ASSERT(strstr(g_output, "raw=1210/310/210/110") != NULL);
}

static void test_bench_shell_stack_has_diagnostic_headroom(void)
{
    TEST_ASSERT(TM_APP_TASK_STACK_SHELL_BYTES >= 2048U);
}

int main(void)
{
    test_trace_captures_first_last_ranges_peaks_and_samples();
    test_trace_ring_wrap_keeps_the_latest_64_samples();
    test_trace_sample_read_rejects_invalid_index_and_null_output();
    test_trace_sample_saturates_fixed_point_and_flags_conversion();
    test_trace_sample_accepts_exact_fixed_point_limits_without_saturation();
    test_trace_clear_resets_validity_count_and_sequence();
    test_fault_context_captures_staged_register_snapshot_before_pwm_disable();
    test_trace_does_not_clear_or_overwrite_fault_context();
    test_diag_trace_reads_are_stopped_only();
    test_diag_trace_sample_reads_one_record_after_stop();
    test_bench_shell_stack_has_diagnostic_headroom();

    puts("[test_motor_bench_trace_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
