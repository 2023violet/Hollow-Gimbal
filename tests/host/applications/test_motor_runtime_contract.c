/**
 * @file    test_motor_runtime_contract.c
 * @brief   Host contract tests for motor runtime owner seams
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "motor_runtime.h"
#include "motor_store.h"

void     host_rtos_reset(void);
void     host_rtos_set_ipsr(uint32_t ipsr);
uint32_t host_rtos_get_isr_critical_enter_count(void);
uint32_t host_rtos_get_isr_critical_exit_count(void);
void     host_runtime_stub_reset_lpf_counters(void);
uint32_t host_runtime_stub_get_lpf_init_count(void);
uint32_t host_runtime_stub_get_lpf_refresh_count(void);
void     host_runtime_stub_set_motor_load(int status, float curr_bandwidth);
void     host_runtime_stub_reset_fault_context_counters(void);
uint32_t host_runtime_stub_get_fault_context_clear_count(void);
void     host_runtime_stub_reset_bench_maps(void);
uint8_t  host_runtime_stub_get_pwm_map(void);
uint8_t  host_runtime_stub_get_adc_map(void);
uint16_t host_runtime_stub_get_adc_trigger(void);

bool motor_runtime_diag_set_pwm_map(uint8_t map_id);
bool motor_runtime_diag_set_adc_map(uint8_t map_id);
void motor_runtime_diag_reset_maps(void);
bool motor_runtime_diag_set_adc_trigger(uint16_t compare);

#define TEST_ASSERT(expr)                                                     \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "[test_motor_runtime_contract] assertion failed: %s\n",  \
                    #expr);                                                   \
            exit(1);                                                          \
    }                                                                     \
    } while (0)

static void test_runtime_init_and_clear_hooks(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));

    motor_runtime_init(&motor);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(motor.ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_INACTIVE);
    TEST_ASSERT(motor.ctrl_state.theta_seed_offset_rad == 0.0f);

    motor.ctrl_state.target_vel = 9.0f;
    motor.ctrl_state.target_pos = 7.0f;
    motor_runtime_reset_transient_setpoints(&motor);
    TEST_ASSERT(motor.ctrl_state.target_vel == 0.0f);
    TEST_ASSERT(motor.ctrl_state.target_pos == motor.meas_data.out_pos_rad);
    TEST_ASSERT(motor.ctrl_state.theta_seed_offset_rad == 0.0f);
}

static void test_runtime_phase_seed_offset_is_bench_idle_only(void)
{
    struct motor_dev motor;
    const float      original = 0.25f;

    memset(&motor, 0, sizeof(motor));
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);

    TEST_ASSERT(motor_runtime_diag_set_phase_seed_offset(FOC_PI * 0.5f) == true);
    TEST_ASSERT(fabsf(motor.ctrl_state.theta_seed_offset_rad - (FOC_PI * 0.5f)) < 0.0001f);

    TEST_ASSERT(motor_runtime_diag_set_phase_seed_offset(original) == true);
    motor.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(motor_runtime_diag_set_phase_seed_offset(-original) == false);
    TEST_ASSERT(fabsf(motor.ctrl_state.theta_seed_offset_rad - original) < 0.0001f);

    motor.axis_state = AXIS_STATE_IDLE;
    TEST_ASSERT(motor_runtime_diag_set_phase_seed_offset(NAN) == false);
    TEST_ASSERT(motor_runtime_diag_set_phase_seed_offset(INFINITY) == false);
    TEST_ASSERT(motor_runtime_diag_set_phase_seed_offset(FOC_PI + 0.01f) == false);
    TEST_ASSERT(motor_runtime_diag_set_phase_seed_offset(-FOC_PI - 0.01f) == false);
    TEST_ASSERT(fabsf(motor.ctrl_state.theta_seed_offset_rad - original) < 0.0001f);
}

static void test_runtime_bench_encoder_direction_is_idle_only_and_ram_only(void)
{
    struct motor_dev motor;
    uint8_t          original_dir;

    memset(&motor, 0, sizeof(motor));
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);
    original_dir = motor.enc_pri.config->dir;

    TEST_ASSERT(motor_runtime_diag_set_encoder_dir(DIR_CW) == true);
    TEST_ASSERT(motor.enc_pri.config->dir == DIR_CW);

    motor.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(motor_runtime_diag_set_encoder_dir(DIR_CCW) == false);
    TEST_ASSERT(motor.enc_pri.config->dir == DIR_CW);

    motor.axis_state = AXIS_STATE_IDLE;
    TEST_ASSERT(motor_runtime_diag_set_encoder_dir(DIR_CCW) == true);
    TEST_ASSERT(motor.enc_pri.config->dir == DIR_CCW);
    TEST_ASSERT(motor_runtime_diag_set_encoder_dir(2U) == false);
    TEST_ASSERT(motor.enc_pri.config->dir == DIR_CCW);

    motor.enc_pri.config->dir = original_dir;
}

static float expected_lpf_alpha(float curr_bandwidth)
{
    float cutoff_hz = curr_bandwidth * (DEFAULT_CUTOFF_HZ / DEFAULT_CURR_BW_HZ);
    float wc        = FOC_HZ_TO_RADS(cutoff_hz);

    return wc / (wc + DEFAULT_SAMPLING_HZ);
}

static void test_runtime_init_uses_persisted_bandwidth_for_lpf(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    host_runtime_stub_set_motor_load(PARAM_OK, 100.0f);
    host_runtime_stub_reset_lpf_counters();

    motor_runtime_init(&motor);

    TEST_ASSERT(motor.ctrl_config.curr_bandwidth == 100.0f);
    TEST_ASSERT(host_runtime_stub_get_lpf_init_count() == 1U);
    TEST_ASSERT(fabsf(motor.ctrl_state.id_filter.alpha - expected_lpf_alpha(100.0f)) < 1.0e-6f);
    TEST_ASSERT(fabsf(motor.ctrl_state.iq_filter.alpha - expected_lpf_alpha(100.0f)) < 1.0e-6f);

    host_runtime_stub_set_motor_load(PARAM_EMPTY, DEFAULT_CURR_BW);
}

static void test_runtime_init_rejects_invalid_persisted_bandwidth(void)
{
    const float invalid_values[] = {0.0f, -1.0f, NAN, INFINITY};

    for (size_t i = 0U; i < sizeof(invalid_values) / sizeof(invalid_values[0]); ++i) {
        struct motor_dev motor;

        memset(&motor, 0, sizeof(motor));
        host_runtime_stub_set_motor_load(PARAM_OK, invalid_values[i]);
        motor_runtime_init(&motor);

        TEST_ASSERT(motor.ctrl_config.curr_bandwidth == DEFAULT_CURR_BW_HZ);
        TEST_ASSERT(fabsf(motor.ctrl_state.id_filter.alpha - expected_lpf_alpha(DEFAULT_CURR_BW_HZ)) < 1.0e-6f);
        TEST_ASSERT(fabsf(motor.ctrl_state.iq_filter.alpha - expected_lpf_alpha(DEFAULT_CURR_BW_HZ)) < 1.0e-6f);
    }

    host_runtime_stub_set_motor_load(PARAM_EMPTY, DEFAULT_CURR_BW);
}

static void test_runtime_currbw_refreshes_lpf_without_resetting_output(void)
{
    struct motor_dev motor;
    float            old_output;

    memset(&motor, 0, sizeof(motor));
    host_runtime_stub_set_motor_load(PARAM_EMPTY, DEFAULT_CURR_BW);
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);
    motor.ctrl_state.id_filter.output = 1.25f;
    motor.ctrl_state.iq_filter.output = -0.75f;
    old_output = motor.ctrl_state.id_filter.output;
    host_runtime_stub_reset_lpf_counters();

    TEST_ASSERT(motor_runtime_diag_set_param("currbw", 300.0f) == true);
    TEST_ASSERT(motor.ctrl_config.curr_bandwidth == 300.0f);
    TEST_ASSERT(host_runtime_stub_get_lpf_refresh_count() == 1U);
    TEST_ASSERT(motor.ctrl_state.id_filter.output == old_output);
    TEST_ASSERT(motor.ctrl_state.iq_filter.output == -0.75f);
    TEST_ASSERT(fabsf(motor.ctrl_state.id_filter.alpha - expected_lpf_alpha(300.0f)) < 1.0e-6f);
}

static void test_runtime_currbw_rejects_nonfinite_and_nonpositive_values(void)
{
    const float invalid_values[] = {0.0f, -1.0f, NAN, INFINITY};
    struct motor_dev motor;
    float            old_alpha;

    memset(&motor, 0, sizeof(motor));
    host_runtime_stub_set_motor_load(PARAM_EMPTY, DEFAULT_CURR_BW);
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);
    motor.ctrl_config.curr_bandwidth = 300.0f;
    old_alpha = motor.ctrl_state.id_filter.alpha;

    for (size_t i = 0U; i < sizeof(invalid_values) / sizeof(invalid_values[0]); ++i) {
        host_runtime_stub_reset_lpf_counters();
        TEST_ASSERT(motor_runtime_diag_set_param("currbw", invalid_values[i]) == false);
        TEST_ASSERT(motor.ctrl_config.curr_bandwidth == 300.0f);
        TEST_ASSERT(host_runtime_stub_get_lpf_refresh_count() == 0U);
        TEST_ASSERT(motor.ctrl_state.id_filter.alpha == old_alpha);
    }
}

static void test_runtime_diag_writes_are_rejected_while_running(void)
{
    struct motor_dev motor;
    float            old_offset;
    float            old_curr_bw;
    float            old_kp;

    memset(&motor, 0, sizeof(motor));
    host_runtime_stub_set_motor_load(PARAM_EMPTY, DEFAULT_CURR_BW);
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);
    motor.axis_state = AXIS_STATE_RUNNING;

    motor.ctrl_mode               = CTRL_MODE_CURR;
    motor.input_mode              = INPUT_MODE_INACTIVE;
    motor.ctrl_state.vd_set       = 0.7f;
    motor.ctrl_state.vq_set       = 0.8f;
    motor.ctrl_state.we_volt_ramp = 0.9f;
    motor.ctrl_state.if_enable    = 0U;
    old_curr_bw                   = motor.ctrl_config.curr_bandwidth;
    old_offset                    = motor.enc_pri.config->mech_offset;
    old_kp                        = motor.ctrl_config.iq_pid.config.kp;

    TEST_ASSERT(motor_runtime_request_set_passthrough(CTRL_MODE_CURR, 1.0f, 0.0f) == false);
    TEST_ASSERT(motor_runtime_diag_set_voltage(1.0f, 2.0f, 3.0f) == false);
    TEST_ASSERT(motor_runtime_diag_set_if(0.2f, 3.14f) == false);
    TEST_ASSERT(motor_runtime_diag_set_phase_volt(1, 0.1f) == false);
    TEST_ASSERT(motor_runtime_diag_set_param("currbw", 300.0f) == false);
    TEST_ASSERT(motor_runtime_diag_set_param("encoff", 0.25f) == false);
    TEST_ASSERT(motor_runtime_diag_set_pid("iq", "kp", 2.0f) == false);
    TEST_ASSERT(motor_runtime_diag_reset_pid("iq") == false);

    TEST_ASSERT(motor.ctrl_mode == CTRL_MODE_CURR);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_INACTIVE);
    TEST_ASSERT(motor.ctrl_state.vd_set == 0.7f);
    TEST_ASSERT(motor.ctrl_state.vq_set == 0.8f);
    TEST_ASSERT(motor.ctrl_state.we_volt_ramp == 0.9f);
    TEST_ASSERT(motor.ctrl_state.if_enable == 0U);
    TEST_ASSERT(motor.ctrl_config.curr_bandwidth == old_curr_bw);
    TEST_ASSERT(motor.enc_pri.config->mech_offset == old_offset);
    TEST_ASSERT(motor.ctrl_config.iq_pid.config.kp == old_kp);
}

static void test_runtime_diag_writes_remain_available_in_idle(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    host_runtime_stub_set_motor_load(PARAM_EMPTY, DEFAULT_CURR_BW);
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);
    motor.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(motor_runtime_request_set_passthrough(CTRL_MODE_CURR, 0.1f, 0.0f) == true);
    TEST_ASSERT(motor.ctrl_mode == CTRL_MODE_CURR);
    TEST_ASSERT(motor.ctrl_state.target_iq == 0.1f);
}

static void test_runtime_bench_maps_are_idle_only_and_do_not_change_product_state(void)
{
    struct motor_dev motor;
    float            old_peak;
    uint8_t          old_phase_order;

    memset(&motor, 0, sizeof(motor));
    host_runtime_stub_set_motor_load(PARAM_EMPTY, DEFAULT_CURR_BW);
    host_runtime_stub_reset_bench_maps();
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);
    old_peak = motor.prot_config.i_peak;
    old_phase_order = motor.hw_config.phase_order;

    for (uint8_t map_id = 0U; map_id < 6U; ++map_id) {
        TEST_ASSERT(motor_runtime_diag_set_pwm_map(map_id) == true);
        TEST_ASSERT(motor_runtime_diag_set_adc_map(map_id) == true);
        TEST_ASSERT(host_runtime_stub_get_pwm_map() == map_id);
        TEST_ASSERT(host_runtime_stub_get_adc_map() == map_id);
    }
    TEST_ASSERT(motor_runtime_diag_set_pwm_map(6U) == false);
    TEST_ASSERT(motor_runtime_diag_set_adc_map(6U) == false);

    motor.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(motor_runtime_diag_set_pwm_map(0U) == false);
    TEST_ASSERT(motor_runtime_diag_set_adc_map(0U) == false);
    TEST_ASSERT(motor.hw_config.phase_order == old_phase_order);
    TEST_ASSERT(motor.prot_config.i_peak == old_peak);

    motor.axis_state = AXIS_STATE_IDLE;
    motor_runtime_diag_reset_maps();
    TEST_ASSERT(host_runtime_stub_get_pwm_map() == 0U);
    TEST_ASSERT(host_runtime_stub_get_adc_map() == 0U);
}

static void test_runtime_bench_adc_trigger_is_bounded_and_idle_only(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);
    host_runtime_stub_reset_bench_maps();

    TEST_ASSERT(motor_runtime_diag_set_adc_trigger(256U) == true);
    TEST_ASSERT(host_runtime_stub_get_adc_trigger() == 256U);
    TEST_ASSERT(motor_runtime_diag_set_adc_trigger(3740U) == true);
    TEST_ASSERT(host_runtime_stub_get_adc_trigger() == 3740U);
    TEST_ASSERT(motor_runtime_diag_set_adc_trigger(255U) == false);
    TEST_ASSERT(motor_runtime_diag_set_adc_trigger(3741U) == false);

    motor.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(motor_runtime_diag_set_adc_trigger(1024U) == false);
    TEST_ASSERT(host_runtime_stub_get_adc_trigger() == 3740U);
}

static void test_runtime_fault_clear_clears_latched_context(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    host_runtime_stub_set_motor_load(PARAM_EMPTY, DEFAULT_CURR_BW);
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);
    host_runtime_stub_reset_fault_context_counters();
    motor.axis_state = AXIS_STATE_FAULT;
    motor.fault_code = FAULT_OVER_CURR;

    TEST_ASSERT(motor_runtime_request_axis_action(TM_MOTOR_RUNTIME_AXIS_CLEAR_FAULT) == true);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(motor.fault_code == FAULT_NONE);
    TEST_ASSERT(host_runtime_stub_get_fault_context_clear_count() == 1U);
}

static void test_runtime_disable_freezes_encoder_trace_until_fault_clear(void)
{
    struct motor_dev              motor;
    struct encoder_bench_input    input;
    struct encoder_bench_sample   sample;

    memset(&motor, 0, sizeof(motor));
    memset(&input, 0, sizeof(input));
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);

    encoder_bench_trace_clear();
    input.raw_word = 0x1111U;
    encoder_bench_trace_update(&input, 11, 0U);

    motor.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(motor_runtime_request_axis_action(TM_MOTOR_RUNTIME_AXIS_DISABLE) == true);
    input.raw_word = 0x2222U;
    encoder_bench_trace_update(&input, 22, 0U);
    TEST_ASSERT(encoder_bench_trace_count() == 1U);
    TEST_ASSERT(encoder_bench_trace_read(0U, &sample));
    TEST_ASSERT(sample.raw_word == 0x1111U);

    motor.axis_state = AXIS_STATE_FAULT;
    TEST_ASSERT(motor_runtime_request_axis_action(TM_MOTOR_RUNTIME_AXIS_CLEAR_FAULT) == true);
    input.raw_word = 0x3333U;
    encoder_bench_trace_update(&input, 33, 0U);
    TEST_ASSERT(encoder_bench_trace_count() == 2U);
    TEST_ASSERT(encoder_bench_trace_read(1U, &sample));
    TEST_ASSERT(sample.raw_word == 0x3333U);
}

static void test_runtime_bench_keepalive_is_running_only_and_observable(void)
{
    struct motor_dev motor;
    struct motor_runtime_bench_status status;

    memset(&motor, 0, sizeof(motor));
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);

    TEST_ASSERT(motor_runtime_diag_bench_keepalive() == false);
    TEST_ASSERT(motor_runtime_diag_bench_status(&status) == true);
    TEST_ASSERT(status.armed == false);

    motor.axis_state = AXIS_STATE_RUNNING;
    TEST_ASSERT(motor_runtime_diag_bench_keepalive() == true);
    TEST_ASSERT(motor_runtime_diag_bench_status(&status) == true);
    TEST_ASSERT(status.armed == true);
    TEST_ASSERT(status.expired == false);
    TEST_ASSERT(status.heartbeat_age_ms == 0U);
}

static void test_runtime_start_requires_valid_fresh_adc_calibration(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor_runtime_init(&motor);
    motor_runtime_attach(&motor);

    TEST_ASSERT(motor_runtime_request_axis_action(TM_MOTOR_RUNTIME_AXIS_START) == false);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);

    motor.meas_data.adc_calib_valid = true;
    motor.meas_data.adc_calib_sample_count = CALIB_CURR_DUR;
    motor.meas_data.adc_calib_fresh_count = CALIB_CURR_DUR;
    TEST_ASSERT(motor_runtime_request_axis_action(TM_MOTOR_RUNTIME_AXIS_START) == true);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_RUNNING);
}

static void test_runtime_attach_and_current_binding(void)
{
    struct motor_dev motor_a;
    struct motor_dev motor_b;

    memset(&motor_a, 0, sizeof(motor_a));
    memset(&motor_b, 0, sizeof(motor_b));

    motor_runtime_attach(NULL);
    TEST_ASSERT(motor_runtime_current() == NULL);

    motor_runtime_attach(&motor_a);
    TEST_ASSERT(motor_runtime_current() == &motor_a);

    motor_runtime_attach(&motor_b);
    TEST_ASSERT(motor_runtime_current() == &motor_b);
}

static void test_runtime_snapshot_returns_false_and_zeros_when_unattached(void)
{
    struct motor_runtime_snapshot snapshot;

    memset(&snapshot, 0xA5, sizeof(snapshot));

    motor_runtime_attach(NULL);

    TEST_ASSERT(motor_runtime_snapshot(&snapshot) == false);
    TEST_ASSERT(snapshot.axis_state == 0U);
    TEST_ASSERT(snapshot.ctrl_mode == 0U);
    TEST_ASSERT(snapshot.input_mode == 0U);
    TEST_ASSERT(snapshot.sensor_type == 0U);
    TEST_ASSERT(snapshot.fault_code == 0U);
    TEST_ASSERT(snapshot.enc_valid == false);
    TEST_ASSERT(snapshot.v_bus == 0.0f);
    TEST_ASSERT(snapshot.i_q == 0.0f);
    TEST_ASSERT(snapshot.i_d == 0.0f);
    TEST_ASSERT(snapshot.out_vel_rpm == 0.0f);
    TEST_ASSERT(snapshot.out_pos_turns == 0.0f);
    TEST_ASSERT(snapshot.motor_temp == 0.0f);
    TEST_ASSERT(snapshot.driver_temp == 0.0f);
    TEST_ASSERT(snapshot.angle_mech_rad == 0.0f);
    TEST_ASSERT(snapshot.angle_elec_rad == 0.0f);
    TEST_ASSERT(snapshot.encoder_vel_rads == 0.0f);
    TEST_ASSERT(snapshot.save_req == 0U);
    TEST_ASSERT(snapshot.calib_state == 0U);
    TEST_ASSERT(snapshot.rs_state == 0U);
    TEST_ASSERT(snapshot.ls_state == 0U);
    TEST_ASSERT(snapshot.flux_state == 0U);
    TEST_ASSERT(snapshot.js_state == 0U);
    TEST_ASSERT(snapshot.enc_state == 0U);
    TEST_ASSERT(snapshot.mech_offset == 0.0f);
    TEST_ASSERT(snapshot.pole_pairs == 0U);
}

static void test_runtime_snapshot_returns_false_for_null_output(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor_runtime_attach(&motor);

    TEST_ASSERT(motor_runtime_snapshot(NULL) == false);
}

static void test_runtime_snapshot_captures_live_shell_fields(void)
{
    struct motor_dev              motor;
    struct motor_runtime_snapshot snapshot;

    memset(&motor, 0, sizeof(motor));
    memset(&snapshot, 0, sizeof(snapshot));

    motor_runtime_init(&motor);
    motor.axis_state                   = AXIS_STATE_RUNNING;
    motor.ctrl_mode                    = CTRL_MODE_POS;
    motor.input_mode                   = INPUT_MODE_TRAP_TRAJ;
    motor.sensor_type                  = SENSOR_TYPE_INC_ENC;
    motor.fault_code                   = 0x1234ABCDU;
    motor.calib_state.state            = RS_CALC;
    motor.calib_state.rs_state         = RS_END;
    motor.calib_state.ls_state         = LS_END;
    motor.calib_state.flux_state       = FX_END;
    motor.calib_state.js_state         = JS_END;
    motor.calib_state.enc_state        = ENC_LUT_END;
    motor.enc_pri.state.calib_valid    = true;
    motor.enc_pri.config->mech_offset  = 1.5f;
    motor.enc_pri.config->pole_pairs   = 7U;
    motor.meas_data.v_bus              = 51.25f;
    motor.foc_calc.i_q                 = 7.5f;
    motor.foc_calc.i_d                 = -1.25f;
    motor.meas_data.ia_raw             = 101.0f;
    motor.meas_data.ib_raw             = 202.0f;
    motor.meas_data.ic_raw             = 303.0f;
    motor.meas_data.ia_offset          = 1000.0f;
    motor.meas_data.ib_offset          = 1001.0f;
    motor.meas_data.ic_offset          = 1002.0f;
    motor.meas_data.a                  = 0.11f;
    motor.meas_data.b                  = -0.22f;
    motor.meas_data.c                  = 0.33f;
    motor.meas_data.v_bus_raw          = 4095.0f;
    motor.ctrl_state.theta_sim         = 1.1f;
    motor.ctrl_state.we_volt_now       = 2.2f;
    motor.ctrl_state.we_volt_ramp      = 3.3f;
    motor.meas_data.theta_elec         = 4.4f;
    motor.foc_calc.v_d                 = 0.5f;
    motor.foc_calc.v_q                 = 0.6f;
    motor.foc_calc.v_alpha             = 0.7f;
    motor.foc_calc.v_beta              = 0.8f;
    motor.foc_calc.dtc_a               = 0.2f;
    motor.foc_calc.dtc_b               = 0.4f;
    motor.foc_calc.dtc_c               = 0.6f;
    motor.foc_calc.sector              = 5U;
    motor.meas_data.out_vel_rpm        = 345.5f;
    motor.meas_data.out_pos_turns      = 12.75f;
    motor.meas_data.motor_temp         = 48.0f;
    motor.meas_data.driver_temp        = 52.5f;
    motor.enc_pri.state.angle_mech_rad = 1.125f;
    motor.enc_pri.state.angle_elec_rad = 2.25f;
    motor.enc_pri.state.velocity_rads  = 9.5f;
    motor.calib_state.param_save_req   = PARAM_SAVE_MOTOR | PARAM_SAVE_SYS;

    motor_runtime_attach(&motor);
    host_rtos_reset();
    host_rtos_set_ipsr(1U);

    TEST_ASSERT(motor_runtime_snapshot(&snapshot) == true);
    TEST_ASSERT(host_rtos_get_isr_critical_enter_count() == 1U);
    TEST_ASSERT(host_rtos_get_isr_critical_exit_count() == 1U);
    TEST_ASSERT(snapshot.axis_state == (uint8_t)AXIS_STATE_RUNNING);
    TEST_ASSERT(snapshot.ctrl_mode == (uint8_t)CTRL_MODE_POS);
    TEST_ASSERT(snapshot.input_mode == (uint8_t)INPUT_MODE_TRAP_TRAJ);
    TEST_ASSERT(snapshot.sensor_type == (uint8_t)SENSOR_TYPE_INC_ENC);
    TEST_ASSERT(snapshot.fault_code == 0x1234ABCDU);
    TEST_ASSERT(snapshot.calib_state == (uint8_t)RS_CALC);
    TEST_ASSERT(snapshot.rs_state == (uint8_t)RS_END);
    TEST_ASSERT(snapshot.ls_state == (uint8_t)LS_END);
    TEST_ASSERT(snapshot.flux_state == (uint8_t)FX_END);
    TEST_ASSERT(snapshot.js_state == (uint8_t)JS_END);
    TEST_ASSERT(snapshot.enc_state == (uint8_t)ENC_LUT_END);
    TEST_ASSERT(snapshot.enc_valid == true);
    TEST_ASSERT(snapshot.mech_offset == 1.5f);
    TEST_ASSERT(snapshot.pole_pairs == 7U);
    TEST_ASSERT(snapshot.v_bus == 51.25f);
    TEST_ASSERT(snapshot.i_q == 7.5f);
    TEST_ASSERT(snapshot.i_d == -1.25f);
    TEST_ASSERT(snapshot.ia_raw == 101.0f);
    TEST_ASSERT(snapshot.ib_raw == 202.0f);
    TEST_ASSERT(snapshot.ic_raw == 303.0f);
    TEST_ASSERT(snapshot.ia_offset == 1000.0f);
    TEST_ASSERT(snapshot.ib_offset == 1001.0f);
    TEST_ASSERT(snapshot.ic_offset == 1002.0f);
    TEST_ASSERT(snapshot.ia == 0.11f);
    TEST_ASSERT(snapshot.ib == -0.22f);
    TEST_ASSERT(snapshot.ic == 0.33f);
    TEST_ASSERT(snapshot.v_bus_raw == 4095.0f);
    TEST_ASSERT(snapshot.theta_sim == 1.1f);
    TEST_ASSERT(snapshot.theta_elec == 2.25f);
    TEST_ASSERT(snapshot.we_volt_now == 2.2f);
    TEST_ASSERT(snapshot.we_volt_ramp == 3.3f);
    TEST_ASSERT(snapshot.vd == 0.5f);
    TEST_ASSERT(snapshot.vq == 0.6f);
    TEST_ASSERT(snapshot.v_alpha == 0.7f);
    TEST_ASSERT(snapshot.v_beta == 0.8f);
    TEST_ASSERT(snapshot.dtc_a == 0.2f);
    TEST_ASSERT(snapshot.dtc_b == 0.4f);
    TEST_ASSERT(snapshot.dtc_c == 0.6f);
    TEST_ASSERT(snapshot.sector == 5U);
    TEST_ASSERT(snapshot.out_vel_rpm == 345.5f);
    TEST_ASSERT(snapshot.out_pos_turns == 12.75f);
    TEST_ASSERT(snapshot.motor_temp == 48.0f);
    TEST_ASSERT(snapshot.driver_temp == 52.5f);
    TEST_ASSERT(snapshot.angle_mech_rad == 1.125f);
    TEST_ASSERT(snapshot.angle_elec_rad == 2.25f);
    TEST_ASSERT(snapshot.encoder_vel_rads == 9.5f);
    TEST_ASSERT(snapshot.save_req == (uint8_t)(PARAM_SAVE_MOTOR | PARAM_SAVE_SYS));
}

static void test_runtime_snapshot_refreshes_output_motion_from_encoder_state(void)
{
    struct motor_dev              motor;
    struct motor_runtime_snapshot snapshot;

    memset(&motor, 0, sizeof(motor));
    memset(&snapshot, 0, sizeof(snapshot));

    motor_runtime_init(&motor);
    motor.axis_state = AXIS_STATE_FAULT;
    motor.hw_config.gear_ratio = 1.0f;

    /* Simulate a stale last-running FOC value after the encoder has settled. */
    motor.meas_data.out_vel_rpm   = -334.21f;
    motor.meas_data.out_pos_turns = 0.227f;
    motor.enc_pri.state.velocity_rads = -0.032f;
    motor.enc_pri.state.total_enc     = 0.5f;

    motor_runtime_attach(&motor);

    TEST_ASSERT(motor_runtime_snapshot(&snapshot) == true);
    TEST_ASSERT(fabsf(snapshot.out_vel_rpm - FOC_RADS_TO_RPM(-0.032f)) < 0.001f);
    TEST_ASSERT(fabsf(snapshot.out_pos_turns - 0.5f) < 0.0001f);
}

int main(void)
{
    test_runtime_init_and_clear_hooks();
    test_runtime_phase_seed_offset_is_bench_idle_only();
    test_runtime_bench_encoder_direction_is_idle_only_and_ram_only();
    test_runtime_init_uses_persisted_bandwidth_for_lpf();
    test_runtime_init_rejects_invalid_persisted_bandwidth();
    test_runtime_currbw_refreshes_lpf_without_resetting_output();
    test_runtime_currbw_rejects_nonfinite_and_nonpositive_values();
    test_runtime_diag_writes_are_rejected_while_running();
    test_runtime_diag_writes_remain_available_in_idle();
    test_runtime_bench_maps_are_idle_only_and_do_not_change_product_state();
    test_runtime_bench_adc_trigger_is_bounded_and_idle_only();
    test_runtime_fault_clear_clears_latched_context();
    test_runtime_disable_freezes_encoder_trace_until_fault_clear();
    test_runtime_bench_keepalive_is_running_only_and_observable();
    test_runtime_start_requires_valid_fresh_adc_calibration();
    test_runtime_attach_and_current_binding();
    test_runtime_snapshot_returns_false_and_zeros_when_unattached();
    test_runtime_snapshot_returns_false_for_null_output();
    test_runtime_snapshot_captures_live_shell_fields();
    test_runtime_snapshot_refreshes_output_motion_from_encoder_state();
    puts("[test_motor_runtime_contract] passed");
    return 0;
}
