/**
 * @file    tm_format.c
 * @brief   Themis lightweight bounded formatter
 * @version v1.0.0
 * @date    2026.03.28
 *
 * Implementation role
 * -------------------
 * The formatter intentionally implements only the conversion patterns that the
 * current Themis tree uses. Keeping the surface small avoids dragging a large
 * generic printf backend into ROM while still preserving deterministic,
 * bounded-buffer behavior.
 *
 * Key collaborators
 * -----------------
 * - `tm_format.h`: public bounded-formatting contract
 * - `tmdef.h`: low-level type helpers such as `ssize_t`
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_format.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include "tmdef.h"

/* Private Types -------------------------------------------------------------------------------- */

enum tm_format_length {
    TM_FORMAT_LEN_DEFAULT = 0,
    TM_FORMAT_LEN_LONG,
    TM_FORMAT_LEN_LONGLONG,
    TM_FORMAT_LEN_SIZE,
};

struct tm_format_spec {
    unsigned int          width;
    int                   precision;
    enum tm_format_length length;
    char                  type;
    bool                  left_align;
    bool                  zero_pad;
};

struct tm_format_writer {
    char  *buffer;
    size_t buffer_size;
    size_t length;
};

/* Private Helpers ------------------------------------------------------------------------------ */

static void _tm_format_finish(struct tm_format_writer *writer);
static void _tm_format_putc(struct tm_format_writer *writer, char ch);
static void _tm_format_putn(struct tm_format_writer *writer, const char *text, size_t len);
static void _tm_format_repeat(struct tm_format_writer *writer, char ch, size_t count);
static bool _tm_format_parse_spec(const char **cursor, struct tm_format_spec *spec, va_list *args);
static unsigned int _tm_format_abs_width_from_int(int value);
static int          _tm_format_precision_append_digit(int precision, int digit);
static size_t _tm_format_strnlen(const char *text, size_t limit);
static void   _tm_format_u64_divmod10(uint64_t value, uint64_t *quotient, uint8_t *remainder);
static size_t _tm_format_u32_to_base(uint32_t value, unsigned int base, bool uppercase, char *out);
static size_t _tm_format_u64_to_base(uint64_t value, unsigned int base, bool uppercase, char *out);
static uint64_t _tm_format_get_unsigned_arg(va_list *args, enum tm_format_length length);
static int64_t  _tm_format_get_signed_arg(va_list *args, enum tm_format_length length);
static void     _tm_format_emit_string(struct tm_format_writer     *writer,
                                       const char                  *text,
                                       const struct tm_format_spec *spec);
static void     _tm_format_emit_char(struct tm_format_writer     *writer,
                                     char                         value,
                                     const struct tm_format_spec *spec);
static void     _tm_format_emit_unsigned(struct tm_format_writer     *writer,
                                         uint64_t                     value,
                                         unsigned int                 base,
                                         bool                         uppercase,
                                         const char                  *prefix,
                                         const struct tm_format_spec *spec);
static void     _tm_format_emit_signed(struct tm_format_writer     *writer,
                                       int64_t                      value,
                                       const struct tm_format_spec *spec);

/* Public Functions ----------------------------------------------------------------------------- */

size_t tm_format_vsnprintf(char *buffer, size_t buffer_size, const char *fmt, va_list args)
{
    struct tm_format_writer writer = {
        .buffer      = buffer,
        .buffer_size = buffer_size,
        .length      = 0U,
    };
    va_list ap;

    if ((fmt == NULL) || ((buffer == NULL) && (buffer_size != 0U))) {
        return 0U;
    }

    va_copy(ap, args);

    while (*fmt != '\0') {
        struct tm_format_spec spec;

        if (*fmt != '%') {
            _tm_format_putc(&writer, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == '\0') {
            break;
        }

        if (*fmt == '%') {
            _tm_format_putc(&writer, '%');
            fmt++;
            continue;
        }

        if (!_tm_format_parse_spec(&fmt, &spec, &ap)) {
            _tm_format_putc(&writer, '%');
            continue;
        }

        switch (spec.type) {
        case 'c':
            _tm_format_emit_char(&writer, (char)va_arg(ap, int), &spec);
            break;
        case 's':
            _tm_format_emit_string(&writer, va_arg(ap, const char *), &spec);
            break;
        case 'd':
            _tm_format_emit_signed(&writer, _tm_format_get_signed_arg(&ap, spec.length), &spec);
            break;
        case 'u':
            _tm_format_emit_unsigned(&writer,
                                     _tm_format_get_unsigned_arg(&ap, spec.length),
                                     10U,
                                     false,
                                     "",
                                     &spec);
            break;
        case 'x':
            _tm_format_emit_unsigned(&writer,
                                     _tm_format_get_unsigned_arg(&ap, spec.length),
                                     16U,
                                     false,
                                     "",
                                     &spec);
            break;
        case 'X':
            _tm_format_emit_unsigned(&writer,
                                     _tm_format_get_unsigned_arg(&ap, spec.length),
                                     16U,
                                     true,
                                     "",
                                     &spec);
            break;
        case 'p': {
            struct tm_format_spec ptr_spec = spec;

            if (ptr_spec.width == 0U) {
                ptr_spec.width    = (unsigned int)((sizeof(void *) * 2U) + 2U);
                ptr_spec.zero_pad = true;
            }

            _tm_format_emit_unsigned(&writer,
                                     (uint64_t)(uintptr_t)va_arg(ap, void *),
                                     16U,
                                     false,
                                     "0x",
                                     &ptr_spec);
            break;
        }
        default:
            _tm_format_putc(&writer, '%');
            _tm_format_putc(&writer, spec.type);
            break;
        }
    }

    va_end(ap);
    _tm_format_finish(&writer);
    return writer.length;
}

size_t tm_format_snprintf(char *buffer, size_t buffer_size, const char *fmt, ...)
{
    va_list args;
    size_t  written;

    va_start(args, fmt);
    written = tm_format_vsnprintf(buffer, buffer_size, fmt, args);
    va_end(args);

    return written;
}

/* Private Functions ---------------------------------------------------------------------------- */

static void _tm_format_finish(struct tm_format_writer *writer)
{
    if ((writer == NULL) || (writer->buffer == NULL) || (writer->buffer_size == 0U)) {
        return;
    }

    if (writer->length < writer->buffer_size) {
        writer->buffer[writer->length] = '\0';
    } else {
        writer->buffer[writer->buffer_size - 1U] = '\0';
    }
}

static void _tm_format_putc(struct tm_format_writer *writer, char ch)
{
    if (writer == NULL) {
        return;
    }

    if ((writer->buffer != NULL) && (writer->buffer_size > 0U) &&
        (writer->length + 1U < writer->buffer_size)) {
        writer->buffer[writer->length] = ch;
    }

    writer->length++;
}

static void _tm_format_putn(struct tm_format_writer *writer, const char *text, size_t len)
{
    if ((writer == NULL) || (text == NULL)) {
        return;
    }

    for (size_t i = 0U; i < len; i++) {
        _tm_format_putc(writer, text[i]);
    }
}

static void _tm_format_repeat(struct tm_format_writer *writer, char ch, size_t count)
{
    for (size_t i = 0U; i < count; i++) {
        _tm_format_putc(writer, ch);
    }
}

static bool _tm_format_parse_spec(const char **cursor, struct tm_format_spec *spec, va_list *args)
{
    const char *fmt;

    if ((cursor == NULL) || (*cursor == NULL) || (spec == NULL) || (args == NULL)) {
        return false;
    }

    fmt = *cursor;

    spec->width      = 0U;
    spec->precision  = -1;
    spec->length     = TM_FORMAT_LEN_DEFAULT;
    spec->type       = '\0';
    spec->left_align = false;
    spec->zero_pad   = false;

    while ((*fmt == '-') || (*fmt == '0')) {
        if (*fmt == '-') {
            spec->left_align = true;
        } else {
            spec->zero_pad = true;
        }
        fmt++;
    }

    if (*fmt == '*') {
        int dynamic_width = va_arg(*args, int);

        if (dynamic_width < 0) {
            spec->left_align = true;
            spec->width      = _tm_format_abs_width_from_int(dynamic_width);
        } else {
            spec->width = (unsigned int)dynamic_width;
        }
        fmt++;
    } else {
        while ((*fmt >= '0') && (*fmt <= '9')) {
            spec->width = (spec->width * 10U) + (unsigned int)(*fmt - '0');
            fmt++;
        }
    }

    if (*fmt == '.') {
        fmt++;
        spec->precision = 0;
        if (*fmt == '*') {
            spec->precision = va_arg(*args, int);
            fmt++;
        } else {
            while ((*fmt >= '0') && (*fmt <= '9')) {
                spec->precision = _tm_format_precision_append_digit(spec->precision,
                                                                    (int)(*fmt - '0'));
                fmt++;
            }
        }

        if (spec->precision < 0) {
            spec->precision = -1;
        }
    }

    if (*fmt == 'l') {
        spec->length = TM_FORMAT_LEN_LONG;
        fmt++;
        if (*fmt == 'l') {
            spec->length = TM_FORMAT_LEN_LONGLONG;
            fmt++;
        }
    } else if (*fmt == 'z') {
        spec->length = TM_FORMAT_LEN_SIZE;
        fmt++;
    }

    spec->type = *fmt;
    if (spec->type == '\0') {
        return false;
    }

    *cursor = fmt + 1;
    return true;
}

static unsigned int _tm_format_abs_width_from_int(int value)
{
    if (value == INT_MIN) {
        return (unsigned int)INT_MAX;
    }

    return (value < 0) ? (unsigned int)(-value) : (unsigned int)value;
}

static int _tm_format_precision_append_digit(int precision, int digit)
{
    if (precision > ((INT_MAX - digit) / 10)) {
        return INT_MAX;
    }

    return (precision * 10) + digit;
}

static size_t _tm_format_strnlen(const char *text, size_t limit)
{
    size_t len = 0U;

    if (text == NULL) {
        return 0U;
    }

    while ((len < limit) && (text[len] != '\0')) {
        len++;
    }

    return len;
}

static void _tm_format_u64_divmod10(uint64_t value, uint64_t *quotient, uint8_t *remainder)
{
    uint64_t q = 0U;
    uint64_t r = 0U;

    if ((quotient == NULL) || (remainder == NULL)) {
        return;
    }

    for (int bit = 63; bit >= 0; bit--) {
        r = (r << 1U) | ((value >> bit) & 1ULL);
        if (r >= 10ULL) {
            r -= 10ULL;
            q |= (1ULL << bit);
        }
    }

    *quotient  = q;
    *remainder = (uint8_t)r;
}

static size_t _tm_format_u32_to_base(uint32_t value, unsigned int base, bool uppercase, char *out)
{
    static const char _digits_lower[] = "0123456789abcdef";
    static const char _digits_upper[] = "0123456789ABCDEF";
    const char       *digits          = uppercase ? _digits_upper : _digits_lower;
    size_t            len             = 0U;

    if ((out == NULL) || ((base != 10U) && (base != 16U))) {
        return 0U;
    }

    if (base == 16U) {
        do {
            out[len++] = digits[value & 0x0FU];
            value >>= 4U;
        } while (value != 0U);

        return len;
    }

    do {
        out[len++] = digits[value % 10U];
        value /= 10U;
    } while (value != 0U);

    return len;
}

static size_t _tm_format_u64_to_base(uint64_t value, unsigned int base, bool uppercase, char *out)
{
    static const char _digits_lower[] = "0123456789abcdef";
    static const char _digits_upper[] = "0123456789ABCDEF";
    const char       *digits          = uppercase ? _digits_upper : _digits_lower;
    size_t            len             = 0U;

    if ((out == NULL) || ((base != 10U) && (base != 16U))) {
        return 0U;
    }

    if (value <= UINT32_MAX) {
        return _tm_format_u32_to_base((uint32_t)value, base, uppercase, out);
    }

    if (base == 16U) {
        do {
            out[len++] = digits[(unsigned int)(value & 0x0FULL)];
            value >>= 4U;
        } while (value != 0U);

        return len;
    }

    do {
        uint64_t quotient;
        uint8_t  remainder;

        _tm_format_u64_divmod10(value, &quotient, &remainder);
        out[len++] = digits[remainder];
        value      = quotient;
    } while (value != 0U);

    return len;
}

static uint64_t _tm_format_get_unsigned_arg(va_list *args, enum tm_format_length length)
{
    switch (length) {
    case TM_FORMAT_LEN_LONG:
        return (uint64_t)va_arg(*args, unsigned long);
    case TM_FORMAT_LEN_LONGLONG:
        return (uint64_t)va_arg(*args, unsigned long long);
    case TM_FORMAT_LEN_SIZE:
        return (uint64_t)va_arg(*args, size_t);
    case TM_FORMAT_LEN_DEFAULT:
    default:
        return (uint64_t)va_arg(*args, unsigned int);
    }
}

static int64_t _tm_format_get_signed_arg(va_list *args, enum tm_format_length length)
{
    switch (length) {
    case TM_FORMAT_LEN_LONG:
        return (int64_t)va_arg(*args, long);
    case TM_FORMAT_LEN_LONGLONG:
        return (int64_t)va_arg(*args, long long);
    case TM_FORMAT_LEN_SIZE:
        return (int64_t)va_arg(*args, ssize_t);
    case TM_FORMAT_LEN_DEFAULT:
    default:
        return (int64_t)va_arg(*args, int);
    }
}

static void _tm_format_emit_string(struct tm_format_writer     *writer,
                                   const char                  *text,
                                   const struct tm_format_spec *spec)
{
    size_t len;
    size_t pad = 0U;

    if ((writer == NULL) || (spec == NULL)) {
        return;
    }

    if (text == NULL) {
        text = "(null)";
    }

    len = (spec->precision >= 0) ? _tm_format_strnlen(text, (size_t)spec->precision)
                                 : _tm_format_strnlen(text, SIZE_MAX);
    if (spec->width > len) {
        pad = spec->width - len;
    }

    if (!spec->left_align) {
        _tm_format_repeat(writer, ' ', pad);
    }

    _tm_format_putn(writer, text, len);

    if (spec->left_align) {
        _tm_format_repeat(writer, ' ', pad);
    }
}

static void _tm_format_emit_char(struct tm_format_writer     *writer,
                                 char                         value,
                                 const struct tm_format_spec *spec)
{
    size_t pad = 0U;

    if ((writer == NULL) || (spec == NULL)) {
        return;
    }

    if (spec->width > 1U) {
        pad = spec->width - 1U;
    }

    if (!spec->left_align) {
        _tm_format_repeat(writer, ' ', pad);
    }

    _tm_format_putc(writer, value);

    if (spec->left_align) {
        _tm_format_repeat(writer, ' ', pad);
    }
}

static void _tm_format_emit_unsigned(struct tm_format_writer     *writer,
                                     uint64_t                     value,
                                     unsigned int                 base,
                                     bool                         uppercase,
                                     const char                  *prefix,
                                     const struct tm_format_spec *spec)
{
    char   digits[32];
    size_t digits_len;
    size_t prefix_len;
    size_t body_len;
    size_t pad = 0U;
    char   pad_char;

    if ((writer == NULL) || (prefix == NULL) || (spec == NULL)) {
        return;
    }

    digits_len = _tm_format_u64_to_base(value, base, uppercase, digits);
    prefix_len = _tm_format_strnlen(prefix, SIZE_MAX);
    body_len   = prefix_len + digits_len;
    if (spec->width > body_len) {
        pad = spec->width - body_len;
    }

    pad_char = (!spec->left_align && spec->zero_pad && (spec->precision < 0)) ? '0' : ' ';

    if (!spec->left_align && (pad_char == ' ')) {
        _tm_format_repeat(writer, ' ', pad);
    }

    _tm_format_putn(writer, prefix, prefix_len);

    if (!spec->left_align && (pad_char == '0')) {
        _tm_format_repeat(writer, '0', pad);
    }

    for (size_t i = digits_len; i > 0U; i--) {
        _tm_format_putc(writer, digits[i - 1U]);
    }

    if (spec->left_align) {
        _tm_format_repeat(writer, ' ', pad);
    }
}

static void _tm_format_emit_signed(struct tm_format_writer     *writer,
                                   int64_t                      value,
                                   const struct tm_format_spec *spec)
{
    uint64_t magnitude;

    if ((writer == NULL) || (spec == NULL)) {
        return;
    }

    if (value < 0) {
        magnitude = (uint64_t)(-(value + 1)) + 1U;
        _tm_format_emit_unsigned(writer, magnitude, 10U, false, "-", spec);
        return;
    }

    _tm_format_emit_unsigned(writer, (uint64_t)value, 10U, false, "", spec);
}

/* ---------------------------------------- end of file ----------------------------------------- */
