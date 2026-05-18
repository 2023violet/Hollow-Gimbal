/**
 * @file    test_tm_format.c
 * @brief   Host-side tests for the Themis lightweight formatter
 * @version v1.0.0
 * @date    2026.03.28
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tm_format.h"

bool themis_test_tm_format_parse_star_width(const char *fmt,
                                            int         width_arg,
                                            unsigned int *width,
                                            bool        *left_align);
bool themis_test_tm_format_parse_literal_precision(const char *fmt, int *precision, char *type);

#define TEST_ASSERT(expr)                                              \
    do {                                                               \
        if (!(expr)) {                                                 \
            fprintf(stderr,                                            \
                    "[test_tm_format] assertion failed: %s (%s:%d)\n", \
                    #expr,                                             \
                    __FILE__,                                          \
                    __LINE__);                                         \
            exit(1);                                                   \
        }                                                              \
    } while (0)

static void test_tm_format_basic_integer_and_hex_output(void)
{
    char   buffer[96];
    size_t written;

    written = tm_format_snprintf(buffer,
                                 sizeof(buffer),
                                 "adc=%u hex=%02X low=%04x neg=%d long=%ld ulong=%lu",
                                 7U,
                                 0xABU,
                                 0x1FU,
                                 -42,
                                 -1234L,
                                 5678UL);

    TEST_ASSERT(written == strlen("adc=7 hex=AB low=001f neg=-42 long=-1234 ulong=5678"));
    TEST_ASSERT(strcmp(buffer, "adc=7 hex=AB low=001f neg=-42 long=-1234 ulong=5678") == 0);
}

static void test_tm_format_width_precision_and_alignment(void)
{
    char   buffer[96];
    size_t written;

    written =
        tm_format_snprintf(buffer, sizeof(buffer), "[%-9s][%.*s][%05d]", "heat", 3, "12345", -42);

    TEST_ASSERT(written == strlen("[heat     ][123][-0042]"));
    TEST_ASSERT(strcmp(buffer, "[heat     ][123][-0042]") == 0);
}

static void test_tm_format_pointer_and_percent(void)
{
    char        buffer[96];
    char        expected[96];
    void       *ptr     = (void *)(uintptr_t)0x1234U;
    const char *hex_fmt = (sizeof(void *) == 8U) ? "[%%][0x%016llx]" : "[%%][0x%08x]";
    size_t      written = tm_format_snprintf(buffer, sizeof(buffer), "[%%][%p]", ptr);

    if (sizeof(void *) == 8U) {
        (void)snprintf(expected, sizeof(expected), hex_fmt, (unsigned long long)(uintptr_t)ptr);
    } else {
        (void)snprintf(expected, sizeof(expected), hex_fmt, (unsigned int)(uintptr_t)ptr);
    }

    TEST_ASSERT(written == strlen(expected));
    TEST_ASSERT(strcmp(buffer, expected) == 0);
}

static void test_tm_format_long_long_paths_stay_correct(void)
{
    char               buffer[128];
    unsigned long long value        = 1234567890123456789ULL;
    long long          signed_value = -1234567890123456789LL;
    unsigned long long hex_value    = 0x123456789ABCDEF0ULL;
    size_t             written;

    written = tm_format_snprintf(buffer,
                                 sizeof(buffer),
                                 "u=%llu d=%lld x=%llX",
                                 value,
                                 signed_value,
                                 hex_value);

    TEST_ASSERT(written ==
                strlen("u=1234567890123456789 d=-1234567890123456789 x=123456789ABCDEF0"));
    TEST_ASSERT(strcmp(buffer, "u=1234567890123456789 d=-1234567890123456789 x=123456789ABCDEF0") ==
                0);
}

static void test_tm_format_truncation_matches_snprintf_style(void)
{
    char   buffer[8];
    size_t written;

    written = tm_format_snprintf(buffer, sizeof(buffer), "abcdefghi");

    TEST_ASSERT(written == 9U);
    TEST_ASSERT(strcmp(buffer, "abcdefg") == 0);
}

static void test_tm_format_int_min_dynamic_width_clamps_without_overflow(void)
{
    unsigned int width      = 0U;
    bool         left_align = false;

    TEST_ASSERT(themis_test_tm_format_parse_star_width("*d", INT_MIN, &width, &left_align));
    TEST_ASSERT(left_align);
    TEST_ASSERT(width == (unsigned int)INT_MAX);
}

static void test_tm_format_precision_parser_saturates_at_int_max(void)
{
    int  precision = -1;
    char type      = '\0';

    TEST_ASSERT(
        themis_test_tm_format_parse_literal_precision(".2147483648s", &precision, &type));
    TEST_ASSERT(type == 's');
    TEST_ASSERT(precision == INT_MAX);
}

int main(void)
{
    test_tm_format_basic_integer_and_hex_output();
    test_tm_format_width_precision_and_alignment();
    test_tm_format_pointer_and_percent();
    test_tm_format_long_long_paths_stay_correct();
    test_tm_format_truncation_matches_snprintf_style();
    test_tm_format_int_min_dynamic_width_clamps_without_overflow();
    test_tm_format_precision_parser_saturates_at_int_max();

    puts("[test_tm_format] passed");
    return 0;
}
