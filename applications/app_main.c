/**
 * @file    app_main.c
 * @brief   Falcon FOC staged boot bridge
 * @version v1.0.0
 * @date    2026.05.11
 *
 * Implementation role
 * -------------------
 * - bridges generated AT32 bring-up into the Themis staged export pipeline
 * - records boot-stage failure metadata for debugger inspection
 * - keeps BSP pre-kernel work, RTOS object-model init, and APP export startup
 *   in one explicit sequence
 *
 * Key collaborators
 * -----------------
 * - `bsp_init()`: board-owned pre-kernel glue
 * - `tm_export`: staged init hook executor
 * - `cmsis_os2`: RTOS object-model bootstrap and scheduler start
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_main.h"
#include "bsp_init.h"
#include "tm_export.h"
#include "cmsis_os2.h"
#include <stdint.h>

/* Private Macros ------------------------------------------------------------------------------- */

#define APP_BOOT_STAGE_IDLE         (0U)
#define APP_BOOT_STAGE_BSP_INIT     (1U)
#define APP_BOOT_STAGE_PRE_KERNEL   (2U)
#define APP_BOOT_STAGE_KERNEL_INIT  (3U)
#define APP_BOOT_STAGE_POST_KERNEL  (4U)
#define APP_BOOT_STAGE_KERNEL_START (5U)

/* Private State -------------------------------------------------------------------------------- */

static volatile uint32_t _app_boot_stage   = APP_BOOT_STAGE_IDLE;
static volatile int      _app_boot_status  = 0;
static const char *volatile _app_boot_hook = NULL;
static volatile uint8_t _app_boot_level    = 0U;
static volatile uint8_t _app_boot_prio     = 0U;

/* Private Function Prototypes ------------------------------------------------------------------ */

static void _app_boot_halt(uint32_t stage, int status);
static void _app_boot_capture_export_failure(void);
static void _app_boot_require(int status, uint32_t stage);

/* Public Functions ----------------------------------------------------------------------------- */

void app_main_bootstrap(void)
{
    osStatus_t kernel_status;

    _app_boot_require(bsp_init(), APP_BOOT_STAGE_BSP_INIT);
    _app_boot_require(export_init_execute_range(EXPORT_LEVEL_PRE_KERNEL_FIRST,
                                                EXPORT_LEVEL_PRE_KERNEL_LAST),
                      APP_BOOT_STAGE_PRE_KERNEL);

    kernel_status = osKernelInitialize();
    if (kernel_status != osOK) {
        _app_boot_halt(APP_BOOT_STAGE_KERNEL_INIT, (int)kernel_status);
    }

    _app_boot_require(export_init_execute_range(EXPORT_LEVEL_POST_KERNEL_FIRST, EXPORT_LEVEL_APP),
                      APP_BOOT_STAGE_POST_KERNEL);

    kernel_status = osKernelStart();
    _app_boot_halt(APP_BOOT_STAGE_KERNEL_START,
                   (kernel_status == osOK) ? -1 : (int)kernel_status);
}

/* Private Functions ---------------------------------------------------------------------------- */

static void _app_boot_halt(uint32_t stage, int status)
{
    _app_boot_stage  = stage;
    _app_boot_status = status;

    for (;;) {
    }
}

static void _app_boot_capture_export_failure(void)
{
    struct tm_export_last_error info;

    if (!export_init_last_error_get(&info)) {
        _app_boot_hook  = NULL;
        _app_boot_level = 0U;
        _app_boot_prio  = 0U;
        return;
    }

    _app_boot_hook  = info.name;
    _app_boot_level = info.level;
    _app_boot_prio  = info.priority;
}

static void _app_boot_require(int status, uint32_t stage)
{
    if (status != 0) {
        _app_boot_capture_export_failure();
        _app_boot_halt(stage, status);
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
