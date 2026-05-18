/**
 * @file    bsp_init.c
 * @brief   Falcon FOC board pre-kernel initialization
 * @version v1.0.0
 * @date    2026.05.13
 *
 * Implementation role
 * -------------------
 * This file owns only the board-local pre-kernel glue that must exist before
 * the staged export pipeline and scheduler startup. Raw MCU bring-up remains
 * owned by `platform/at/at32f415/generated/src/main.c`.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "bsp_init.h"
#include "at32f415.h"
#include "core_time.h"

/* Public Functions ----------------------------------------------------------------------------- */

int bsp_init(void)
{
    return core_time_init(SystemCoreClock);
}

/* ---------------------------------------- end of file ----------------------------------------- */
