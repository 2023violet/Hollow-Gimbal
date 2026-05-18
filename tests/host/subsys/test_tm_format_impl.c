/**
 * @file    test_tm_format_impl.c
 * @brief   Test-local compile unit wrapper for tm_format.c
 * @version v1.0.0
 * @date    2026.03.28
 */

#include "../../../subsys/format/tm_format.c"

static bool themis_test_tm_format_parse_probe(const char              *fmt,
                                              struct tm_format_spec   *spec,
                                              ...)
{
    va_list     args;
    const char *cursor = fmt;
    bool        ok;

    va_start(args, spec);
    ok = _tm_format_parse_spec(&cursor, spec, &args);
    va_end(args);
    return ok;
}

bool themis_test_tm_format_parse_star_width(const char *fmt,
                                            int         width_arg,
                                            unsigned int *width,
                                            bool        *left_align)
{
    struct tm_format_spec spec = {0};
    bool                  ok;

    if ((fmt == NULL) || (width == NULL) || (left_align == NULL)) {
        return false;
    }

    ok = themis_test_tm_format_parse_probe(fmt, &spec, width_arg);
    if (!ok) {
        return false;
    }

    *width      = spec.width;
    *left_align = spec.left_align;
    return true;
}

bool themis_test_tm_format_parse_literal_precision(const char *fmt, int *precision, char *type)
{
    struct tm_format_spec spec = {0};
    bool                  ok;

    if ((fmt == NULL) || (precision == NULL) || (type == NULL)) {
        return false;
    }

    ok = themis_test_tm_format_parse_probe(fmt, &spec, 0);
    if (!ok) {
        return false;
    }

    *precision = spec.precision;
    *type      = spec.type;
    return true;
}
