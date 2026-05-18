/**
 * @file    app_shell_common.c
 * @brief   FalconFoc shell shared helpers
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_common.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "tm_format.h"

/* Private Macros ------------------------------------------------------------------------------- */

#define APP_SHELL_FLOAT_FRAC_MAX (6U)

/* Private Helpers ------------------------------------------------------------------------------ */

static const uint32_t _app_shell_pow10_u32[] = {
    1U,
    10U,
    100U,
    1000U,
    10000U,
    100000U,
    1000000U,
};

/* Public Functions ----------------------------------------------------------------------------- */

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

const char *app_shell_sensor_name(uint8_t sensor)
{
    switch (sensor) {
    case SENSOR_TYPE_NONE:
        return "none";
    case SENSOR_TYPE_MAGNETIC:
        return "magnetic";
    case SENSOR_TYPE_HALL:
        return "hall";
    case SENSOR_TYPE_INC_ENC:
        return "inc-enc";
    default:
        return "unknown";
    }
}

const char *app_shell_fault_bit_name(uint32_t bit)
{
    switch (bit) {
    case FAULT_OVER_CURR:
        return "over-curr";
    case FAULT_OVER_VOLT:
        return "over-volt";
    case FAULT_UNDER_VOLT:
        return "under-volt";
    case FAULT_OVER_VEL:
        return "over-vel";
    case FAULT_MOTOR_OVER_TEMP:
        return "motor-over-temp";
    case FAULT_DRIVER_OVER_TEMP:
        return "driver-over-temp";
    case FAULT_SENSOR_ERROR:
        return "sensor-error";
    case FAULT_CALIBRATION_FAILED:
        return "calibration-failed";
    case FAULT_COMM_ERROR:
        return "comm-error";
    default:
        return "unknown";
    }
}

int app_shell_parse_ctrl_mode(const char *text, enum foc_ctrl_mode *mode_out)
{
    if ((text == NULL) || (mode_out == NULL)) {
        return -EINVAL;
    }

    if (strcmp(text, "volt") == 0) {
        *mode_out = CTRL_MODE_VOLT;
        return 0;
    }
    if (strcmp(text, "curr") == 0) {
        *mode_out = CTRL_MODE_CURR;
        return 0;
    }
    if (strcmp(text, "vel") == 0) {
        *mode_out = CTRL_MODE_VEL;
        return 0;
    }
    if (strcmp(text, "pos") == 0) {
        *mode_out = CTRL_MODE_POS;
        return 0;
    }

    return -EINVAL;
}

int app_shell_parse_input_mode(const char *text, enum foc_input_mode *mode_out)
{
    if ((text == NULL) || (mode_out == NULL)) {
        return -EINVAL;
    }

    if (strcmp(text, "inactive") == 0) {
        *mode_out = INPUT_MODE_INACTIVE;
        return 0;
    }
    if ((strcmp(text, "pass") == 0) || (strcmp(text, "passthrough") == 0)) {
        *mode_out = INPUT_MODE_PASSTHROUGH;
        return 0;
    }
    if ((strcmp(text, "vel-ramp") == 0) || (strcmp(text, "vel_ramp") == 0)) {
        *mode_out = INPUT_MODE_VEL_RAMP;
        return 0;
    }
    if ((strcmp(text, "torque-ramp") == 0) || (strcmp(text, "torque_ramp") == 0)) {
        *mode_out = INPUT_MODE_TORQUE_RAMP;
        return 0;
    }
    if ((strcmp(text, "pos-filter") == 0) || (strcmp(text, "pos_filter") == 0)) {
        *mode_out = INPUT_MODE_POS_FILTER;
        return 0;
    }
    if ((strcmp(text, "vel-filter") == 0) || (strcmp(text, "vel_filter") == 0)) {
        *mode_out = INPUT_MODE_VEL_FILTER;
        return 0;
    }
    if ((strcmp(text, "traj") == 0) || (strcmp(text, "trap-traj") == 0) ||
        (strcmp(text, "trap_traj") == 0)) {
        *mode_out = INPUT_MODE_TRAP_TRAJ;
        return 0;
    }
    if (strcmp(text, "tuning") == 0) {
        *mode_out = INPUT_MODE_TUNING;
        return 0;
    }

    return -EINVAL;
}

int app_shell_parse_float_arg(const char *text, float *out)
{
    char *end = NULL;

    if ((text == NULL) || (out == NULL)) {
        return -EINVAL;
    }

    *out = strtof(text, &end);
    if (end == text) {
        return -EINVAL;
    }

    while ((*end == ' ') || (*end == '\t')) {
        end++;
    }

    return (*end == '\0') ? 0 : -EINVAL;
}

int app_shell_format_float(char *buffer, size_t buffer_size, float value, uint8_t frac_digits)
{
    bool     negative;
    float    abs_value;
    uint32_t scale;
    uint32_t scaled;
    uint32_t int_part;
    uint32_t frac_part;
    size_t   used;
    size_t   written;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return -EINVAL;
    }

    if (frac_digits > APP_SHELL_FLOAT_FRAC_MAX) {
        frac_digits = APP_SHELL_FLOAT_FRAC_MAX;
    }

    negative  = (value < 0.0f);
    abs_value = negative ? -value : value;
    scale     = _app_shell_pow10_u32[frac_digits];
    scaled    = (uint32_t)((abs_value * (float)scale) + 0.5f);
    int_part  = scaled / scale;
    frac_part = scaled % scale;
    used      = 0U;

    buffer[0] = '\0';

    if (negative) {
        if (buffer_size < 2U) {
            return -ERANGE;
        }
        buffer[used++] = '-';
        buffer[used]   = '\0';
    }

    written = tm_format_snprintf(&buffer[used], buffer_size - used, "%u", int_part);
    if (written >= (buffer_size - used)) {
        return -ERANGE;
    }
    used += written;

    while ((frac_digits > 0U) && ((frac_part % 10U) == 0U)) {
        frac_part /= 10U;
        frac_digits--;
    }

    if (frac_digits == 0U) {
        return 0;
    }

    if ((used + 1U) >= buffer_size) {
        return -ERANGE;
    }

    buffer[used++] = '.';
    buffer[used]   = '\0';

    written = tm_format_snprintf(&buffer[used], buffer_size - used, "%0*u", (int)frac_digits, frac_part);
    if (written >= (buffer_size - used)) {
        return -ERANGE;
    }

    return 0;
}

const char *app_shell_format_float_or_dash(char *buffer, size_t buffer_size, float value, uint8_t frac_digits)
{
    if (app_shell_format_float(buffer, buffer_size, value, frac_digits) != 0) {
        if ((buffer != NULL) && (buffer_size > 0U)) {
            (void)tm_format_snprintf(buffer, buffer_size, "-");
        }
    }

    return buffer;
}

/* ---------------------------------------- end of file ----------------------------------------- */
