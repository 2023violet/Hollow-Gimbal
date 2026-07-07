/**
 * @file    app_shell_common.h
 * @brief   FalconFoc shell shared helpers
 * @version v1.0.0
 * @date    2026.05.12
 *
 * Design overview
 * ---------------
 * This header owns shell-side vocabulary helpers, numeric parsing, and small
 * formatting helpers. It does not own transport, session state, or product
 * policy.
 *
 * Usage example
 * -------------
 * - command handlers use the `*_name()` helpers to keep user-facing vocabulary
 *   stable
 * - numeric write commands use `app_shell_parse_float_arg()` to stay on the
 *   bounded decimal-only input surface
 * - row helpers use `app_shell_format_float_or_dash()` to avoid `%f` in the
 *   shell output path
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>
#include "motor_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vocabulary Helpers -------------------------------------------------------------------- */

/**
 * @brief  Translate one axis state into the stable shell vocabulary.
 */
const char *app_shell_axis_state_name(uint8_t state);

/**
 * @brief  Translate one control mode into the stable shell vocabulary.
 */
const char *app_shell_ctrl_mode_name(uint8_t mode);

/**
 * @brief  Translate one input mode into the stable shell vocabulary.
 */
const char *app_shell_input_mode_name(uint8_t mode);

/**
 * @brief  Translate one sensor type into the stable shell vocabulary.
 */
const char *app_shell_sensor_name(uint8_t sensor);

/**
 * @brief  Translate one fault bit into the stable shell vocabulary.
 */
const char *app_shell_fault_bit_name(uint32_t bit);

/* Parsing Helpers ----------------------------------------------------------------------- */

/**
 * @brief  Parse one control-mode token accepted by the `cfg` and `diag` shell paths.
 */
int app_shell_parse_ctrl_mode(const char *text, enum foc_ctrl_mode *mode_out);

/**
 * @brief  Parse one input-mode token accepted by the `cfg` shell path.
 */
int app_shell_parse_input_mode(const char *text, enum foc_input_mode *mode_out);

/**
 * @brief  Parse one bounded decimal float accepted by FalconFoc shell commands.
 *
 * Supported syntax is:
 * - optional leading whitespace
 * - optional `+` / `-`
 * - decimal integer part
 * - optional fractional part
 * - optional trailing whitespace
 *
 * Scientific notation, hex-float syntax, `nan`, `inf`, and trailing junk are
 * rejected on purpose to keep the shell surface small and deterministic.
 */
int app_shell_parse_float_arg(const char *text, float *out);

/* Formatting Helpers -------------------------------------------------------------------- */

/**
 * @brief  Format one float into a caller-owned bounded buffer without `%f`.
 */
int         app_shell_format_float(char *buffer, size_t buffer_size, float value, uint8_t frac_digits);

/**
 * @brief  Format one float or write `-` when formatting fails.
 */
const char *app_shell_format_float_or_dash(char *buffer,
                                           size_t buffer_size,
                                           float  value,
                                           uint8_t frac_digits);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
