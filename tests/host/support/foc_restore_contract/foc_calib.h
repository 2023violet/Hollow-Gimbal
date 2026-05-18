/**
 * @file    foc_calib.h
 * @brief   Narrow host shim for foc_restore contract coverage
 * @version v1.0.0
 * @date    2026.05.14
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct motor_dev;

void foc_calib_runtime_refresh(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif
