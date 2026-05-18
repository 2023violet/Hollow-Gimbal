/**
 * @file    tmdef.h
 * @brief   Host forwarder for the canonical Themis low-level macro surface
 * @version v1.1.0
 * @date    2026.05.14
 *
 * Host-test include paths put `tests/host/support/` before the public
 * `include/` root. Keep this file as a thin forwarder so host builds resolve
 * the same `tmdef.h` contract as firmware code instead of a partial shim.
 */

#pragma once

#include "../../../include/tmdef.h"

/* ---------------------------------------- end of file ----------------------------------------- */
