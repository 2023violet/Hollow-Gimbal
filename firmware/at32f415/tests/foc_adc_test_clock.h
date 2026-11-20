/**
 * @file    foc_adc_test_clock.h
 * @brief   Virtual timer observations shared by Board1 Host tests.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Design overview
 * ---------------
 * Generate synthetic sticky flags from an extended test clock.
 *
 * Usage example
 * -------------
 * Initialize the epoch and issue observations in increasing elapsed time.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Each test owns its clock. An observation represents a flag acknowledgement.
 *
 * Failure / recovery
 * ------------------
 * Tests can change a generated observation to inject a peripheral fault.
 */
#pragma once

#include "foc_board.h"

/* Public Types ---------------------------------------------------------------- */

struct foc_adc_test_clock {
    uint32_t epoch;
    uint64_t elapsed;
};

/* Public Functions ------------------------------------------------------------ */

static inline struct foc_board_adc_event foc_adc_test_event(struct foc_adc_test_clock *clock,
                                                            uint64_t elapsed)
{
    const uint64_t old_half = (clock->epoch + clock->elapsed) >> 31U;
    const uint64_t new_half = (clock->epoch + elapsed) >> 31U;
    unsigned int flags = 0U;
    if (new_half - old_half > 1U) {
        flags = FOC_BOARD_TIME_WRAP | FOC_BOARD_TIME_HALF;
    } else if (new_half != old_half) {
        flags = (new_half & 1U) != 0U ? FOC_BOARD_TIME_HALF : FOC_BOARD_TIME_WRAP;
    }
    if (elapsed / FOC_BOARD_H0_US != clock->elapsed / FOC_BOARD_H0_US) {
        flags |= FOC_BOARD_SLOT_WRAP;
    }
    clock->elapsed = elapsed;
    const uint32_t time = clock->epoch + (uint32_t)elapsed;
    return (struct foc_board_adc_event){
        .before_us = time,
        .now_us = time,
        .phase_ticks =
            (uint32_t)(elapsed % FOC_BOARD_H0_US) * (FOC_BOARD_TIMER_HZ / FOC_BOARD_US_HZ),
        .flags = flags,
        .coherent = true,
    };
}

/* ---------------------------------------- end of file ----------------------------------------- */
