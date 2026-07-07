/**
 * @file    test_foc_round_contract.c
 * @brief   Host regression tests for FalconFoc local round helpers
 * @version v1.0.0
 * @date    2026.05.20
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "foc_math.h"

#define TEST_ASSERT(expr)                                                                 \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            fprintf(stderr,                                                               \
                    "[test_foc_round_contract] assertion failed: %s (%s:%d)\n",          \
                    #expr,                                                                \
                    __FILE__,                                                             \
                    __LINE__);                                                            \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

static void test_foc_round_float_helper_matches_contract(void)
{
    float neg_zero = -0.0f;

    TEST_ASSERT(foc_round(0.0f) == 0.0f);
    TEST_ASSERT(foc_round(0.49f) == 0.0f);
    TEST_ASSERT(foc_round(0.5f) == 1.0f);
    TEST_ASSERT(foc_round(1.5f) == 2.0f);
    TEST_ASSERT(foc_round(-0.5f) == -1.0f);
    TEST_ASSERT(foc_round(-1.5f) == -2.0f);
    TEST_ASSERT(foc_round(neg_zero) == neg_zero);
    TEST_ASSERT(signbit(foc_round(neg_zero)) != 0);
}

static void test_foc_round_i32_matches_product_contract(void)
{
    TEST_ASSERT(foc_round_i32(0.0f) == 0);
    TEST_ASSERT(foc_round_i32(0.49f) == 0);
    TEST_ASSERT(foc_round_i32(0.5f) == 1);
    TEST_ASSERT(foc_round_i32(1.5f) == 2);
    TEST_ASSERT(foc_round_i32(-0.5f) == -1);
    TEST_ASSERT(foc_round_i32(-1.5f) == -2);
}

int main(void)
{
    test_foc_round_float_helper_matches_contract();
    test_foc_round_i32_matches_product_contract();

    puts("[test_foc_round_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
