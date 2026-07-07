/**
 * @file    foc_restore.h
 * @brief   FalconFoc parameter-restore finalization helpers
 * @version v1.0.0
 * @date    2026.05.21
 *
 * Design overview
 * ---------------
 * This header owns the final restore step that turns persisted parameters into
 * a live motor runtime snapshot without reintroducing transient setpoints.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void foc_restore_finalize(struct motor_dev *motor);
void foc_restore_clear_transient_setpoints(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
