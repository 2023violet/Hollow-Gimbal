/**
 * @file    test_app_shell_common_contract.c
 * @brief   Host contract tests for FalconFoc shell decimal parsing
 * @version v1.0.0
 * @date    2026.05.19
 */

#include <stdio.h>
#include <stdlib.h>

#include "app_shell_common.h"

#define TEST_ASSERT(expr)                                                             \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr,                                                           \
                    "[test_app_shell_common_contract] assertion failed: %s (%s:%d)\n", \
                    #expr,                                                            \
                    __FILE__,                                                         \
                    __LINE__);                                                        \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

static float test_absf(float value)
{
    return (value < 0.0f) ? -value : value;
}

static void test_parse_accepts_decimal_surface(void)
{
    float value = 0.0f;

    TEST_ASSERT(app_shell_parse_float_arg("123", &value) == 0);
    TEST_ASSERT(test_absf(value - 123.0f) < 0.0001f);

    TEST_ASSERT(app_shell_parse_float_arg("-123", &value) == 0);
    TEST_ASSERT(test_absf(value + 123.0f) < 0.0001f);

    TEST_ASSERT(app_shell_parse_float_arg("12.34", &value) == 0);
    TEST_ASSERT(test_absf(value - 12.34f) < 0.0001f);

    TEST_ASSERT(app_shell_parse_float_arg("-0.125", &value) == 0);
    TEST_ASSERT(test_absf(value + 0.125f) < 0.0001f);

    TEST_ASSERT(app_shell_parse_float_arg("  \t+7.5  ", &value) == 0);
    TEST_ASSERT(test_absf(value - 7.5f) < 0.0001f);
}

static void test_parse_rejects_non_decimal_or_garbage_forms(void)
{
    float value = 0.0f;

    TEST_ASSERT(app_shell_parse_float_arg("12.34rpm", &value) != 0);
    TEST_ASSERT(app_shell_parse_float_arg("0.1234567", &value) != 0);
    TEST_ASSERT(app_shell_parse_float_arg("1e3", &value) != 0);
    TEST_ASSERT(app_shell_parse_float_arg("nan", &value) != 0);
    TEST_ASSERT(app_shell_parse_float_arg("inf", &value) != 0);
    TEST_ASSERT(app_shell_parse_float_arg("0x1.8", &value) != 0);
}

int main(void)
{
    test_parse_accepts_decimal_surface();
    test_parse_rejects_non_decimal_or_garbage_forms();

    puts("[test_app_shell_common_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
