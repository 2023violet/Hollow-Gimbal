/**
 * @file    foc_restore.h
 * @brief   FalconFoc parameter-restore finalization helpers
 * @version v1.0.0
 * @date    2026.05.14
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Rebind runtime-only derived state after one or more parameter groups were restored.
 *
 * This helper intentionally does not regenerate user-owned PID tuning. It only
 * refreshes runtime state that depends on already-restored persistent config.
 *
 * @param  motor  Motor instance.
 */
void foc_restore_finalize(struct motor_dev *motor);

/**
 * @brief Clear transient runtime setpoints after restore without clobbering persisted modes.
 *
 * This helper zeros live command targets and re-seeds hold positions so a
 * restarted axis does not resume stale motion. It must not overwrite the
 * persisted ctrl/input mode that was just restored from Flash.
 *
 * @param  motor  Motor instance.
 */
void foc_restore_clear_transient_setpoints(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
