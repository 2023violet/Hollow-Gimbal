/**
 * @file    app_shell_common.h
 * @brief   FalconFoc shell shared helpers
 * @version v1.0.0
 * @date    2026.05.12
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern struct motor_dev _motor_ins;

const char *app_shell_axis_state_name(uint8_t state);
const char *app_shell_ctrl_mode_name(uint8_t mode);
const char *app_shell_input_mode_name(uint8_t mode);
const char *app_shell_sensor_name(uint8_t sensor);
const char *app_shell_fault_bit_name(uint32_t bit);

int app_shell_parse_ctrl_mode(const char *text, enum foc_ctrl_mode *mode_out);
int app_shell_parse_input_mode(const char *text, enum foc_input_mode *mode_out);
int app_shell_parse_float_arg(const char *text, float *out);

int         app_shell_format_float(char *buffer, size_t buffer_size, float value, uint8_t frac_digits);
const char *app_shell_format_float_or_dash(char *buffer,
                                           size_t buffer_size,
                                           float  value,
                                           uint8_t frac_digits);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
