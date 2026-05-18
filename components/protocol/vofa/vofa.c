/**
 * @file    vofa.c
 * @brief   Reusable VOFA+ waveform framing component with selectable protocol
 * @version v2.0.0
 * @date    2026.03.28
 *
 * Implementation role
 * -------------------
 * - pairs with `vofa.h`
 * - owns protocol-specific frame packing and channel-to-float conversion
 * - keeps transport ownership outside the component through the send callback
 *
 * Key collaborators
 * -----------------
 * - `vofa.h`: binding and protocol-selection contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "vofa.h"
#include <string.h>
#include "tmdef.h"

/* Private Macros ------------------------------------------------------------------------------- */

/* JustFloat tail: IEEE-754 +infinity marker expected by VOFA+. */
#define VOFA_TAIL_SIZE (4U)

#if (VOFA_PROTOCOL == VOFA_PROTO_JUSTFLOAT)
static const uint8_t _justfloat_tail[VOFA_TAIL_SIZE] = {0x00, 0x00, 0x80, 0x7F};
#else
#define VOFA_SIGNIFICANT_DIGITS        (6U)
#define VOFA_FIXED_MAX_FRACTION_DIGITS (5U)
#define VOFA_SCI_FRACTION_DIGITS       (5U)
#endif

/* Private Types -------------------------------------------------------------------------------- */

struct vofa_channel_info {
    void               *ptr;
    enum vofa_data_type type;
};

struct vofa_handle {
    struct vofa_channel_info channels[VOFA_MAX_CHANNELS];
    uint8_t                  active_ch_num;

    /* JustFloat binary TX buffer: N * 4 bytes + 4-byte tail marker. */
    uint8_t jf_buf[VOFA_MAX_CHANNELS * 4U + VOFA_TAIL_SIZE];

    /* FireWater ASCII TX buffer. */
    uint8_t fw_buf[VOFA_FW_BUF_SIZE];

    vofa_send_callback_t send_callback;
};

/* Private State -------------------------------------------------------------------------------- */

static struct vofa_handle _vofa;

/* Private Helpers ------------------------------------------------------------------------------ */

#if (VOFA_PROTOCOL == VOFA_PROTO_FIREWATER)
static int _u32_to_ascii(char *buffer, size_t buffer_size, uint32_t value);
static int _format_float(char *buffer, size_t buffer_size, float value);
#endif

static void _noop_send(uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
}

static void _vofa_refresh_active_channels(void)
{
    uint8_t active = 0U;

    for (uint8_t i = 0U; i < VOFA_MAX_CHANNELS; i++) {
        if (_vofa.channels[i].ptr != NULL) {
            active = (uint8_t)(i + 1U);
        }
    }

    _vofa.active_ch_num = active;
}

#if (VOFA_PROTOCOL == VOFA_PROTO_JUSTFLOAT)
static float _read_channel(uint8_t i)
{
    void               *ptr  = _vofa.channels[i].ptr;
    enum vofa_data_type type = _vofa.channels[i].type;
    float               result;

    if (ptr == NULL) {
        return 0.0f;
    }

    switch (type) {
    case VOFA_FLOAT:
        memcpy(&result, ptr, sizeof(result));
        return result;
    case VOFA_INT8: {
        int8_t value = 0;
        memcpy(&value, ptr, sizeof(value));
        return (float)value;
    }
    case VOFA_UINT8: {
        uint8_t value = 0U;
        memcpy(&value, ptr, sizeof(value));
        return (float)value;
    }
    case VOFA_INT16: {
        int16_t value = 0;
        memcpy(&value, ptr, sizeof(value));
        return (float)value;
    }
    case VOFA_UINT16: {
        uint16_t value = 0U;
        memcpy(&value, ptr, sizeof(value));
        return (float)value;
    }
    case VOFA_INT32: {
        int32_t value = 0;
        memcpy(&value, ptr, sizeof(value));
        return (float)value;
    }
    case VOFA_UINT32: {
        uint32_t value = 0U;
        memcpy(&value, ptr, sizeof(value));
        return (float)value;
    }
    default:
        return 0.0f;
    }
}
#endif

#if (VOFA_PROTOCOL == VOFA_PROTO_FIREWATER)
static int _format_signed_ascii(char *buffer, size_t buffer_size, int32_t value)
{
    size_t   pos      = 0U;
    uint32_t magnitude;
    int      written;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return -1;
    }

    if (value < 0) {
        if (buffer_size < 2U) {
            return -1;
        }
        buffer[pos++] = '-';
        magnitude     = (uint32_t)(-(int64_t)value);
    } else {
        magnitude = (uint32_t)value;
    }

    written = _u32_to_ascii(&buffer[pos], buffer_size - pos, magnitude);
    if (written < 0) {
        return -1;
    }

    return (int)(pos + (size_t)written);
}

static int _format_channel_value(uint8_t index, char *buffer, size_t buffer_size)
{
    void               *ptr  = _vofa.channels[index].ptr;
    enum vofa_data_type type = _vofa.channels[index].type;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return -1;
    }

    if (ptr == NULL) {
        if (buffer_size < 2U) {
            return -1;
        }

        buffer[0] = '0';
        return 1;
    }

    switch (type) {
    case VOFA_FLOAT: {
        float value = 0.0f;
        memcpy(&value, ptr, sizeof(value));
        return _format_float(buffer, buffer_size, value);
    }
    case VOFA_INT8: {
        int8_t value = 0;
        memcpy(&value, ptr, sizeof(value));
        return _format_signed_ascii(buffer, buffer_size, (int32_t)value);
    }
    case VOFA_UINT8: {
        uint8_t value = 0U;
        memcpy(&value, ptr, sizeof(value));
        return _u32_to_ascii(buffer, buffer_size, (uint32_t)value);
    }
    case VOFA_INT16: {
        int16_t value = 0;
        memcpy(&value, ptr, sizeof(value));
        return _format_signed_ascii(buffer, buffer_size, (int32_t)value);
    }
    case VOFA_UINT16: {
        uint16_t value = 0U;
        memcpy(&value, ptr, sizeof(value));
        return _u32_to_ascii(buffer, buffer_size, (uint32_t)value);
    }
    case VOFA_INT32: {
        int32_t value = 0;
        memcpy(&value, ptr, sizeof(value));
        return _format_signed_ascii(buffer, buffer_size, value);
    }
    case VOFA_UINT32: {
        uint32_t value = 0U;
        memcpy(&value, ptr, sizeof(value));
        return _u32_to_ascii(buffer, buffer_size, value);
    }
    default:
        return -1;
    }
}
#endif

/* Protocol Implementations --------------------------------------------------------------------- */

#if (VOFA_PROTOCOL == VOFA_PROTO_JUSTFLOAT)

static void _process_justfloat(void)
{
    uint16_t offset = 0U;

    for (uint8_t i = 0U; i < _vofa.active_ch_num; i++) {
        float val = _read_channel(i);
        memcpy(&_vofa.jf_buf[offset], &val, sizeof(val));
        offset += (uint16_t)sizeof(val);
    }

    memcpy(&_vofa.jf_buf[offset], _justfloat_tail, VOFA_TAIL_SIZE);
    offset += VOFA_TAIL_SIZE;

    _vofa.send_callback(_vofa.jf_buf, offset);
}

#else

static const uint32_t _pow10_u32[] = {1U, 10U, 100U, 1000U, 10000U, 100000U};
static const float _pow10_f32[] = {1.0f, 10.0f, 100.0f, 1000.0f, 10000.0f, 100000.0f, 1000000.0f};

static int _u32_to_ascii(char *buffer, size_t buffer_size, uint32_t value)
{
    char   tmp[10];
    size_t len = 0U;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return -1;
    }

    do {
        tmp[len++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (len < sizeof(tmp)));

    if (len > buffer_size) {
        return -1;
    }

    for (size_t i = 0U; i < len; i++) {
        buffer[i] = tmp[len - 1U - i];
    }

    return (int)len;
}

static int _u32_to_ascii_padded(char *buffer, size_t buffer_size, uint32_t value, uint8_t width)
{
    char tmp[10];
    int  digits;

    if ((buffer == NULL) || (buffer_size < width)) {
        return -1;
    }

    digits = _u32_to_ascii(tmp, sizeof(tmp), value);
    if ((digits < 0) || ((uint8_t)digits > width)) {
        return -1;
    }

    for (uint8_t i = 0U; i < (uint8_t)(width - (uint8_t)digits); i++) {
        buffer[i] = '0';
    }

    memcpy(&buffer[width - (uint8_t)digits], tmp, (size_t)digits);
    return (int)width;
}

static int _format_fixed(char *buffer, size_t buffer_size, float abs_value, bool negative);
static int _format_scientific(char *buffer, size_t buffer_size, float abs_value, bool negative);

static int _format_fixed(char *buffer, size_t buffer_size, float abs_value, bool negative)
{
    size_t   pos        = 0U;
    uint8_t  int_digits = 1U;
    uint8_t  frac_digits;
    uint32_t scale;
    uint32_t scaled;
    uint32_t int_part;
    uint32_t frac_part;
    int      written;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return -1;
    }

    if (negative) {
        if (pos >= buffer_size) {
            return -1;
        }
        buffer[pos++] = '-';
    }

    while ((int_digits < (uint8_t)TM_ARRAY_SIZE(_pow10_f32)) &&
           (abs_value >= _pow10_f32[int_digits])) {
        int_digits++;
    }

    frac_digits = (int_digits < VOFA_SIGNIFICANT_DIGITS)
                      ? (uint8_t)(VOFA_SIGNIFICANT_DIGITS - int_digits)
                      : 0U;
    if (frac_digits > VOFA_FIXED_MAX_FRACTION_DIGITS) {
        frac_digits = VOFA_FIXED_MAX_FRACTION_DIGITS;
    }

    scale     = _pow10_u32[frac_digits];
    scaled    = (uint32_t)((abs_value * (float)scale) + 0.5f);
    int_part  = scaled / scale;
    frac_part = scaled % scale;

    if (int_part >= 1000000U) {
        return _format_scientific(buffer, buffer_size, abs_value, negative);
    }

    written = _u32_to_ascii(&buffer[pos], buffer_size - pos, int_part);
    if (written < 0) {
        return -1;
    }
    pos += (size_t)written;

    while ((frac_digits > 0U) && ((frac_part % 10U) == 0U)) {
        frac_part /= 10U;
        frac_digits--;
    }

    if (frac_digits > 0U) {
        if (pos >= buffer_size) {
            return -1;
        }
        buffer[pos++] = '.';

        written = _u32_to_ascii_padded(&buffer[pos], buffer_size - pos, frac_part, frac_digits);
        if (written < 0) {
            return -1;
        }
        pos += (size_t)written;
    }

    return (int)pos;
}

static int _format_scientific(char *buffer, size_t buffer_size, float abs_value, bool negative)
{
    size_t   pos        = 0U;
    float    norm       = abs_value;
    int      exponent10 = 0;
    uint32_t scaled;
    uint32_t int_part;
    uint32_t frac_part;
    uint8_t  frac_digits = VOFA_SCI_FRACTION_DIGITS;
    int      written;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return -1;
    }

    if (negative) {
        if (pos >= buffer_size) {
            return -1;
        }
        buffer[pos++] = '-';
    }

    while (norm >= 10.0f) {
        norm *= 0.1f;
        exponent10++;
    }

    while ((norm > 0.0f) && (norm < 1.0f)) {
        norm *= 10.0f;
        exponent10--;
    }

    scaled = (uint32_t)((norm * (float)_pow10_u32[VOFA_SCI_FRACTION_DIGITS]) + 0.5f);
    if (scaled >= 1000000U) {
        scaled = 100000U;
        exponent10++;
    }

    int_part  = scaled / _pow10_u32[VOFA_SCI_FRACTION_DIGITS];
    frac_part = scaled % _pow10_u32[VOFA_SCI_FRACTION_DIGITS];

    written = _u32_to_ascii(&buffer[pos], buffer_size - pos, int_part);
    if (written < 0) {
        return -1;
    }
    pos += (size_t)written;

    while ((frac_digits > 0U) && ((frac_part % 10U) == 0U)) {
        frac_part /= 10U;
        frac_digits--;
    }

    if (frac_digits > 0U) {
        if (pos >= buffer_size) {
            return -1;
        }
        buffer[pos++] = '.';

        written = _u32_to_ascii_padded(&buffer[pos], buffer_size - pos, frac_part, frac_digits);
        if (written < 0) {
            return -1;
        }
        pos += (size_t)written;
    }

    if ((pos + 2U) > buffer_size) {
        return -1;
    }
    buffer[pos++] = 'e';
    buffer[pos++] = (exponent10 < 0) ? '-' : '+';

    written = _u32_to_ascii(&buffer[pos],
                            buffer_size - pos,
                            (uint32_t)((exponent10 < 0) ? -exponent10 : exponent10));
    if (written < 0) {
        return -1;
    }
    pos += (size_t)written;

    return (int)pos;
}

static int _format_float(char *buffer, size_t buffer_size, float value)
{
    union {
        float    f32;
        uint32_t u32;
    } bits;
    bool  negative;
    float abs_value;

    bits.f32 = value;
    negative = ((bits.u32 >> 31) != 0U);
    bits.u32 &= 0x7FFFFFFFU;

    if ((bits.u32 & 0x7F800000U) == 0x7F800000U) {
        if ((bits.u32 & 0x007FFFFFU) != 0U) {
            if (buffer_size < 3U) {
                return -1;
            }
            memcpy(buffer, "nan", 3U);
            return 3;
        }

        if (negative) {
            if (buffer_size < 4U) {
                return -1;
            }
            memcpy(buffer, "-inf", 4U);
            return 4;
        }

        if (buffer_size < 3U) {
            return -1;
        }
        memcpy(buffer, "inf", 3U);
        return 3;
    }

    abs_value = bits.f32;
    if (abs_value == 0.0f) {
        if (buffer_size < 1U) {
            return -1;
        }
        buffer[0] = '0';
        return 1;
    }

    if ((abs_value >= 1000000.0f) || (abs_value < 0.001f)) {
        return _format_scientific(buffer, buffer_size, abs_value, negative);
    }

    return _format_fixed(buffer, buffer_size, abs_value, negative);
}

static void _process_firewater(void)
{
    char *fw_buf = (char *)_vofa.fw_buf;
    int   pos    = 0;

    fw_buf[pos++] = '$';

    for (uint8_t i = 0U; i < _vofa.active_ch_num; i++) {
        int n;

        if (pos >= (int)(VOFA_FW_BUF_SIZE - 4U)) {
            break;
        }

        n = _format_channel_value(i, &fw_buf[pos], (size_t)(VOFA_FW_BUF_SIZE - pos - 4));

        if ((n <= 0) || ((pos + n) >= (int)(VOFA_FW_BUF_SIZE - 3))) {
            break;
        }

        pos += n;
        if (i < (_vofa.active_ch_num - 1U)) {
            fw_buf[pos++] = ',';
        }
    }

    fw_buf[pos++] = ';';
    fw_buf[pos++] = '\r';
    fw_buf[pos++] = '\n';
    _vofa.send_callback(_vofa.fw_buf, (uint16_t)pos);
}

#endif

/* Public Functions ----------------------------------------------------------------------------- */

void vofa_init(void)
{
    memset(&_vofa, 0, sizeof(_vofa));
    _vofa.send_callback = _noop_send;
}

void vofa_bind(uint8_t idx, void *ptr, enum vofa_data_type type)
{
    if (idx >= VOFA_MAX_CHANNELS) {
        return;
    }

    _vofa.channels[idx].ptr  = ptr;
    _vofa.channels[idx].type = type;

    if ((uint8_t)(idx + 1U) > _vofa.active_ch_num) {
        _vofa.active_ch_num = (uint8_t)(idx + 1U);
    }
}

void vofa_unbind(uint8_t idx)
{
    if (idx >= VOFA_MAX_CHANNELS) {
        return;
    }

    _vofa.channels[idx].ptr  = NULL;
    _vofa.channels[idx].type = VOFA_UINT8;
    _vofa_refresh_active_channels();
}

void vofa_process_frame(void)
{
    if (_vofa.active_ch_num == 0U) {
        return;
    }

#if (VOFA_PROTOCOL == VOFA_PROTO_FIREWATER)
    _process_firewater();
#else
    /* Default to JustFloat for any non-FireWater compile-time value. */
    _process_justfloat();
#endif
}

void vofa_register_send_callback(vofa_send_callback_t callback)
{
    if (callback != NULL) {
        _vofa.send_callback = callback;
    }
}

uint8_t vofa_get_active_channel_count(void)
{
    return _vofa.active_ch_num;
}

const char *vofa_get_protocol_name(void)
{
#if (VOFA_PROTOCOL == VOFA_PROTO_FIREWATER)
    return "FireWater";
#else
    return "JustFloat";
#endif
}

/* ---------------------------------------- end of file ----------------------------------------- */
