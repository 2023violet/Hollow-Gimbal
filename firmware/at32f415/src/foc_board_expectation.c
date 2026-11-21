/**
 * @file    foc_board_expectation.c
 * @brief   Build-configured calibration expectations, independent of any supply payload.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Implementation role
 * -------------------
 * CMake passes the accepted supply identity and the expected motor pole pairs.
 * Startup compares the supply against these values; the supply cannot set them.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_boot.h"

/* Private Macros --------------------------------------------------------------- */

#if !defined(FOC_BOARD_EXPECTED_SUPPLY_ID) || !defined(FOC_BOARD_EXPECTED_POLE_PAIRS)
#error "The build must define FOC_BOARD_EXPECTED_SUPPLY_ID and FOC_BOARD_EXPECTED_POLE_PAIRS"
#endif

_Static_assert(FOC_BOARD_EXPECTED_POLE_PAIRS > 0, "expected pole pairs");
_Static_assert(FOC_BOARD_EXPECTED_POLE_PAIRS <= FOC_BOARD_POLE_PAIRS_MAX, "expected pole pairs");

/* Private Helpers -------------------------------------------------------------- */

/* None: the values are compile-time configuration. */

/* Public Functions ------------------------------------------------------------- */

struct foc_board_calibration_expectation foc_board_calibration_expectation(void)
{
    return (struct foc_board_calibration_expectation){
        .supply_id = (uint32_t)(FOC_BOARD_EXPECTED_SUPPLY_ID),
        .pole_pairs = (uint16_t)(FOC_BOARD_EXPECTED_POLE_PAIRS),
    };
}

/* ---------------------------------------- end of file ----------------------------------------- */
