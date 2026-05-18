/**
 * @file    tm_format.h
 * @brief   Themis lightweight bounded formatter
 * @version v1.0.0
 * @date    2026.03.28
 *
 * Design overview
 * ---------------
 * This formatter exists to cover the small subset of printf-style formatting
 * used by Themis `subsys/log` and `subsys/shell` without pulling the full
 * libc `snprintf/vsnprintf` implementation into MCU firmware.
 *
 * Supported conversions
 * ---------------------
 * - `%c`
 * - `%s`
 * - `%d`
 * - `%u`
 * - `%x` / `%X`
 * - `%p`
 * - `%%`
 *
 * Supported modifiers
 * -------------------
 * - field width
 * - left align (`-`)
 * - zero pad (`0`)
 * - string precision (`%.*s` / `%.Ns`)
 * - `l` / `ll` / `z` integer length qualifiers
 *
 * Ownership / context
 * -------------------
 * - Caller owns the output buffer.
 * - Formatting is non-allocating and task/ISR agnostic.
 * - The formatter always NUL-terminates when `buffer_size > 0`.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Format one message into a caller-owned buffer.
 *
 * @param[out] buffer       Destination buffer. May be `NULL` only when
 *                          `buffer_size == 0`.
 * @param[in]  buffer_size  Destination buffer capacity in bytes.
 * @param[in]  fmt          Format string.
 * @param[in]  args         Variadic argument list.
 *
 * @return Number of characters that would have been produced, excluding the
 *         terminating NUL. This follows `snprintf()`-style truncation
 *         semantics but returns `size_t`, so it is never negative.
 */
size_t tm_format_vsnprintf(char *buffer, size_t buffer_size, const char *fmt, va_list args);

/**
 * @brief  Variadic wrapper over tm_format_vsnprintf().
 *
 * @return Number of characters that would have been produced, excluding the
 *         terminating NUL. Returns `size_t`, never a negative error code.
 */
size_t tm_format_snprintf(char *buffer, size_t buffer_size, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
