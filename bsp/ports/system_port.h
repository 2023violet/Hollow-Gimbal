/**
 * @file    system_port.h
 * @brief   BSP thin system-control port
 * @version v1.0.0
 * @date    2026.05.14
 *
 * Design overview
 * ---------------
 * This header exposes only the board-level system action that application
 * control-plane code is allowed to request.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void system_port_reset(void);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
