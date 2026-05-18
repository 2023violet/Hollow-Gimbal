/**
 * @file    bsp_init.h
 * @brief   Falcon FOC board pre-kernel initialization entry
 * @version v1.0.0
 * @date    2026.05.13
 *
 * Design overview
 * ---------------
 * `bsp_init()` is the board-owned pre-kernel bridge for the Falcon FOC image.
 * It must stay small and only host board glue that is required before staged
 * export execution and RTOS startup.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

int bsp_init(void);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
