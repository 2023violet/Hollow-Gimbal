/**
 * @file    foc_board_supply_none.c
 * @brief   Default Board1 build: no calibration supply.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Implementation role
 * -------------------
 * The production default carries no external calibration input, so startup
 * reports NOMINAL_CONFIGURATION and CALIBRATION_NOT_AVAILABLE.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_boot.h"

#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

/* None: this translation unit only selects the absence of a supply. */

/* Public Functions ------------------------------------------------------------- */

const struct foc_board_calibration_supply *foc_board_calibration_supply(void)
{
    return NULL;
}

/* ---------------------------------------- end of file ----------------------------------------- */
