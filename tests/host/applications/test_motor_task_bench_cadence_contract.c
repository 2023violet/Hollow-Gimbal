/**
 * @file    test_motor_task_bench_cadence_contract.c
 * @brief   Host contract for the bench-only FOC cadence and time base
 */

#include <stdio.h>
#include <stdlib.h>

#include "foc_config.h"
#include "motor_task_config.h"

#define TEST_ASSERT(expr)                                                               \
    do {                                                                                \
        if (!(expr)) {                                                                  \
            fprintf(stderr,                                                             \
                    "[test_motor_task_bench_cadence_contract] assertion failed: %s\\n", \
                    #expr);                                                            \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

int main(void)
{
    TEST_ASSERT(TM_MOTOR_FOC_ADC_DIVIDER == 4U);
    TEST_ASSERT(CURR_LOOP_FREQ_HZ == 5000U);
    TEST_ASSERT(DEFAULT_SAMPLING_HZ == (float)CURR_LOOP_FREQ_HZ);
    TEST_ASSERT(CURR_LOOP_TS > 0.00019f && CURR_LOOP_TS < 0.00021f);
    TEST_ASSERT((TM_MOTOR_BENCH_RUN_BUDGET_MS == 50U) ||
                (TM_MOTOR_BENCH_RUN_BUDGET_MS == 200U) ||
                (TM_MOTOR_BENCH_RUN_BUDGET_MS == 400U));
    TEST_ASSERT(TM_MOTOR_BENCH_MAX_RUN_MS == TM_MOTOR_BENCH_RUN_BUDGET_MS);
    TEST_ASSERT(TM_MOTOR_BENCH_MAX_RUN_TICKS ==
                ((TM_MOTOR_BENCH_RUN_BUDGET_MS * TM_MOTOR_BENCH_ADC_TICK_HZ) / 1000U));

    TEST_ASSERT(TM_MOTOR_BENCH_OPEN_LOOP_MECH_RPM >= 20.0f);
    TEST_ASSERT(TM_MOTOR_BENCH_OPEN_LOOP_MECH_RPM <= 60.0f);
    TEST_ASSERT(TM_MOTOR_BENCH_OPEN_LOOP_WE_RAD_S > 20.0f);
    TEST_ASSERT(TM_MOTOR_BENCH_OPEN_LOOP_ELECTRICAL_PERIOD_MS > 250.0f);
    TEST_ASSERT(TM_MOTOR_BENCH_OPEN_LOOP_ELECTRICAL_PERIOD_MS < 300.0f);
    TEST_ASSERT(TM_MOTOR_BENCH_OPEN_LOOP_RUN_MS >=
                (uint32_t)(TM_MOTOR_BENCH_OPEN_LOOP_ELECTRICAL_PERIOD_MS + 100.0f));

    puts("[test_motor_task_bench_cadence_contract] passed");
    return 0;
}
