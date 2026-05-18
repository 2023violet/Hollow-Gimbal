/**
 * @file    tm_atomic.h
 * @brief   Stable Themis-prefixed include for utils atomic primitives
 * @version v1.0.0
 * @date    2026.04.04
 *
 * Design overview
 * ---------------
 * `tm_atomic.h` is the stable Themis-prefixed entry point for the portable
 * atomic primitives defined in `atomic.h`. It exists purely to avoid basename
 * collisions when a build also pulls in third-party headers named
 * `atomic.h`.
 *
 * Usage example
 * -------------
 * tm_atomic_u32_t counter = TM_ATOMIC_U32_INIT(0U);
 * (void)tm_atomic_u32_fetch_add_relaxed(&counter, 1U);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - This wrapper owns no state and forwards every contract directly to
 *   `atomic.h`.
 * - Re-entrant, thread-safe, and ISR-safe exactly to the extent documented by
 *   the selected primitive in `atomic.h`.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no RTOS dependency.
 *
 * Failure / recovery
 * ------------------
 * - This wrapper adds no new runtime behavior; see `atomic.h` for per-symbol
 *   contract details.
 *
 * @note This header is an include-alias layer over specific compiler/C11
 *       atomic intrinsics. It does not define an independent implementation.
 */

#pragma once

#include "atomic.h"

/* ---------------------------------------- end of file ----------------------------------------- */
