/**
 * @file    app_shell_cmd_diag.c
 * @brief   FalconFoc product shell `diag` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_output_helpers.h"
#include "app_shell_profile.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "motor_fault_runtime.h"
#include "../periph/motor_bench_mapping.h"
#include "motor_runtime.h"
#include "tm_shell_output.h"

#if APP_SHELL_ENABLED && APP_SHELL_HAS_DIAG_CMDS

/* Private Helpers ------------------------------------------------------------------------------ */

static bool _app_shell_diag_is_pid_loop(const char *name)
{
    return (name != NULL) && ((strcmp(name, "id") == 0) || (strcmp(name, "iq") == 0) ||
                              (strcmp(name, "vel") == 0) || (strcmp(name, "pos") == 0) ||
                              (strcmp(name, "vvel") == 0));
}

static bool _app_shell_diag_is_pid_field(const char *name)
{
    return (name != NULL) && ((strcmp(name, "kp") == 0) || (strcmp(name, "ki") == 0) ||
                              (strcmp(name, "kd") == 0) || (strcmp(name, "lim") == 0));
}

static bool _app_shell_diag_is_param_name(const char *name)
{
    return (name != NULL) &&
           ((strcmp(name, "rs") == 0) || (strcmp(name, "ls") == 0) || (strcmp(name, "flux") == 0) ||
            (strcmp(name, "poles") == 0) || (strcmp(name, "js") == 0) ||
            (strcmp(name, "gear") == 0) || (strcmp(name, "vov") == 0) ||
            (strcmp(name, "vuv") == 0) || (strcmp(name, "imax") == 0) ||
            (strcmp(name, "velmax") == 0) || (strcmp(name, "currbw") == 0) ||
            (strcmp(name, "velbw") == 0) || (strcmp(name, "posbw") == 0) ||
            (strcmp(name, "encoff") == 0));
}

static bool _app_shell_diag_param_requires_positive(const char *name)
{
    return (name != NULL) &&
           ((strcmp(name, "currbw") == 0) || (strcmp(name, "velbw") == 0) || (strcmp(name, "posbw") == 0));
}

static void _app_shell_diag_emit_pid_error(const char *action,
                                           const char *reason,
                                           const char *loop,
                                           const char *field,
                                           const char *axis)
{
    if ((field != NULL) && (axis != NULL)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=pid action=%s result=error axis=%s reason=%s loop=%s field=%s",
                         action,
                         axis,
                         reason,
                         loop,
                         field);
        return;
    }

    if (field != NULL) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=pid action=%s result=error reason=%s loop=%s field=%s",
                         action,
                         reason,
                         loop,
                         field);
        return;
    }

    if (axis != NULL) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=pid action=%s result=error axis=%s reason=%s loop=%s",
                         action,
                         axis,
                         reason,
                         loop);
        return;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=pid action=%s result=error reason=%s loop=%s",
                     action,
                     reason,
                     loop);
}

static void _app_shell_diag_emit_param_error(const char *action,
                                             const char *reason,
                                             const char *name,
                                             const char *axis,
                                             float       value,
                                             bool        include_value)
{
    char buffer[16];

    if (include_value && (axis != NULL)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=param action=%s result=error axis=%s reason=%s name=%s value=%s unit=raw",
                         action,
                         axis,
                         reason,
                         name,
                         app_shell_format_float_or_dash(buffer, sizeof(buffer), value, 4U));
        return;
    }

    if (include_value) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=param action=%s result=error reason=%s name=%s value=%s unit=raw",
                         action,
                         reason,
                         name,
                         app_shell_format_float_or_dash(buffer, sizeof(buffer), value, 4U));
        return;
    }

    if (axis != NULL) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=param action=%s result=error axis=%s reason=%s name=%s",
                         action,
                         axis,
                         reason,
                         name);
        return;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=param action=%s result=error reason=%s name=%s",
                     action,
                     reason,
                     name);
}

static bool _app_shell_diag_reject_pid_write_if_not_idle(const char *action,
                                                         const char *loop,
                                                         const char *field)
{
    struct motor_runtime_snapshot snapshot;

    if (!motor_runtime_snapshot(&snapshot)) {
        _app_shell_diag_emit_pid_error(action, "snapshot_unavailable", loop, field, NULL);
        return true;
    }

    if (snapshot.axis_state == AXIS_STATE_IDLE) {
        return false;
    }

    _app_shell_diag_emit_pid_error(action,
                                   "idle_only",
                                   loop,
                                   field,
                                   app_shell_axis_state_name(snapshot.axis_state));
    return true;
}

static bool _app_shell_diag_reject_param_write_if_not_idle(const char *action, const char *name, float value)
{
    struct motor_runtime_snapshot snapshot;

    if (!motor_runtime_snapshot(&snapshot)) {
        _app_shell_diag_emit_param_error(action, "snapshot_unavailable", name, NULL, value, false);
        return true;
    }

    if (snapshot.axis_state == AXIS_STATE_IDLE) {
        return false;
    }

    _app_shell_diag_emit_param_error(action,
                                     "idle_only",
                                     name,
                                     app_shell_axis_state_name(snapshot.axis_state),
                                     value,
                                     false);
    return true;
}

static bool _app_shell_diag_reject_motion_write_if_not_idle(const char *field)
{
    struct motor_runtime_snapshot snapshot;

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=set result=error reason=snapshot_unavailable field=%s",
                         field);
        return true;
    }

    if (snapshot.axis_state == AXIS_STATE_IDLE) {
        return false;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=set result=error reason=idle_only axis=%s field=%s",
                     app_shell_axis_state_name(snapshot.axis_state),
                     field);
    return true;
}

static void _app_shell_diag_param_show(void)
{
    struct motor_runtime_diag_param_view view;
    char rs[16];
    char ls[16];
    char flux[16];
    char js[16];
    char gear[16];
    char vov[16];
    char vuv[16];
    char imax[16];
    char velmax[16];
    char currbw[16];
    char velbw[16];
    char posbw[16];

    if (!motor_runtime_diag_read_params(&view)) {
        app_shell_emit_snapshot_unavailable("diag", "param", "show");
        return;
    }

    TM_SHELL_SUMMARY("diag", "cmd=param action=show result=ok");
    TM_SHELL_ROW("motor",
                 "poles=%u rs=%s ls=%s flux=%s js=%s",
                 (unsigned)view.poles,
                 app_shell_format_float_or_dash(rs, sizeof(rs), view.rs, 4U),
                 app_shell_format_float_or_dash(ls, sizeof(ls), view.ls, 4U),
                 app_shell_format_float_or_dash(flux, sizeof(flux), view.flux, 4U),
                 app_shell_format_float_or_dash(js, sizeof(js), view.js, 4U));
    TM_SHELL_ROW("gear",
                 "ratio=%s",
                 app_shell_format_float_or_dash(gear, sizeof(gear), view.gear_ratio, 3U));
    TM_SHELL_ROW("prot",
                 "vov=%s vuv=%s imax=%s velmax_rpm=%s",
                 app_shell_format_float_or_dash(vov, sizeof(vov), view.v_bus_ov_thr, 2U),
                 app_shell_format_float_or_dash(vuv, sizeof(vuv), view.v_bus_uv_thr, 2U),
                 app_shell_format_float_or_dash(imax, sizeof(imax), view.i_peak, 2U),
                 app_shell_format_float_or_dash(velmax, sizeof(velmax), view.vel_over_rpm, 2U));
    TM_SHELL_ROW("band",
                 "curr=%s vel=%s pos=%s",
                 app_shell_format_float_or_dash(currbw, sizeof(currbw), view.curr_bandwidth, 1U),
                 app_shell_format_float_or_dash(velbw, sizeof(velbw), view.vel_bandwidth, 1U),
                 app_shell_format_float_or_dash(posbw, sizeof(posbw), view.pos_bandwidth, 1U));
}

#if (TM_BENCH_DIAG != 0U)
static bool _app_shell_diag_parse_u16(const char *text, uint16_t *value_out)
{
    char          *end = NULL;
    unsigned long  value;

    if ((text == NULL) || (text[0] == '\0') || (text[0] == '-') || (text[0] == '+') ||
        (value_out == NULL)) {
        return false;
    }

    value = strtoul(text, &end, 10);
    if ((end == text) || (*end != '\0') || (value > 65535UL)) {
        return false;
    }

    *value_out = (uint16_t)value;
    return true;
}

static bool _app_shell_diag_trace_is_stopped(uint8_t axis_state)
{
    return (axis_state != AXIS_STATE_RUNNING) && (axis_state != AXIS_STATE_CALIB);
}

static bool _app_shell_diag_trace_parse_index(const char *text, uint32_t *index_out)
{
    char          *end = NULL;
    unsigned long  value;

    if ((text == NULL) || (index_out == NULL)) {
        return false;
    }

    value = strtoul(text, &end, 10);
    if ((end == text) || (*end != '\0') || (value >= MOTOR_BENCH_SAMPLE_RING_DEPTH)) {
        return false;
    }

    *index_out = (uint32_t)value;
    return true;
}

static void _app_shell_diag_trace_emit_current(const struct motor_bench_trace *trace)
{
    char ia_peak[16];
    char ib_peak[16];
    char ic_peak[16];
    char vbus_min[16];
    char vbus_max[16];

    TM_SHELL_ROW("trace",
                 "peak=%s/%s/%s vbus=%s/%s",
                 app_shell_format_float_or_dash(ia_peak, sizeof(ia_peak), trace->ia_peak_abs, 3U),
                 app_shell_format_float_or_dash(ib_peak, sizeof(ib_peak), trace->ib_peak_abs, 3U),
                 app_shell_format_float_or_dash(ic_peak, sizeof(ic_peak), trace->ic_peak_abs, 3U),
                 app_shell_format_float_or_dash(vbus_min, sizeof(vbus_min), trace->v_bus_min, 2U),
                 app_shell_format_float_or_dash(vbus_max, sizeof(vbus_max), trace->v_bus_max, 2U));
}

static void _app_shell_diag_trace_emit_angle(const struct motor_bench_trace *trace)
{
    char theta_sim_first[16];
    char theta_sim_last[16];
    char theta_elec_first[16];
    char theta_elec_last[16];

    TM_SHELL_ROW("angle",
                 "sim=%s/%s elec=%s/%s",
                 app_shell_format_float_or_dash(theta_sim_first,
                                                sizeof(theta_sim_first),
                                                trace->theta_sim_first,
                                                3U),
                 app_shell_format_float_or_dash(theta_sim_last,
                                                sizeof(theta_sim_last),
                                                trace->theta_sim_last,
                                                3U),
                 app_shell_format_float_or_dash(theta_elec_first,
                                                sizeof(theta_elec_first),
                                                trace->theta_elec_first,
                                                3U),
                 app_shell_format_float_or_dash(theta_elec_last,
                                                sizeof(theta_elec_last),
                                                trace->theta_elec_last,
                                                3U));
}

static void _app_shell_diag_trace_emit_speed(const struct motor_bench_trace *trace)
{
    char we_now_first[16];
    char we_now_last[16];
    char we_ramp_first[16];
    char we_ramp_last[16];

    TM_SHELL_ROW("speed",
                 "now=%s/%s ramp=%s/%s",
                 app_shell_format_float_or_dash(we_now_first,
                                                sizeof(we_now_first),
                                                trace->we_volt_now_first,
                                                3U),
                 app_shell_format_float_or_dash(we_now_last,
                                                sizeof(we_now_last),
                                                trace->we_volt_now_last,
                                                3U),
                 app_shell_format_float_or_dash(we_ramp_first,
                                                sizeof(we_ramp_first),
                                                trace->we_volt_ramp_first,
                                                3U),
                 app_shell_format_float_or_dash(we_ramp_last,
                                                sizeof(we_ramp_last),
                                                trace->we_volt_ramp_last,
                                                3U));
}

static void _app_shell_diag_trace_emit_foc(const struct motor_bench_trace *trace)
{
    char vd_first[16];
    char vd_last[16];
    char vq_first[16];
    char vq_last[16];

    TM_SHELL_ROW("foc",
                 "vd=%s/%s vq=%s/%s",
                 app_shell_format_float_or_dash(vd_first, sizeof(vd_first), trace->vd_first, 3U),
                 app_shell_format_float_or_dash(vd_last, sizeof(vd_last), trace->vd_last, 3U),
                 app_shell_format_float_or_dash(vq_first, sizeof(vq_first), trace->vq_first, 3U),
                 app_shell_format_float_or_dash(vq_last, sizeof(vq_last), trace->vq_last, 3U));
}

static void _app_shell_diag_trace_emit_duty(const struct motor_bench_trace *trace)
{
    char dtc_a_min[16];
    char dtc_a_max[16];
    char dtc_b_min[16];
    char dtc_b_max[16];
    char dtc_c_min[16];
    char dtc_c_max[16];

    TM_SHELL_ROW("duty",
                 "a=%s-%s b=%s-%s c=%s-%s",
                 app_shell_format_float_or_dash(dtc_a_min, sizeof(dtc_a_min), trace->dtc_a_min, 3U),
                 app_shell_format_float_or_dash(dtc_a_max, sizeof(dtc_a_max), trace->dtc_a_max, 3U),
                 app_shell_format_float_or_dash(dtc_b_min, sizeof(dtc_b_min), trace->dtc_b_min, 3U),
                 app_shell_format_float_or_dash(dtc_b_max, sizeof(dtc_b_max), trace->dtc_b_max, 3U),
                 app_shell_format_float_or_dash(dtc_c_min, sizeof(dtc_c_min), trace->dtc_c_min, 3U),
                 app_shell_format_float_or_dash(dtc_c_max, sizeof(dtc_c_max), trace->dtc_c_max, 3U));
}

static int _app_shell_diag_trace_show(void)
{
    struct motor_bench_trace trace;

    if (!motor_bench_trace_read(&trace)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=trace action=show result=ok valid=0 retained=0");
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=trace action=show result=ok valid=%u sequence=%lu samples=%lu retained=%lu",
                     (unsigned)trace.valid,
                     (unsigned long)trace.sequence,
                     (unsigned long)trace.sample_count,
                     (unsigned long)motor_bench_trace_sample_count());
    TM_SHELL_ROW("trace",
                 "sector=%u/%u raw=%u-%u/%u-%u/%u-%u",
                 (unsigned)trace.first_sector,
                 (unsigned)trace.last_sector,
                 (unsigned)trace.ia_raw_min,
                 (unsigned)trace.ia_raw_max,
                 (unsigned)trace.ib_raw_min,
                 (unsigned)trace.ib_raw_max,
                 (unsigned)trace.ic_raw_min,
                 (unsigned)trace.ic_raw_max);
    _app_shell_diag_trace_emit_current(&trace);
    _app_shell_diag_trace_emit_angle(&trace);
    _app_shell_diag_trace_emit_speed(&trace);
    _app_shell_diag_trace_emit_foc(&trace);
    _app_shell_diag_trace_emit_duty(&trace);
    return SHELL_OK;
}

static int _app_shell_diag_trace_sample_show(uint32_t index)
{
    struct motor_bench_sample sample;

    if (!motor_bench_trace_sample_read(index, &sample)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=trace action=sample result=error reason=bad_index index=%lu",
                         (unsigned long)index);
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=trace action=sample result=ok index=%lu sequence=%lu axis=%s sector=%u fault=0x%08x",
                     (unsigned long)index,
                     (unsigned long)sample.sequence,
                     app_shell_axis_state_name(sample.axis_state),
                     (unsigned)sample.sector,
                     (unsigned)sample.fault_code);
    TM_SHELL_ROW("tmr1",
                 "tmr1_cval=%lu pr=%lu ccr=%lu/%lu/%lu/%lu cctrl=%lu brk=%lu ists=%lu dir=%u",
                 (unsigned long)sample.tmr1_cval,
                 (unsigned long)sample.tmr1_pr,
                 (unsigned long)sample.tmr1_ccr1,
                 (unsigned long)sample.tmr1_ccr2,
                 (unsigned long)sample.tmr1_ccr3,
                 (unsigned long)sample.tmr1_ccr4,
                 (unsigned long)sample.tmr1_cctrl,
                 (unsigned long)sample.tmr1_brk,
                 (unsigned long)sample.tmr1_ists,
                 (unsigned)sample.tmr1_count_dir);
    TM_SHELL_ROW("adc",
                 "sts=%lu raw=%u/%u/%u/%u",
                 (unsigned long)sample.adc_sts,
                 (unsigned)sample.vbus_raw,
                 (unsigned)sample.ic_raw,
                 (unsigned)sample.ib_raw,
                 (unsigned)sample.ia_raw);
    TM_SHELL_ROW("phase",
                 "vbus_mv=%ld i_ma=%ld/%ld/%ld duty=%u/%u/%u",
                 (long)sample.vbus_mv,
                 (long)sample.ia_ma,
                 (long)sample.ib_ma,
                 (long)sample.ic_ma,
                 (unsigned)sample.duty_a_permille,
                 (unsigned)sample.duty_b_permille,
                 (unsigned)sample.duty_c_permille);
    TM_SHELL_ROW("foc",
                 "theta_mrad=%ld/%ld we_mrad_s=%ld/%ld vd_mv=%ld vq_mv=%ld flags=0x%08x",
                 (long)sample.theta_sim_mrad,
                 (long)sample.theta_elec_mrad,
                 (long)sample.we_now_mrad_s,
                 (long)sample.we_ramp_mrad_s,
                 (long)sample.vd_mv,
                 (long)sample.vq_mv,
                 (unsigned)sample.flags);
    return SHELL_OK;
}

static int _app_shell_diag_handle_trace(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;
    uint32_t                      index;

    if ((argc != 3) && (argc != 4)) {
        return SHELL_ERR_ARGC;
    }
    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=trace action=%s result=error reason=snapshot_unavailable",
                         argv[2]);
        return SHELL_OK;
    }

    if (strcmp(argv[2], "clear") == 0) {
        if (snapshot.axis_state != AXIS_STATE_IDLE) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=trace action=clear result=error reason=idle_only axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }
        motor_bench_trace_sample_clear();
        TM_SHELL_SUMMARY("diag", "cmd=trace action=clear result=ok");
        return SHELL_OK;
    }

    if (strcmp(argv[2], "show") == 0) {
        if (argc != 3) {
            return SHELL_ERR_ARGC;
        }
        if (!_app_shell_diag_trace_is_stopped(snapshot.axis_state)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=trace action=show result=error reason=stopped_only axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }

        return _app_shell_diag_trace_show();
    }

    if (strcmp(argv[2], "sample") == 0) {
        if (argc != 4) {
            return SHELL_ERR_ARGC;
        }
        if (!_app_shell_diag_trace_is_stopped(snapshot.axis_state)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=trace action=sample result=error reason=stopped_only axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }
        if (!_app_shell_diag_trace_parse_index(argv[3], &index)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=trace action=sample result=error reason=bad_index index=%s",
                             argv[3]);
            return SHELL_OK;
        }

        return _app_shell_diag_trace_sample_show(index);
    }

    return SHELL_ERR_ARG;
}

static int _app_shell_diag_handle_map(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;
    uint16_t                      map_id;
    uint8_t                       pwm_map;
    uint8_t                       adc_map;
    uint16_t                      adc_trigger;

    if ((argc < 3) || (argc > 4)) {
        return SHELL_ERR_ARGC;
    }
    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=map action=%s result=error reason=snapshot_unavailable",
                         argv[2]);
        return SHELL_OK;
    }

    if (strcmp(argv[2], "show") == 0) {
        if (argc != 3) {
            return SHELL_ERR_ARGC;
        }
        if (!_app_shell_diag_trace_is_stopped(snapshot.axis_state)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=map action=show result=error reason=stopped_only axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }
        if (!motor_runtime_diag_read_bench_maps(&pwm_map, &adc_map, &adc_trigger)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=map action=show result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }
        TM_SHELL_SUMMARY("diag",
                         "cmd=map action=show result=ok pwm=%u adc=%u adc_trig=%u",
                         (unsigned)pwm_map,
                         (unsigned)adc_map,
                         (unsigned)adc_trigger);
        return SHELL_OK;
    }

    if (strcmp(argv[2], "reset") == 0) {
        if (argc != 3) {
            return SHELL_ERR_ARGC;
        }
        if (snapshot.axis_state != AXIS_STATE_IDLE) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=map action=reset result=error reason=idle_only axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }
        motor_runtime_diag_reset_maps();
        TM_SHELL_SUMMARY("diag", "cmd=map action=reset result=ok");
        return SHELL_OK;
    }

    if (argc != 4) {
        return SHELL_ERR_ARGC;
    }
    if ((strcmp(argv[2], "pwm") != 0) && (strcmp(argv[2], "adc") != 0)) {
        return SHELL_ERR_ARG;
    }
    if (snapshot.axis_state != AXIS_STATE_IDLE) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=map action=set result=error reason=idle_only kind=%s axis=%s",
                         argv[2],
                         app_shell_axis_state_name(snapshot.axis_state));
        return SHELL_OK;
    }
    if (!_app_shell_diag_parse_u16(argv[3], &map_id) ||
        (map_id >= MOTOR_BENCH_MAP_COUNT)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=map action=set result=error reason=invalid_value kind=%s map=%s",
                         argv[2],
                         argv[3]);
        return SHELL_OK;
    }

    if (strcmp(argv[2], "pwm") == 0) {
        if (!motor_runtime_diag_set_pwm_map((uint8_t)map_id)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=map action=set result=error reason=invalid_value kind=pwm map=%s",
                             argv[3]);
            return SHELL_OK;
        }
    } else {
        if (!motor_runtime_diag_set_adc_map((uint8_t)map_id)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=map action=set result=error reason=invalid_value kind=adc map=%s",
                             argv[3]);
            return SHELL_OK;
        }
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=map action=set result=ok kind=%s map=%u",
                     argv[2],
                     (unsigned)map_id);
    return SHELL_OK;
}

static int _app_shell_diag_handle_encoder_trace(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;
    struct encoder_bench_sample  sample;
    uint32_t                     index;
    uint32_t                     retained;

    if ((argc < 4) || (argc > 5)) {
        return SHELL_ERR_ARGC;
    }
    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=enc_trace action=%s result=error reason=snapshot_unavailable",
                         argv[3]);
        return SHELL_OK;
    }

    if (strcmp(argv[3], "clear") == 0) {
        if (argc != 4) {
            return SHELL_ERR_ARGC;
        }
        if (snapshot.axis_state != AXIS_STATE_IDLE) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=enc_trace action=clear result=error reason=idle_only axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }
        if (!motor_runtime_diag_encoder_trace_clear()) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=enc_trace action=clear result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }
        TM_SHELL_SUMMARY("diag", "cmd=enc_trace action=clear result=ok");
        return SHELL_OK;
    }

    if (strcmp(argv[3], "show") == 0) {
        if (argc != 4) {
            return SHELL_ERR_ARGC;
        }
        if (!_app_shell_diag_trace_is_stopped(snapshot.axis_state)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=enc_trace action=show result=error reason=stopped_only axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }
        retained = motor_runtime_diag_encoder_trace_count();
        TM_SHELL_SUMMARY("diag",
                         "cmd=enc_trace action=show result=ok retained=%lu",
                         (unsigned long)retained);
        return SHELL_OK;
    }

    if (strcmp(argv[3], "sample") == 0) {
        char mech[16];
        char elec[16];
        char velocity[16];
        char total[16];

        if (argc != 5) {
            return SHELL_ERR_ARGC;
        }
        if (!_app_shell_diag_trace_is_stopped(snapshot.axis_state)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=enc_trace action=sample result=error reason=stopped_only axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }
        retained = motor_runtime_diag_encoder_trace_count();
        if (!_app_shell_diag_trace_parse_index(argv[4], &index) ||
            (index >= retained) ||
            !motor_runtime_diag_encoder_trace_read(index, &sample)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=enc_trace action=sample result=error reason=bad_index index=%s",
                             argv[4]);
            return SHELL_OK;
        }
        TM_SHELL_SUMMARY("diag",
                         "cmd=enc_trace action=sample result=ok index=%lu sequence=%lu",
                         (unsigned long)index,
                         (unsigned long)sample.sequence);
        TM_SHELL_ROW("enc",
                     "raw=%u corrected=%ld direction=%d error=0x%02x",
                     (unsigned)sample.raw_word,
                     (long)sample.corrected_count,
                     (int)sample.direction,
                     (unsigned)sample.error_flags);
        TM_SHELL_ROW("angle",
                     "mech=%s elec=%s vel=%s total=%s",
                     app_shell_format_float_or_dash(mech,
                                                    sizeof(mech),
                                                    sample.angle_mech_rad,
                                                    4U),
                     app_shell_format_float_or_dash(elec,
                                                    sizeof(elec),
                                                    sample.angle_elec_rad,
                                                    4U),
                     app_shell_format_float_or_dash(velocity,
                                                    sizeof(velocity),
                                                    sample.velocity_rads,
                                                    4U),
                     app_shell_format_float_or_dash(total,
                                                    sizeof(total),
                                                    sample.total_enc,
                                                    4U));
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}
#endif

static void _app_shell_diag_pid_row(const char *name, const struct motor_runtime_pid_view *view)
{
    char kp[16];
    char ki[16];
    char kd[16];
    char lim[16];

    TM_SHELL_ROW("pid",
                 "loop=%s kp=%s ki=%s kd=%s lim=%s",
                 name,
                 app_shell_format_float_or_dash(kp, sizeof(kp), view->kp, 4U),
                 app_shell_format_float_or_dash(ki, sizeof(ki), view->ki, 4U),
                 app_shell_format_float_or_dash(kd, sizeof(kd), view->kd, 4U),
                 app_shell_format_float_or_dash(lim, sizeof(lim), view->lim, 4U));
}

static int _app_shell_diag_handle_set(int argc, char *argv[])
{
    float value;
    float limit;

    if (argc < 4) {
        return SHELL_ERR_ARGC;
    }

#if (TM_BENCH_DIAG != 0U)
    if (strcmp(argv[2], "adc_trig") == 0) {
        struct motor_runtime_snapshot snapshot;
        uint16_t                      compare;

        if (argc != 4) {
            return SHELL_ERR_ARGC;
        }
        if (!motor_runtime_snapshot(&snapshot)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=set result=error reason=snapshot_unavailable field=adc_trig");
            return SHELL_OK;
        }
        if (snapshot.axis_state != AXIS_STATE_IDLE) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=set result=error reason=idle_only axis=%s field=adc_trig",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }
        if (!_app_shell_diag_parse_u16(argv[3], &compare) ||
            !motor_bench_adc_trigger_is_valid(compare) ||
            !motor_runtime_diag_set_adc_trigger(compare)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=set result=error reason=invalid_value field=adc_trig value=%s",
                             argv[3]);
            return SHELL_OK;
        }
        TM_SHELL_SUMMARY("diag",
                         "cmd=set result=ok field=adc_trig value=%u unit=timer",
                         (unsigned)compare);
        return SHELL_OK;
    }

    if (strcmp(argv[2], "enc_dir") == 0) {
        uint16_t dir;

        if (argc != 4) {
            return SHELL_ERR_ARGC;
        }
        if (_app_shell_diag_reject_motion_write_if_not_idle("enc_dir")) {
            return SHELL_OK;
        }
        if (!_app_shell_diag_parse_u16(argv[3], &dir) || (dir > (uint16_t)DIR_CCW) ||
            !motor_runtime_diag_set_encoder_dir((uint8_t)dir)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=set result=error reason=invalid_value field=enc_dir value=%s",
                             argv[3]);
            return SHELL_OK;
        }
        TM_SHELL_SUMMARY("diag",
                         "cmd=set result=ok field=enc_dir value=%u direction=%s",
                         (unsigned)dir,
                         (dir == (uint16_t)DIR_CW) ? "cw" : "ccw");
        return SHELL_OK;
    }
#endif

    if (app_shell_parse_float_arg(argv[3], &value) != 0) {
        return SHELL_ERR_ARG;
    }

    limit = 0.0f;
    if ((argc >= 5) && (app_shell_parse_float_arg(argv[4], &limit) != 0)) {
        return SHELL_ERR_ARG;
    }

#if (TM_BENCH_DIAG != 0U)
    if (strcmp(argv[2], "theta0") == 0) {
        if (_app_shell_diag_reject_motion_write_if_not_idle("theta0")) {
            return SHELL_OK;
        }
        if (!motor_runtime_diag_set_phase_seed_offset(value)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=set result=error reason=invalid_value field=theta0");
            return SHELL_OK;
        }
        {
            char offset[16];
            TM_SHELL_SUMMARY("diag",
                             "cmd=set result=ok field=theta0 unit=rad value=%s",
                             app_shell_format_float_or_dash(offset, sizeof(offset), value, 4U));
        }
        return SHELL_OK;
    }
#endif

    if (strcmp(argv[2], "vel") == 0) {
        if (_app_shell_diag_reject_motion_write_if_not_idle("vel")) {
            return SHELL_OK;
        }
        if (!motor_runtime_request_set_passthrough(CTRL_MODE_VEL, FOC_RPM_TO_RADS(value), limit)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "vel_rpm", value, 2U, "rpm");
    }
    if (strcmp(argv[2], "vvel") == 0) {
        if (_app_shell_diag_reject_motion_write_if_not_idle("vvel")) {
            return SHELL_OK;
        }
        if (!motor_runtime_request_set_passthrough(CTRL_MODE_VVEL, FOC_RPM_TO_RADS(value), 0.0f)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "vel_rpm", value, 2U, "rpm");
    }
    if (strcmp(argv[2], "pos") == 0) {
        if (_app_shell_diag_reject_motion_write_if_not_idle("pos")) {
            return SHELL_OK;
        }
        if (!motor_runtime_request_set_passthrough(
                CTRL_MODE_POS,
                value * FOC_2PI,
                (limit > 0.0f) ? FOC_RPM_TO_RADS(limit) : 0.0f)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "pos_turn", value, 3U, "turn");
    }
    if (strcmp(argv[2], "iq") == 0) {
        if (_app_shell_diag_reject_motion_write_if_not_idle("iq")) {
            return SHELL_OK;
        }
        if (!motor_runtime_request_set_passthrough(CTRL_MODE_CURR, value, 0.0f)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "iq", value, 3U, "A");
    }
    if (strcmp(argv[2], "vd") == 0) {
        if (_app_shell_diag_reject_motion_write_if_not_idle("vd")) {
            return SHELL_OK;
        }
        if (!motor_runtime_diag_set_voltage(value, 0.0f, limit)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "vd", value, 3U, "V");
    }
    if (strcmp(argv[2], "vq") == 0) {
        if (_app_shell_diag_reject_motion_write_if_not_idle("vq")) {
            return SHELL_OK;
        }
        if (!motor_runtime_diag_set_voltage(0.0f, value, limit)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "vq", value, 3U, "V");
    }
    if (strcmp(argv[2], "if") == 0) {
        if (_app_shell_diag_reject_motion_write_if_not_idle("if")) {
            return SHELL_OK;
        }
        if (!motor_runtime_diag_set_if(value, limit)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "if_iq", value, 3U, "A");
    }
    if ((strcmp(argv[2], "dp0") == 0) || (strcmp(argv[2], "dp1") == 0) ||
        (strcmp(argv[2], "dp2") == 0)) {
        int phase = argv[2][2] - '0';
        if (_app_shell_diag_reject_motion_write_if_not_idle(argv[2])) {
            return SHELL_OK;
        }
        if (!motor_runtime_diag_set_phase_volt(phase, value)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "dp_volt", value, 3U, "V");
    }

    return SHELL_ERR_ARG;
}

static int _app_shell_diag_handle_ramp(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;
    float value;
    float arg1;
    float arg2;
    float arg3;

    if (!motor_runtime_snapshot(&snapshot)) {
        app_shell_emit_snapshot_unavailable("diag", "ramp", NULL);
        return SHELL_OK;
    }

    if (argc < 4) {
        return SHELL_ERR_ARGC;
    }
    if (app_shell_parse_float_arg(argv[3], &value) != 0) {
        return SHELL_ERR_ARG;
    }

    arg1 = 0.0f;
    arg2 = 0.0f;
    arg3 = 0.0f;
    if ((argc >= 5) && (app_shell_parse_float_arg(argv[4], &arg1) != 0)) {
        return SHELL_ERR_ARG;
    }
    if ((argc >= 6) && (app_shell_parse_float_arg(argv[5], &arg2) != 0)) {
        return SHELL_ERR_ARG;
    }
    if ((argc >= 7) && (app_shell_parse_float_arg(argv[6], &arg3) != 0)) {
        return SHELL_ERR_ARG;
    }

    if (strcmp(argv[2], "vel") == 0) {
        if (!motor_runtime_request_vel_ramp(FOC_RPM_TO_RADS(value),
                                            (arg1 > 0.0f) ? FOC_RPM_TO_RADS(arg1) : 0.0f)) {
            app_shell_emit_snapshot_unavailable("diag", "ramp", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "ramp", NULL, "vel_rpm", value, 2U, "rpm");
    }
    if (strcmp(argv[2], "iq") == 0) {
        if (!motor_runtime_request_torque_ramp(value, arg1)) {
            app_shell_emit_snapshot_unavailable("diag", "ramp", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "ramp", NULL, "iq", value, 3U, "A");
    }
    if (strcmp(argv[2], "pos") == 0) {
        if (!motor_runtime_request_pos_traj(value * FOC_2PI,
                                            (arg1 > 0.0f) ? FOC_RPM_TO_RADS(arg1) : 0.0f,
                                            (arg2 > 0.0f) ? FOC_RPM_TO_RADS(arg2) : 0.0f,
                                            (arg3 > 0.0f) ? FOC_RPM_TO_RADS(arg3) : 0.0f)) {
            app_shell_emit_snapshot_unavailable("diag", "ramp", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "ramp", NULL, "pos_turn", value, 3U, "turn");
    }

    return SHELL_ERR_ARG;
}

static int _app_shell_diag_handle_pid(int argc, char *argv[])
{
    struct motor_runtime_pid_view pid_id_view;
    struct motor_runtime_pid_view pid_iq_view;
    struct motor_runtime_pid_view pid_vel_view;
    struct motor_runtime_pid_view pid_pos_view;
    struct motor_runtime_pid_view pid_vvel_view;
    struct motor_runtime_pid_view pid_view;
    char                         buffer[16];
    float                        value;

    if (argc < 4) {
        return SHELL_ERR_ARGC;
    }

    if (strcmp(argv[2], "all") == 0) {
        if ((argc != 4) || (strcmp(argv[3], "show") != 0)) {
            return SHELL_ERR_ARG;
        }

        if (!motor_runtime_diag_read_pid("id", &pid_id_view) || !motor_runtime_diag_read_pid("iq", &pid_iq_view) ||
            !motor_runtime_diag_read_pid("vel", &pid_vel_view) || !motor_runtime_diag_read_pid("pos", &pid_pos_view) ||
            !motor_runtime_diag_read_pid("vvel", &pid_vvel_view)) {
            app_shell_emit_snapshot_unavailable_named("diag", "pid", "show", "loop", "all");
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("diag", "cmd=pid action=show result=ok loop=all");
        _app_shell_diag_pid_row("id", &pid_id_view);
        _app_shell_diag_pid_row("iq", &pid_iq_view);
        _app_shell_diag_pid_row("vel", &pid_vel_view);
        _app_shell_diag_pid_row("pos", &pid_pos_view);
        _app_shell_diag_pid_row("vvel", &pid_vvel_view);
        return SHELL_OK;
    }

    if (strcmp(argv[3], "show") == 0) {
        if (!_app_shell_diag_is_pid_loop(argv[2])) {
            _app_shell_diag_emit_pid_error("show", "bad_loop", argv[2], NULL, NULL);
            return SHELL_OK;
        }

        if (!motor_runtime_diag_read_pid(argv[2], &pid_view)) {
            app_shell_emit_snapshot_unavailable_named("diag", "pid", "show", "loop", argv[2]);
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("diag", "cmd=pid action=show result=ok loop=%s", argv[2]);
        _app_shell_diag_pid_row(argv[2], &pid_view);
        return SHELL_OK;
    }

    if (strcmp(argv[3], "reset") == 0) {
        if (!_app_shell_diag_is_pid_loop(argv[2])) {
            _app_shell_diag_emit_pid_error("reset", "bad_loop", argv[2], NULL, NULL);
            return SHELL_OK;
        }

        if (_app_shell_diag_reject_pid_write_if_not_idle("reset", argv[2], NULL)) {
            return SHELL_OK;
        }

        if (!motor_runtime_diag_reset_pid(argv[2])) {
            _app_shell_diag_emit_pid_error("reset", "snapshot_unavailable", argv[2], NULL, NULL);
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("diag", "cmd=pid action=reset result=ok loop=%s", argv[2]);
        return SHELL_OK;
    }

    if ((argc != 5) || (app_shell_parse_float_arg(argv[4], &value) != 0)) {
        return SHELL_ERR_ARGC;
    }

    if (!_app_shell_diag_is_pid_loop(argv[2])) {
        _app_shell_diag_emit_pid_error("set", "bad_loop", argv[2], argv[3], NULL);
        return SHELL_OK;
    }
    if (!_app_shell_diag_is_pid_field(argv[3])) {
        _app_shell_diag_emit_pid_error("set", "bad_field", argv[2], argv[3], NULL);
        return SHELL_OK;
    }
    if (_app_shell_diag_reject_pid_write_if_not_idle("set", argv[2], argv[3])) {
        return SHELL_OK;
    }

    if (strcmp(argv[3], "lim") == 0) {
        value = fabsf(value);
    }

    if (!motor_runtime_diag_set_pid(argv[2], argv[3], value)) {
        _app_shell_diag_emit_pid_error("set", "snapshot_unavailable", argv[2], argv[3], NULL);
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=pid action=set result=ok loop=%s field=%s value=%s unit=raw",
                     argv[2],
                     argv[3],
                     app_shell_format_float_or_dash(buffer, sizeof(buffer), value, 4U));
    return SHELL_OK;
}

static int _app_shell_diag_handle_param(int argc, char *argv[])
{
    char  buffer[16];
    float value;

    if ((argc == 3) && (strcmp(argv[2], "show") == 0)) {
        _app_shell_diag_param_show();
        return SHELL_OK;
    }

    if (argc != 4) {
        return SHELL_ERR_ARGC;
    }
    if (app_shell_parse_float_arg(argv[3], &value) != 0) {
        return SHELL_ERR_ARG;
    }

    if (!_app_shell_diag_is_param_name(argv[2])) {
        _app_shell_diag_emit_param_error("set", "bad_name", argv[2], NULL, value, false);
        return SHELL_OK;
    }
    if (_app_shell_diag_param_requires_positive(argv[2]) && (value <= 0.0f)) {
        _app_shell_diag_emit_param_error("set", "bad_value", argv[2], NULL, value, true);
        return SHELL_OK;
    }
    if (_app_shell_diag_reject_param_write_if_not_idle("set", argv[2], value)) {
        return SHELL_OK;
    }

    if (!motor_runtime_diag_set_param(argv[2], value)) {
        _app_shell_diag_emit_param_error("set", "snapshot_unavailable", argv[2], NULL, value, false);
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=param action=set result=ok name=%s value=%s unit=raw",
                     argv[2],
                     app_shell_format_float_or_dash(buffer, sizeof(buffer), value, 4U));
    return SHELL_OK;
}

#if (TM_BENCH_DIAG != 0U)
static int _app_shell_diag_handle_encoder_setabs(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;
    uint16_t dir;
    float mech_offset;
    char offset[16];

    if (argc != 5) {
        return SHELL_ERR_ARGC;
    }
    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("diag", "cmd=enc action=setabs result=error reason=snapshot_unavailable");
        return SHELL_OK;
    }
    if (snapshot.axis_state != AXIS_STATE_IDLE) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=enc action=setabs result=error reason=idle_only axis=%s",
                         app_shell_axis_state_name(snapshot.axis_state));
        return SHELL_OK;
    }
    if (!_app_shell_diag_parse_u16(argv[3], &dir) || (dir > (uint16_t)DIR_CCW) ||
        (app_shell_parse_float_arg(argv[4], &mech_offset) != 0) || !isfinite(mech_offset) ||
        !motor_runtime_diag_set_encoder_abs((uint8_t)dir, mech_offset)) {
        TM_SHELL_SUMMARY("diag", "cmd=enc action=setabs result=error reason=invalid_value");
        return SHELL_OK;
    }
    TM_SHELL_SUMMARY("diag",
                     "cmd=enc action=setabs result=ok direction=%s mech_offset=%s unit=rad",
                     (dir == (uint16_t)DIR_CW) ? "cw" : "ccw",
                     app_shell_format_float_or_dash(offset, sizeof(offset), mech_offset, 4U));
    return SHELL_OK;
}
#endif

#if (TM_BENCH_DIAG != 0U)
static int _app_shell_diag_handle_bench(int argc, char *argv[])
{
    struct motor_runtime_bench_status status;

    if ((argc != 3) || ((strcmp(argv[2], "keepalive") != 0) &&
                        (strcmp(argv[2], "status") != 0))) {
        return SHELL_ERR_ARG;
    }

    if (strcmp(argv[2], "keepalive") == 0) {
        if (!motor_runtime_diag_bench_keepalive()) {
            TM_SHELL_SUMMARY("diag", "cmd=bench action=keepalive result=error reason=running_only");
            return SHELL_OK;
        }
        TM_SHELL_SUMMARY("diag", "cmd=bench action=keepalive result=ok reason=accepted");
        return SHELL_OK;
    }

    if (!motor_runtime_diag_bench_status(&status)) {
        TM_SHELL_SUMMARY("diag", "cmd=bench action=status result=error reason=snapshot_unavailable");
        return SHELL_OK;
    }
    TM_SHELL_SUMMARY("diag",
                     "cmd=bench action=status result=ok armed=%u age_ms=%lu timeout_count=%lu reason=%s",
                     status.armed ? 1U : 0U,
                     (unsigned long)status.heartbeat_age_ms,
                     (unsigned long)status.timeout_count,
                     status.expired ? "heartbeat_timeout" : "active");
    return SHELL_OK;
}

static int _app_shell_diag_handle_adc(int argc, char *argv[])
{
    struct motor_runtime_adc_view view;
    struct motor_runtime_snapshot snapshot;
    char ia_raw[16];
    char ib_raw[16];
    char ic_raw[16];
    char ia_offset[16];
    char ib_offset[16];
    char ic_offset[16];
    char ia_residual[16];
    char ib_residual[16];
    char ic_residual[16];

    if ((argc != 3) || ((strcmp(argv[2], "show") != 0) && (strcmp(argv[2], "zero") != 0))) {
        return SHELL_ERR_ARG;
    }

    if (strcmp(argv[2], "zero") == 0) {
        if (!motor_runtime_snapshot(&snapshot)) {
            TM_SHELL_SUMMARY("diag", "cmd=adc action=zero result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }
        if ((snapshot.axis_state != AXIS_STATE_IDLE) || (snapshot.fault_code != FAULT_NONE)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=adc action=zero result=error reason=idle_only axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }
        if (!motor_runtime_diag_zero_current_offsets()) {
            TM_SHELL_SUMMARY("diag", "cmd=adc action=zero result=error reason=calibration_failed");
            return SHELL_OK;
        }
        TM_SHELL_SUMMARY("diag", "cmd=adc action=zero result=ok reason=accepted");
    }

    if (!motor_runtime_diag_read_adc(&view)) {
        TM_SHELL_SUMMARY("diag", "cmd=adc action=show result=error reason=snapshot_unavailable");
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=adc action=show result=ok valid=%u clamp=low_side_zero samples=%lu fresh=%lu seq=%lu map=%u/%u adc_trig=%u",
                     view.calib_valid ? 1U : 0U,
                     (unsigned long)view.sample_count,
                     (unsigned long)view.fresh_count,
                     (unsigned long)view.sample_seq,
                     (unsigned)view.pwm_map,
                     (unsigned)view.adc_map,
                     (unsigned)view.adc_trigger);
    TM_SHELL_ROW("adc_raw",
                 "raw=%s/%s/%s off=%s/%s/%s",
                 app_shell_format_float_or_dash(ia_raw, sizeof(ia_raw), view.ia_raw, 1U),
                 app_shell_format_float_or_dash(ib_raw, sizeof(ib_raw), view.ib_raw, 1U),
                 app_shell_format_float_or_dash(ic_raw, sizeof(ic_raw), view.ic_raw, 1U),
                 app_shell_format_float_or_dash(ia_offset, sizeof(ia_offset), view.ia_offset, 1U),
                 app_shell_format_float_or_dash(ib_offset, sizeof(ib_offset), view.ib_offset, 1U),
                 app_shell_format_float_or_dash(ic_offset, sizeof(ic_offset), view.ic_offset, 1U));
    TM_SHELL_ROW("adc_res",
                 "a=%s/%s/%s unit=A",
                 app_shell_format_float_or_dash(ia_residual, sizeof(ia_residual), view.ia_residual, 3U),
                 app_shell_format_float_or_dash(ib_residual, sizeof(ib_residual), view.ib_residual, 3U),
                 app_shell_format_float_or_dash(ic_residual, sizeof(ic_residual), view.ic_residual, 3U));
    return SHELL_OK;
}
#endif

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_diag_command(int argc, char *argv[])
{
    if (argc < 2) {
        return SHELL_ERR_ARGC;
    }

    if ((strcmp(argv[1], "if2vel") == 0) && (argc >= 3)) {
        float rpm = 0.0f;
        if ((app_shell_parse_float_arg(argv[2], &rpm) != 0) ||
            (!isfinite(rpm))) {
            return SHELL_ERR_ARG;
        }
        if (!motor_runtime_request_if_to_vel_handoff(rpm)) {
            app_shell_emit_snapshot_unavailable("diag", "if2vel", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "if2vel", NULL, "vel_rpm", rpm, 3U, "rpm");
    }
    if ((strcmp(argv[1], "voltloop") == 0) && (argc >= 4)) {
        float rpm = 0.0f;
        float vq  = 0.0f;
        float we_elec = 0.0f;
        char  rpm_buf[16];
        char  vq_buf[16];
        char  we_buf[16];

        if ((app_shell_parse_float_arg(argv[2], &rpm) != 0) || (!isfinite(rpm)) ||
            (app_shell_parse_float_arg(argv[3], &vq) != 0) || (!isfinite(vq))) {
            return SHELL_ERR_ARG;
        }
        if (!motor_runtime_diag_set_voltloop(rpm, vq, &we_elec)) {
            struct motor_runtime_snapshot snapshot;

            if (motor_runtime_snapshot(&snapshot) && (snapshot.axis_state != AXIS_STATE_IDLE)) {
                TM_SHELL_SUMMARY("diag",
                                 "cmd=voltloop result=error reason=idle_only axis=%s",
                                 app_shell_axis_state_name(snapshot.axis_state));
            } else {
                TM_SHELL_SUMMARY("diag",
                                 "cmd=voltloop result=error reason=invalid_value vel_rpm=%s vq=%s",
                                 app_shell_format_float_or_dash(rpm_buf, sizeof(rpm_buf), rpm, 2U),
                                 app_shell_format_float_or_dash(vq_buf, sizeof(vq_buf), vq, 3U));
            }
            return SHELL_OK;
        }
        TM_SHELL_SUMMARY("diag",
                         "cmd=voltloop result=ok vel_rpm=%s unit=rpm",
                         app_shell_format_float_or_dash(rpm_buf, sizeof(rpm_buf), rpm, 2U));
        TM_SHELL_ROW("voltloop",
                     "vq=%s we_elec=%s",
                     app_shell_format_float_or_dash(vq_buf, sizeof(vq_buf), vq, 3U),
                     app_shell_format_float_or_dash(we_buf, sizeof(we_buf), we_elec, 2U));
        return SHELL_OK;
    }
    if (strcmp(argv[1], "set") == 0) {
        return _app_shell_diag_handle_set(argc, argv);
    }
#if (TM_BENCH_DIAG != 0U)
    if (strcmp(argv[1], "bench") == 0) {
        return _app_shell_diag_handle_bench(argc, argv);
    }
    if (strcmp(argv[1], "adc") == 0) {
        return _app_shell_diag_handle_adc(argc, argv);
    }
    if ((strcmp(argv[1], "velstep") == 0) && (argc == 3)) {
        char  buffer[16];
        float rpm = 0.0f;

        if ((app_shell_parse_float_arg(argv[2], &rpm) != 0) || !isfinite(rpm)) {
            return SHELL_ERR_ARG;
        }
        if (!motor_runtime_request_bench_velocity_step(rpm)) {
            TM_SHELL_SUMMARY("diag",
                             "cmd=velstep result=error reason=precondition vel_rpm=%s unit=rpm",
                             app_shell_format_float_or_dash(buffer, sizeof(buffer), rpm, 3U));
            return SHELL_OK;
        }
        TM_SHELL_SUMMARY("diag",
                         "cmd=velstep result=ok reason=accepted vel_rpm=%s unit=rpm",
                         app_shell_format_float_or_dash(buffer, sizeof(buffer), rpm, 3U));
        return SHELL_OK;
    }
    if (strcmp(argv[1], "trace") == 0) {
        return _app_shell_diag_handle_trace(argc, argv);
    }
    if (strcmp(argv[1], "map") == 0) {
        return _app_shell_diag_handle_map(argc, argv);
    }
    if (strcmp(argv[1], "enc") == 0) {
        if ((argc >= 3) && (strcmp(argv[2], "setabs") == 0)) {
            return _app_shell_diag_handle_encoder_setabs(argc, argv);
        }
        if ((argc < 3) || (strcmp(argv[2], "trace") != 0)) {
            return SHELL_ERR_ARG;
        }
        return _app_shell_diag_handle_encoder_trace(argc, argv);
    }
#endif
    if (strcmp(argv[1], "ramp") == 0) {
        return _app_shell_diag_handle_ramp(argc, argv);
    }
    if (strcmp(argv[1], "pid") == 0) {
        return _app_shell_diag_handle_pid(argc, argv);
    }
    if (strcmp(argv[1], "param") == 0) {
        return _app_shell_diag_handle_param(argc, argv);
    }

    return SHELL_ERR_ARG;
}

#if (TM_BENCH_DIAG != 0U)
SHELL_CMD_EXPORT(diag, app_shell_diag_command, 80U, "set|adc|bench|velstep|trace|map|enc|ramp|pid|param", "Diag");
#else
SHELL_CMD_EXPORT(diag, app_shell_diag_command, 80U, "set|ramp|pid|param", "Diag");
#endif

#endif /* APP_SHELL_HAS_DIAG_CMDS */

/* ---------------------------------------- end of file ----------------------------------------- */
