/**
 * @file    bsp_system.c
 * @brief   FalconFoc thin system-control BSP port
 * @version v1.1.0
 * @date    2026.06.02
 *
 * Implementation role
 * -------------------
 * This file owns the southbound MCU reset action for FalconFoc control-plane
 * code. Vendor and CMSIS reset details stay local to this `.c` so the public
 * BSP contract remains northbound and board-focused.
 *
 * Key collaborators
 * -----------------
 * - `bsp_system.h`: public reset contract
 * - `tm_runtime.h`: shared execution-context detection
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "bsp_system.h"
#include <errno.h>
#include "tm_runtime.h"

#if !defined(THEMIS_HOST_TEST)
#ifndef CMSIS_device_header
#error "bsp_system.c requires CMSIS_device_header from platform compile definitions."
#endif

#include CMSIS_device_header
#endif

/* Private Function Prototypes ------------------------------------------------------------------ */

static int _bsp_system_reset_context_is_valid(void);

/* Public Functions ----------------------------------------------------------------------------- */

int bsp_system_reset(void)
{
    int rc = _bsp_system_reset_context_is_valid();

    if (rc != 0) {
        return rc;
    }

#if !defined(THEMIS_HOST_TEST)
    NVIC_SystemReset();
#endif

    return 0;
}

/* Private Functions ---------------------------------------------------------------------------- */

static int _bsp_system_reset_context_is_valid(void)
{
    if (tm_is_in_isr()) {
        return -EPERM;
    }

    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
