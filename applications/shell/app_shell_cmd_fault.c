/**
 * @file    app_shell_cmd_fault.c
 * @brief   FalconFoc product shell `fault` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_output_helpers.h"
#include <string.h>
#include "motor_fault_runtime.h"
#include "motor_runtime.h"
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

#if APP_SHELL_ENABLED

static void _app_shell_fault_emit_head(const struct motor_fault_context *context)
{
    TM_SHELL_ROW("context",
                 "valid=1 seq=%lu fault=0x%08x axis=%s mode=%s sector=%u",
                 (unsigned long)context->sequence,
                 (unsigned)context->fault_code,
                 app_shell_axis_state_name(context->axis_state),
                 app_shell_ctrl_mode_name(context->ctrl_mode),
                 (unsigned)context->sector);
}

static void _app_shell_fault_emit_adc(const struct motor_fault_context *context)
{
    char ia_raw[16];
    char ib_raw[16];
    char ic_raw[16];
    char v_bus_raw[16];

    TM_SHELL_ROW("adc",
                 "raw=%s/%s/%s vraw=%s",
                 app_shell_format_float_or_dash(ia_raw, sizeof(ia_raw), context->ia_raw, 2U),
                 app_shell_format_float_or_dash(ib_raw, sizeof(ib_raw), context->ib_raw, 2U),
                 app_shell_format_float_or_dash(ic_raw, sizeof(ic_raw), context->ic_raw, 2U),
                 app_shell_format_float_or_dash(v_bus_raw, sizeof(v_bus_raw), context->v_bus_raw, 2U));
}

static void _app_shell_fault_emit_phase(const struct motor_fault_context *context)
{
    char ia[16];
    char ib[16];
    char ic[16];
    char v_bus[16];

    TM_SHELL_ROW("phase",
                 "i=%s/%s/%s vbus=%s",
                 app_shell_format_float_or_dash(ia, sizeof(ia), context->ia, 3U),
                 app_shell_format_float_or_dash(ib, sizeof(ib), context->ib, 3U),
                 app_shell_format_float_or_dash(ic, sizeof(ic), context->ic, 3U),
                 app_shell_format_float_or_dash(v_bus, sizeof(v_bus), context->v_bus, 2U));
}

static void _app_shell_fault_emit_angle(const struct motor_fault_context *context)
{
    char theta_sim[16];
    char theta_elec[16];
    char we_now[16];
    char we_ramp[16];

    TM_SHELL_ROW("angle",
                 "theta=%s/%s we=%s/%s",
                 app_shell_format_float_or_dash(theta_sim, sizeof(theta_sim), context->theta_sim, 3U),
                 app_shell_format_float_or_dash(theta_elec, sizeof(theta_elec), context->theta_elec, 3U),
                 app_shell_format_float_or_dash(we_now, sizeof(we_now), context->we_volt_now, 3U),
                 app_shell_format_float_or_dash(we_ramp, sizeof(we_ramp), context->we_volt_ramp, 3U));
}

static void _app_shell_fault_emit_foc(const struct motor_fault_context *context)
{
    char id[16];
    char iq[16];
    char vd[16];
    char vq[16];
    char v_alpha[16];
    char v_beta[16];

    TM_SHELL_ROW("foc",
                 "idq=%s/%s vdq=%s/%s ab=%s/%s",
                 app_shell_format_float_or_dash(id, sizeof(id), context->id, 3U),
                 app_shell_format_float_or_dash(iq, sizeof(iq), context->iq, 3U),
                 app_shell_format_float_or_dash(vd, sizeof(vd), context->vd, 3U),
                 app_shell_format_float_or_dash(vq, sizeof(vq), context->vq, 3U),
                 app_shell_format_float_or_dash(v_alpha, sizeof(v_alpha), context->v_alpha, 3U),
                 app_shell_format_float_or_dash(v_beta, sizeof(v_beta), context->v_beta, 3U));
}

static void _app_shell_fault_emit_svpwm(const struct motor_fault_context *context)
{
    char dtc_a[16];
    char dtc_b[16];
    char dtc_c[16];

    TM_SHELL_ROW("svpwm",
                 "d=%s/%s/%s",
                 app_shell_format_float_or_dash(dtc_a, sizeof(dtc_a), context->dtc_a, 3U),
                 app_shell_format_float_or_dash(dtc_b, sizeof(dtc_b), context->dtc_b, 3U),
                 app_shell_format_float_or_dash(dtc_c, sizeof(dtc_c), context->dtc_c, 3U));
}

static void _app_shell_fault_context_show(void)
{
    struct motor_fault_context context;

    if (!motor_fault_context_read(&context)) {
        TM_SHELL_ROW("context", "valid=0");
        return;
    }

    _app_shell_fault_emit_head(&context);
    _app_shell_fault_emit_adc(&context);
    _app_shell_fault_emit_phase(&context);
    _app_shell_fault_emit_angle(&context);
    _app_shell_fault_emit_foc(&context);
    _app_shell_fault_emit_svpwm(&context);
}

static void _app_shell_fault_status(void)
{
    struct motor_runtime_snapshot snapshot;
    uint32_t mask;
    uint32_t bit;

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("fault", "cmd=status result=error reason=snapshot_unavailable");
        return;
    }

    mask = snapshot.fault_code;
    TM_SHELL_SUMMARY("fault",
                     "cmd=status result=ok active=%u mask=0x%08x",
                     (mask != 0U) ? 1U : 0U,
                     (unsigned)mask);

    if (mask == 0U) {
        TM_SHELL_ROW("fault", "name=none");
        _app_shell_fault_context_show();
        return;
    }

    for (bit = 1U; bit != 0U; bit <<= 1U) {
        if ((mask & bit) != 0U) {
            TM_SHELL_ROW("fault", "name=%s bit=0x%08x", app_shell_fault_bit_name(bit), (unsigned)bit);
        }
    }

    _app_shell_fault_context_show();
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_fault_command(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;
    bool                          ok;

    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "status") == 0))) {
        _app_shell_fault_status();
        return SHELL_OK;
    }

    if ((argc == 2) && (strcmp(argv[1], "clear") == 0)) {
        if (!motor_runtime_snapshot(&snapshot)) {
            TM_SHELL_SUMMARY("fault", "cmd=clear result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }

        ok = motor_runtime_request_axis_action(TM_MOTOR_RUNTIME_AXIS_CLEAR_FAULT);
        if (!motor_runtime_snapshot(&snapshot)) {
            TM_SHELL_SUMMARY("fault", "cmd=clear result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }

        if (!ok) {
            TM_SHELL_SUMMARY("fault",
                             "cmd=clear result=error axis=%s rc=-1",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("fault", "cmd=clear result=ok mask=0x%08x", (unsigned)snapshot.fault_code);
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(fault, app_shell_fault_command, 40U, "[status|clear]", "Fault");

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
