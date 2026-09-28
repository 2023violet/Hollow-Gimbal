/**
 * @file    test_foc_vbus_guard_contract.c
 * @brief   Host contract tests for VBUS numeric guards
 * @version v1.0.0
 * @date    2026.08.27
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../../applications/periph/foc_ctrl.h"

#define TEST_ASSERT(expr)                                                     \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                    \
                    "[test_foc_vbus_guard_contract] assertion failed: %s\n", \
                    #expr);                                                   \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

int main(void)
{
    TEST_ASSERT(foc_vbus_numeric_valid(12.0f));
    TEST_ASSERT(!foc_vbus_numeric_valid(FOC_VBUS_NUMERIC_MIN_V));
    TEST_ASSERT(!foc_vbus_numeric_valid(0.0f));
    TEST_ASSERT(!foc_vbus_numeric_valid(-1.0f));
    TEST_ASSERT(!foc_vbus_numeric_valid(NAN));
    TEST_ASSERT(!foc_vbus_numeric_valid(INFINITY));

    puts("[test_foc_vbus_guard_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
