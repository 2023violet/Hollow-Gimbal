/**
 * @file    foc_common.h
 * @brief   Host shim that reuses the live FalconFoc app-layer data model
 * @version v1.3.0
 * @date    2026.05.21
 */

#pragma once

/* Reuse the real app-layer owner map; host support fills in lower-level deps. */
#include "../../../applications/algo/foc_common.h"
