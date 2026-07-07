/**
 * @file    app_main.h
 * @brief   Falcon FOC application bootstrap entry
 * @version v1.0.0
 * @date    2026.05.11
 *
 * Design overview
 * ---------------
 * `app_main_bootstrap()` is the application-owned bridge from the generated
 * AT32 platform entry into the formal Themis staged boot pipeline used by the
 * Falcon FOC image.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Single caller: `platform/at/at32f23/generated/src/main.c`
 * - Called exactly once after generated low-level bring-up
 * - Runs before the scheduler starts
 * - Never returns on failure
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

void app_main_bootstrap(void);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
