/**
 * @file    system_port.c
 * @brief   BSP thin system-control port
 * @version v1.0.0
 * @date    2026.05.14
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "system_port.h"

#if !defined(THEMIS_HOST_TEST)
#include "at32f415.h"
#endif

/* Public Functions ----------------------------------------------------------------------------- */

void system_port_reset(void)
{
#if !defined(THEMIS_HOST_TEST)
    NVIC_SystemReset();
#endif
}

/* ---------------------------------------- end of file ----------------------------------------- */
