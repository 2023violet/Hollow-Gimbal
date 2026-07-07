/**
 * @file    motor_fault_runtime.h
 * @brief   FalconFoc fault-policy runtime owner
 * @version v1.0.0
 * @date    2026.05.21
 *
 * Design overview
 * ---------------
 * This header owns the product-side fault policy wrapper. The hardware-coupled
 * FOC engines only provide measurements; runtime decides when a fault is
 * latched and how the axis is forced safe.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime Policy ------------------------------------------------------------------------ */

/**
 * @brief  Evaluate live motor measurements and latch FalconFoc product faults.
 */
void foc_fault_check(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
