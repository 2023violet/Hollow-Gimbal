/**
 * @file    test_app_shell_burst_contract.c
 * @brief   Host contract tests for FalconFoc shell burst output budget
 * @version v1.0.0
 * @date    2026.05.20
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_task.h"
#include "foc_common.h"
#include "foc_motion_runtime.h"
#include "foc_param.h"
#include "host_rtos.h"
#include "motor_runtime.h"
#include "motor_fsm.h"
#include "motor_calibration_runtime.h"
#include "motor_task.h"
#include "tm_shell.h"

#define TEST_ASSERT(expr)                                                                \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            fprintf(stderr,                                                              \
                    "[test_app_shell_burst_contract] assertion failed: %s (%s:%d)\n",   \
                    #expr,                                                               \
                    __FILE__,                                                            \
                    __LINE__);                                                           \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

struct motor_dev _motor_ins;
static struct motor_dev g_runtime_motor;

static const struct shell_cmd_entry *g_shell_begin = NULL;
static const struct shell_cmd_entry *g_shell_end   = NULL;
static char                          g_output[4096];
static size_t                        g_output_len          = 0U;
static size_t                        g_print_frame_count   = 0U;
static size_t                        g_print_payload_total = 0U;
static size_t                        g_prompt_payload      = 0U;
static bool                          g_reset_called        = false;

static const struct shell_cmd_entry g_shell_entries[] = {
    {
        .magic    = SHELL_CMD_MAGIC,
        .priority = 10U,
        .name     = "sys",
        .func     = app_shell_sys_command,
        .help     = "[status|tasks|reboot]",
        .category = "System",
    },
    {
        .magic    = SHELL_CMD_MAGIC,
        .priority = 20U,
        .name     = "state",
        .func     = app_shell_state_command,
        .help     = "[show|start|stop|disable]",
        .category = "Runtime",
    },
    {
        .magic    = SHELL_CMD_MAGIC,
        .priority = 30U,
        .name     = "obs",
        .func     = app_shell_obs_command,
        .help     = "[all|iq|id|vel|pos|vbus|temp|enc]",
        .category = "Observe",
    },
    {
        .magic    = SHELL_CMD_MAGIC,
        .priority = 40U,
        .name     = "fault",
        .func     = app_shell_fault_command,
        .help     = "[status|clear]",
        .category = "Fault",
    },
    {
        .magic    = SHELL_CMD_MAGIC,
        .priority = 50U,
        .name     = "store",
        .func     = app_shell_store_command,
        .help     = "[status|save <slot>|load <slot>|erase]",
        .category = "Store",
    },
    {
        .magic    = SHELL_CMD_MAGIC,
        .priority = 60U,
        .name     = "cal",
        .func     = app_shell_cal_command,
        .help     = "[status|enc|motor <all|rs|ls|flux|js>|abort]",
        .category = "Calib",
    },
    {
        .magic    = SHELL_CMD_MAGIC,
        .priority = 70U,
        .name     = "cfg",
        .func     = app_shell_cfg_command,
        .help     = "[show|ctrl <mode>|input <mode>]",
        .category = "Config",
    },
    {
        .magic    = SHELL_CMD_MAGIC,
        .priority = 80U,
        .name     = "diag",
        .func     = app_shell_diag_command,
        .help     = "set|ramp|pid|param",
        .category = "Diag",
    },
};

const struct shell_cmd_entry *themis_host_shell_cmd_begin(void)
{
    return g_shell_begin;
}

const struct shell_cmd_entry *themis_host_shell_cmd_end(void)
{
    return g_shell_end;
}

static void shell_table_select(const struct shell_cmd_entry *entries, size_t count)
{
    g_shell_begin = entries;
    g_shell_end   = entries + count;
}

static void shell_capture_reset(void)
{
    memset(g_output, 0, sizeof(g_output));
    g_output_len          = 0U;
    g_print_frame_count   = 0U;
    g_print_payload_total = 0U;
    g_prompt_payload      = 0U;
}

static void shell_capture(const char *str, uint16_t len)
{
    TEST_ASSERT(str != NULL);
    TEST_ASSERT((g_output_len + len) < sizeof(g_output));

    memcpy(&g_output[g_output_len], str, len);
    g_output_len += len;
    g_output[g_output_len] = '\0';

    g_print_frame_count++;
    if ((len == (uint16_t)strlen(SHELL_PROMPT)) && (strncmp(str, SHELL_PROMPT, len) == 0)) {
        g_prompt_payload += len;
    } else {
        g_print_payload_total += len;
    }
}

static void test_env_reset(void)
{
    static struct encoder_config enc_cfg = {
        .mech_offset = 0.456f,
        .pole_pairs  = 7U,
        .dir         = 1U,
    };

    memset(&_motor_ins, 0, sizeof(_motor_ins));
    memset(&g_runtime_motor, 0, sizeof(g_runtime_motor));
    motor_runtime_attach(NULL);
    host_rtos_reset();
    shell_table_select(g_shell_entries, sizeof(g_shell_entries) / sizeof(g_shell_entries[0]));
    shell_capture_reset();
    shell_init();
    shell_set_print(shell_capture);
    g_reset_called = false;

    _motor_ins.axis_state                    = AXIS_STATE_IDLE;
    _motor_ins.ctrl_mode                     = CTRL_MODE_VEL;
    _motor_ins.input_mode                    = INPUT_MODE_TRAP_TRAJ;
    _motor_ins.sensor_type                   = SENSOR_TYPE_MAGNETIC;
    _motor_ins.fault_code                    = 0U;
    _motor_ins.meas_data.v_bus               = 24.5f;
    _motor_ins.meas_data.out_vel_rpm         = 123.45f;
    _motor_ins.meas_data.out_pos_turns       = 6.789f;
    _motor_ins.meas_data.motor_temp          = 36.5f;
    _motor_ins.meas_data.driver_temp         = 34.0f;
    _motor_ins.foc_calc.i_q                  = 1.234f;
    _motor_ins.foc_calc.i_d                  = -0.321f;
    _motor_ins.enc_pri.state.calib_valid     = true;
    _motor_ins.enc_pri.state.angle_mech_rad  = 0.123f;
    _motor_ins.enc_pri.state.angle_elec_rad  = 1.234f;
    _motor_ins.enc_pri.state.velocity_rads   = 12.345f;
    _motor_ins.enc_pri.config                = &enc_cfg;
    _motor_ins.hw_config.poles               = 7.0f;
    _motor_ins.hw_config.rs                  = 0.2301f;
    _motor_ins.hw_config.ls                  = 0.0012f;
    _motor_ins.hw_config.flux                = 0.021f;
    _motor_ins.hw_config.js                  = 0.002f;
    _motor_ins.hw_config.gear_ratio          = 1.500f;
    _motor_ins.prot_config.v_bus_ov_thr      = 28.0f;
    _motor_ins.prot_config.v_bus_uv_thr      = 18.0f;
    _motor_ins.prot_config.i_peak            = 8.5f;
    _motor_ins.prot_config.vel_over_thr      = 31.4159f;
    _motor_ins.ctrl_config.curr_bandwidth    = 1500.0f;
    _motor_ins.ctrl_config.vel_bandwidth     = 180.0f;
    _motor_ins.ctrl_config.pos_bandwidth     = 25.0f;
    _motor_ins.ctrl_config.id_pid.config.kp  = 1.1f;
    _motor_ins.ctrl_config.id_pid.config.ki  = 2.2f;
    _motor_ins.ctrl_config.id_pid.config.kd  = 0.0f;
    _motor_ins.ctrl_config.id_pid.config.max_output = 12.0f;
    _motor_ins.ctrl_config.iq_pid.config     = _motor_ins.ctrl_config.id_pid.config;
    _motor_ins.ctrl_config.vel_pid.config    = _motor_ins.ctrl_config.id_pid.config;
    _motor_ins.ctrl_config.pos_pid.config    = _motor_ins.ctrl_config.id_pid.config;

    g_runtime_motor.axis_state                   = AXIS_STATE_RUNNING;
    g_runtime_motor.ctrl_mode                    = CTRL_MODE_POS;
    g_runtime_motor.input_mode                   = INPUT_MODE_TRAP_TRAJ;
    g_runtime_motor.sensor_type                  = SENSOR_TYPE_MAGNETIC;
    g_runtime_motor.fault_code                   = 0x00000005U;
    g_runtime_motor.meas_data.v_bus              = 48.5f;
    g_runtime_motor.meas_data.out_vel_rpm        = 222.25f;
    g_runtime_motor.meas_data.out_pos_turns      = 9.875f;
    g_runtime_motor.meas_data.motor_temp         = 41.5f;
    g_runtime_motor.meas_data.driver_temp        = 39.0f;
    g_runtime_motor.foc_calc.i_q                 = 2.500f;
    g_runtime_motor.foc_calc.i_d                 = -1.125f;
    g_runtime_motor.enc_pri.config               = &enc_cfg;
    g_runtime_motor.enc_pri.state.calib_valid    = true;
    g_runtime_motor.enc_pri.state.angle_mech_rad = 0.456f;
    g_runtime_motor.enc_pri.state.angle_elec_rad = 2.468f;
    g_runtime_motor.enc_pri.state.velocity_rads  = 23.456f;
    motor_runtime_attach(&g_runtime_motor);
}

void motor_fsm_dispatch_calib(struct motor_dev *motor, enum foc_calib_state calib_type)
{
    TEST_ASSERT(motor != NULL);
    motor->calib_state.state = calib_type;
    motor->axis_state        = AXIS_STATE_CALIB;
}

void foc_cmd_voltage_set(struct motor_dev *motor, float vd_set, float vq_set, float we_ramp)
{
    TEST_ASSERT(motor != NULL);
    if (motor->ctrl_mode != CTRL_MODE_VOLT) {
        motor->ctrl_state.theta_sim = motor->meas_data.theta_elec;
    }
    motor->ctrl_mode              = CTRL_MODE_VOLT;
    motor->input_mode             = INPUT_MODE_INACTIVE;
    motor->ctrl_state.vd_set      = vd_set;
    motor->ctrl_state.vq_set      = vq_set;
    motor->ctrl_state.we_volt_ramp = we_ramp;
}

int bsp_system_reset(void)
{
    g_reset_called = true;
    return 0;
}

bool tm_shell_task_diag_snapshot(struct tm_shell_task_diag_snapshot *snapshot)
{
    TEST_ASSERT(snapshot != NULL);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->initialized       = true;
    snapshot->transport_ready   = true;
    snapshot->ready             = true;
    snapshot->queue_depth       = 3U;
    snapshot->queue_used        = 0U;
    snapshot->queue_high_water  = 1U;
    snapshot->submit_ok_count   = 5U;
    snapshot->submit_fail_count = 0U;
    return true;
}

static void test_burst_budget(const char *line, const char *expected_token)
{
    size_t reserved_bytes;

    test_env_reset();
    TEST_ASSERT(SHELL_TX_RING_SIZE == 768U);
    shell_process_line(line, (uint16_t)strlen(line));

    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);
    TEST_ASSERT((expected_token == NULL) || (strstr(g_output, expected_token) != NULL));

    reserved_bytes = g_print_payload_total + (2U * g_print_frame_count) + g_prompt_payload;
    TEST_ASSERT(reserved_bytes <= SHELL_TX_RING_SIZE);
}

static void test_obs_snapshot_unavailable_reports_error(void)
{
    size_t reserved_bytes;

    test_env_reset();
    motor_runtime_attach(NULL);
    shell_process_line("obs iq", (uint16_t)strlen("obs iq"));

    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);
    TEST_ASSERT(strstr(g_output, "cmd=iq result=error reason=snapshot_unavailable") != NULL);

    reserved_bytes = g_print_payload_total + (2U * g_print_frame_count) + g_prompt_payload;
    TEST_ASSERT(reserved_bytes <= SHELL_TX_RING_SIZE);
}

static void test_state_snapshot_unavailable_reports_error(void)
{
    size_t reserved_bytes;

    test_env_reset();
    motor_runtime_attach(NULL);
    shell_process_line("state show", (uint16_t)strlen("state show"));

    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);
    TEST_ASSERT(strstr(g_output, "cmd=show result=error reason=snapshot_unavailable") != NULL);

    reserved_bytes = g_print_payload_total + (2U * g_print_frame_count) + g_prompt_payload;
    TEST_ASSERT(reserved_bytes <= SHELL_TX_RING_SIZE);
}

static void test_fault_snapshot_unavailable_reports_error(void)
{
    size_t reserved_bytes;

    test_env_reset();
    motor_runtime_attach(NULL);
    shell_process_line("fault status", (uint16_t)strlen("fault status"));

    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);
    TEST_ASSERT(strstr(g_output, "cmd=status result=error reason=snapshot_unavailable") != NULL);

    reserved_bytes = g_print_payload_total + (2U * g_print_frame_count) + g_prompt_payload;
    TEST_ASSERT(reserved_bytes <= SHELL_TX_RING_SIZE);
}

static void test_sys_status_uses_expanded_shell_counters(void)
{
    size_t reserved_bytes;

    test_env_reset();
    shell_process_line("sys status", (uint16_t)strlen("sys status"));

    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);
    TEST_ASSERT(strstr(g_output, "cmd=status result=ok version=") != NULL);
    TEST_ASSERT(strstr(g_output, "shell=on") != NULL);
    TEST_ASSERT(strstr(g_output, "queue_used=0 queue_depth=3 submit_ok_count=5 submit_fail_count=0") != NULL);

    reserved_bytes = g_print_payload_total + (2U * g_print_frame_count) + g_prompt_payload;
    TEST_ASSERT(reserved_bytes <= SHELL_TX_RING_SIZE);
}

static void test_store_save_sys_fits_burst_budget(void)
{
    size_t reserved_bytes;

    test_env_reset();
    g_runtime_motor.axis_state = AXIS_STATE_IDLE;
    shell_process_line("store save sys", (uint16_t)strlen("store save sys"));

    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);
    TEST_ASSERT(strstr(g_output, "cmd=save result=ok target=sys mode=deferred") != NULL);
    TEST_ASSERT(strstr(g_output, "total_data=") != NULL);
    TEST_ASSERT(strstr(g_output, "total_flash=") != NULL);
    TEST_ASSERT(strstr(g_output, "write='queued; safe-task will commit flash later'") != NULL);

    reserved_bytes = g_print_payload_total + (2U * g_print_frame_count) + g_prompt_payload;
    TEST_ASSERT(reserved_bytes <= SHELL_TX_RING_SIZE);
}

int main(void)
{
    test_burst_budget("help", "[help] count=9");
    test_burst_budget("state show", "cmd=show result=ok");
    test_burst_budget("obs all", "cmd=all result=ok view=all");
    test_burst_budget("obs iq", "cmd=iq result=ok value=2.5 unit=A");
    test_burst_budget("obs enc", "cmd=enc result=ok");
    test_burst_budget("fault status", "cmd=status result=ok active=1 mask=0x00000005");
    test_burst_budget("diag pid all show", "cmd=pid action=show result=ok loop=all");
    test_burst_budget("diag param show", "cmd=param action=show result=ok");
    test_burst_budget("store status", "cmd=status result=ok");
    test_store_save_sys_fits_burst_budget();
    test_obs_snapshot_unavailable_reports_error();
    test_state_snapshot_unavailable_reports_error();
    test_fault_snapshot_unavailable_reports_error();
    test_sys_status_uses_expanded_shell_counters();

    puts("[test_app_shell_burst_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
